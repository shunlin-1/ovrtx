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
bool write_sidecar(const SidecarConfig& cfg) {
    // Clipping range expressed in *scene units* — keeps the camera
    // sensible whether the source authored in mm, cm, or m.
    double near_v = 0.001    / cfg.meters_per_unit;
    double far_v  = 100000.0 / cfg.meters_per_unit;

    std::ostringstream ss;
    ss.imbue(std::locale::classic());  // no comma decimal separators
    ss << "#usda 1.0\n"
       << "(\n"
       << "    defaultPrim = \"World\"\n"
       << "    upAxis = \"" << cfg.up_axis << "\"\n"
       << "    metersPerUnit = " << cfg.meters_per_unit << "\n"
       << ")\n\n"
       << "def Xform \"Root\" (\n"
       << "    references = @" << cfg.scene_usd_path << "@\n"
       << ")\n"
       << "{\n}\n\n"
       << "def Xform \"World\"\n"
       << "{\n"
       << "    def Camera \"Camera\"\n"
       << "    {\n"
       << "        token projection = \"perspective\"\n"
       << "        float focalLength = "        << cfg.focal_length         << "\n"
       << "        float horizontalAperture = " << cfg.horizontal_aperture  << "\n"
       << "        float verticalAperture = "   << cfg.vertical_aperture    << "\n"
       << "        float2 clippingRange = (" << near_v << ", " << far_v << ")\n"
       << "        custom matrix4d omni:xform = (\n"
       << "            (1, 0, 0, 0),\n"
       << "            (0, 1, 0, 0),\n"
       << "            (0, 0, 1, 0),\n"
       << "            (0, 0, 4, 1)\n"
       << "        )\n"
       << "        uniform token[] xformOpOrder = [\"omni:xform\"]\n"
       << "    }\n"
       << "}\n\n"
       << "def \"Render\"\n"
       << "{\n"
       << "    def RenderProduct \"Camera\"\n"
       << "    {\n"
       << "        rel camera = </World/Camera>\n"
       << "        rel orderedVars = </Render/Camera/LdrColor>\n"
       << "        int2 resolution = (" << cfg.width << ", " << cfg.height << ")\n"
       << "        custom token omni:rtx:rendermode = \"" << cfg.rendermode << "\"\n\n"
       << "        def RenderVar \"LdrColor\"\n"
       << "        {\n"
       << "            string sourceName = \"LdrColor\"\n"
       << "        }\n"
       << "    }\n\n"
       << "    def RenderSettings \"Settings\"\n"
       << "    {\n"
       << "        rel products = </Render/Camera>\n"
       << "        custom token omni:rtx:rendermode = \"" << cfg.rendermode << "\"\n"
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
