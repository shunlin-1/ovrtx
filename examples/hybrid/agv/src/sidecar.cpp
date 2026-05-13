// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#include "sidecar.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>

namespace agv {

StageMetadata parse_stage_metadata(const std::string& usd_path) {
    StageMetadata md{};
    std::ifstream in(usd_path, std::ios::binary);
    if (!in) return md;

    // Top-level metadata lives in the first few hundred bytes; cap the
    // read to stay O(1) on multi-MB ASCII USDA files.
    constexpr std::size_t kHeadBytes = 4096;
    std::string head(kHeadBytes, '\0');
    in.read(head.data(), kHeadBytes);
    head.resize(static_cast<std::size_t>(in.gcount()));

    std::smatch m;
    std::regex up_re(R"(upAxis\s*=\s*\"([YZ])\")");
    if (std::regex_search(head, m, up_re)) {
        md.up_axis = m[1].str()[0];
    }
    std::regex mpu_re(R"(metersPerUnit\s*=\s*([0-9.eE+\-]+))");
    if (std::regex_search(head, m, mpu_re)) {
        try { md.meters_per_unit = std::stod(m[1].str()); }
        catch (...) {}
    }
    return md;
}

// [snippet:sidecar-writer]
//
// Sidecar layout (subLayers composition, not references):
//
//   subLayers = [@./<scene>.usda@]      ← pulls IN scene's full top-level
//                                         tree (World, Environment, Render),
//                                         no defaultPrim filter
//   def Camera "AgvCamera" { ... }       ← top-level camera, apiSchemas to
//                                         register as a renderable sensor
//   def "Render"/...                     ← merges with scene's /Render,
//                                         adds /AgvViewport under the
//                                         canonical kit-sensor path
//                                         /Render/OmniverseKit/HydraTextures
bool write_sidecar(const SidecarConfig& cfg) {
    // Clipping range expressed in *scene units* — keeps the camera
    // sensible whether the source authored in mm, cm, or m.
    double near_v = 0.001    / cfg.meters_per_unit;
    double far_v  = 100000.0 / cfg.meters_per_unit;

    // subLayers paths are resolved relative to the sidecar's location.
    // Sidecar is written into the same directory as the scene file
    // (see main.cpp), so a simple `./<basename>` works.
    std::string scene_basename;
    {
        auto pos = cfg.scene_usd_path.find_last_of("/\\");
        scene_basename = (pos == std::string::npos)
            ? cfg.scene_usd_path
            : cfg.scene_usd_path.substr(pos + 1);
    }

    std::ostringstream ss;
    ss.imbue(std::locale::classic());  // no comma decimal separators
    ss << "#usda 1.0\n"
       << "(\n"
       << "    defaultPrim = \"AgvCamera\"\n"
       << "    upAxis = \"" << cfg.up_axis << "\"\n"
       << "    metersPerUnit = " << cfg.meters_per_unit << "\n"
       << "    subLayers = [\n"
       << "        @./" << scene_basename << "@\n"
       << "    ]\n"
       << ")\n\n"
       // Top-level camera — kit-sensor-recognised via apiSchemas.
       << "def Camera \"AgvCamera\" (\n"
       << "    prepend apiSchemas = [\"OmniRtxCameraAutoExposureAPI_1\", \"OmniRtxCameraExposureAPI_1\"]\n"
       << ")\n"
       << "{\n"
       << "    token projection = \"perspective\"\n"
       << "    float focalLength = "        << cfg.focal_length         << "\n"
       << "    float horizontalAperture = " << cfg.horizontal_aperture  << "\n"
       << "    float verticalAperture = "   << cfg.vertical_aperture    << "\n"
       << "    float2 clippingRange = (" << near_v << ", " << far_v << ")\n"
       << "    custom matrix4d omni:xform = (\n"
       << "        (1, 0, 0, 0),\n"
       << "        (0, 1, 0, 0),\n"
       << "        (0, 0, 1, 0),\n"
       << "        (0, 0, 4, 1)\n"
       << "    )\n"
       << "    uniform token[] xformOpOrder = [\"omni:xform\"]\n"
       << "    float exposure:fStop = 5\n"
       << "    float exposure:responsivity = 1\n"
       << "    float exposure:time = 0.02\n"
       << "}\n\n"
       // Render product at the canonical kit-sensor scope.
       << "def \"Render\"\n"
       << "{\n"
       << "    def \"OmniverseKit\"\n"
       << "    {\n"
       << "        def \"HydraTextures\"\n"
       << "        {\n"
       << "            def RenderProduct \"AgvViewport\" (\n"
       // OmniRtxPostBloomPhysicalAPI_1 is the apiSchema that unlocks
       // the omni:rtx:post:bloom:* attribute slot on this RenderProduct.
       // Without it ovrtx rejects the bloom attributes (causing
       // "Couldn't find sensor prim associated with the render product"
       // and "Invalid USD RenderProduct Prim"). With it, the bloom
       // post-process actually runs at render time.
       << "                prepend apiSchemas = [\"OmniRtxSettingsCommonAdvancedAPI_1\", \"OmniRtxSettingsRtAdvancedAPI_1\", \"OmniRtxSettingsPtAdvancedAPI_1\", \"OmniRtxPostBloomPhysicalAPI_1\"]\n"
       << "            )\n"
       << "            {\n"
       << "                rel camera = </AgvCamera>\n"
       << "                rel orderedVars = </Render/Vars/LdrColor>\n"
       << "                uniform int2 resolution = (" << cfg.width << ", " << cfg.height << ")\n"
       << "                custom token omni:rtx:rendermode = \"" << cfg.rendermode << "\"\n"
       << "                token omni:rtx:background:source:type = \"domeLight\"\n"
       // Auto-exposure: tonemapper adapts to scene brightness so an
       // emissive cube doesn't clip the whole image to white. Same knob
       // the planet-system example uses. Without this, the neon mode's
       // bright emissive surface dominates the tonemap and everything
       // around it goes black (or the cube itself turns pure white).
       << "                bool omni:rtx:autoExposure:enabled = 1\n"
       // ── Physical bloom (ovrtx-native) ───────────────────────────────
       // Copied from examples/python/agv/Test.usda's working
       // RenderProduct, with OmniRtxPostBloomPhysicalAPI_1 added to
       // apiSchemas above. Models camera-lens bloom physically:
       //
       //   enabled           — on/off
       //   scale             — overall halo brightness (0.0–1.0).
       //                       0.3 is subtle, 1.0 is dramatic.
       //   fStop             — aperture stop. LOWER (more negative) =
       //                       WIDER halo. -68 = very wide (Test.usda
       //                       value). -20 = moderate. 0 = tight.
       //   cutoff            — per-RGB HDR radiance threshold above
       //                       which a pixel contributes bloom. Pre-
       //                       tonemap, so values are in scene radiance
       //                       units (thousands). (4000, 3000, 1500)
       //                       = trigger only on very bright pixels.
       //   cutoffFuzziness   — soft-threshold falloff (0 = hard, 1 =
       //                       smooth). 1 looks more natural.
       //   aperture:blades   — number of polygon sides for the lens-
       //                       star streak shape. 6 = hexagonal,
       //                       10 = nearly circular.
       << "                bool omni:rtx:post:bloom:enabled = 1\n"
       << "                float omni:rtx:post:bloom:scale = 0.8\n"
       << "                float omni:rtx:post:bloom:fStop = -68\n"
       << "                float3 omni:rtx:post:bloom:cutoff = (4000, 3000, 1500)\n"
       << "                float omni:rtx:post:bloom:cutoffFuzziness = 1\n"
       << "                int omni:rtx:post:bloom:aperture:blades = 6\n"
       << "            }\n"
       << "        }\n"
       << "    }\n"
       << "    def \"Vars\"\n"
       << "    {\n"
       << "        def RenderVar \"LdrColor\"\n"
       << "        {\n"
       << "            uniform string sourceName = \"LdrColor\"\n"
       << "            uniform token sourceType = \"raw\"\n"
       << "        }\n"
       << "    }\n"
       << "}\n";

    std::ofstream out(cfg.out_path, std::ios::binary);
    if (!out) return false;
    const std::string body = ss.str();
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return out.good();
}
// [/snippet:sidecar-writer]

}  // namespace agv
