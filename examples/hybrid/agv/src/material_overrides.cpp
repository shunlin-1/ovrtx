// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#include "material_overrides.h"

#include <ovrtx/ovrtx.h>
#include <ovrtx/ovrtx_types.h>

#include <cstdio>
#include <cstring>

namespace agv {

namespace {

// Mode presets — match examples/python/agv/main.py constants.
constexpr float kNeonColor[3]   = {0.30f, 0.90f, 1.00f};
constexpr float kNeonIntensity  = 10000.0f;

constexpr float kXrayOpacity    = 0.15f;

constexpr float kXrayLightColor[3]    = {0.30f, 0.90f, 1.00f};
constexpr float kXrayLightOpacity     = 0.40f;
constexpr float kXrayLightIntensity   = 1500.0f;

template <typename ResultT>
bool check(ResultT const& r, const char* op) {
    if (r.status != OVRTX_API_ERROR) return false;
    ovx_string_t e = ovrtx_get_last_error();
    if (e.ptr && e.length > 0) {
        std::fprintf(stderr, "ovrtx %s failed: %.*s\n",
                     op, int(e.length), e.ptr);
    } else {
        std::fprintf(stderr, "ovrtx %s failed\n", op);
    }
    return true;
}

// Wrapper around ovrtx_write_attribute for a single primitive type.
// `lanes` packs the components (3 for color, 1 for scalar) — mirrors
// the dtype lanes convention used by the C examples.
bool write_inline(ovrtx_renderer_t* renderer,
                  const std::string& prim_path,
                  const char* attr_name,
                  uint8_t dtype_code, uint8_t dtype_bits, uint16_t lanes,
                  void* data,
                  ovrtx_attribute_semantic_t semantic = OVRTX_SEMANTIC_NONE) {
    ovx_string_t pp = {prim_path.c_str(), prim_path.size()};
    ovrtx_prim_list_t prim_list{};
    prim_list.prim_paths = &pp;
    prim_list.num_paths = 1;

    ovrtx_attribute_type_t at{};
    at.dtype = {dtype_code, dtype_bits, lanes};
    at.is_array = false;
    at.semantic = semantic;

    ovrtx_binding_desc_t b{};
    b.prim_list = prim_list;
    b.attribute_name.string = {attr_name, std::strlen(attr_name)};
    b.attribute_type = at;
    b.prim_mode = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
    b.flags = OVRTX_BINDING_FLAG_NONE;

    ovrtx_binding_desc_or_handle_t boh{};
    boh.binding_desc = b;

    DLTensor t{};
    t.data = data;
    t.device = {kDLCPU, 0};
    t.ndim = 1;
    int64_t shape[1] = {1};
    t.shape = shape;
    t.dtype = {dtype_code, dtype_bits, lanes};

    ovrtx_input_buffer_t in_buf{};
    in_buf.tensors = &t;
    in_buf.tensor_count = 1;

    auto wr = ovrtx_write_attribute(renderer, &boh, &in_buf,
                                    OVRTX_DATA_ACCESS_SYNC);
    return !check(wr, attr_name);
}

}  // namespace

ShaderOverride::Mode mode_from_string(const std::string& s) {
    if (s == "neon")       return ShaderOverride::Mode::Neon;
    if (s == "xray")       return ShaderOverride::Mode::Xray;
    if (s == "xray-light") return ShaderOverride::Mode::XrayLight;
    return ShaderOverride::Mode::None;
}

static const char* mode_name(ShaderOverride::Mode m) {
    switch (m) {
    case ShaderOverride::Mode::Neon:      return "neon";
    case ShaderOverride::Mode::Xray:      return "xray";
    case ShaderOverride::Mode::XrayLight: return "xray-light";
    case ShaderOverride::Mode::None:      return "off";
    }
    return "?";
}

MaterialOverrides::MaterialOverrides(ovrtx_renderer_t* renderer)
    : renderer_(renderer) {}

void MaterialOverrides::write_emissive(const std::string& shader_path,
                                        const float color[3],
                                        float intensity) {
    float c[3] = {color[0], color[1], color[2]};
    write_inline(renderer_, shader_path, "inputs:emissive_color",
                 kDLFloat, 32, 3, c);
    float i = intensity;
    write_inline(renderer_, shader_path, "inputs:emissive_intensity",
                 kDLFloat, 32, 1, &i);
    // enable_emission is bool; ovrtx accepts uint8 underneath.
    std::uint8_t flag = intensity > 0.0f ? 1 : 0;
    write_inline(renderer_, shader_path, "inputs:enable_emission",
                 kDLUInt, 8, 1, &flag);
}

void MaterialOverrides::write_opacity(const std::string& shader_path,
                                       float opacity) {
    float o = opacity;
    write_inline(renderer_, shader_path, "inputs:opacity_constant",
                 kDLFloat, 32, 1, &o);
    std::uint8_t flag = opacity < 1.0f ? 1 : 0;
    write_inline(renderer_, shader_path, "inputs:enable_opacity",
                 kDLUInt, 8, 1, &flag);
}

void MaterialOverrides::restore(const PickEntry& entry) {
    write_emissive(entry.shader_path,
                   entry.orig_color.data(),
                   entry.orig_intensity);
    write_opacity(entry.shader_path, 1.0f);
}

// [snippet:material-toggle]
void MaterialOverrides::apply(const PickEntry& entry,
                              ShaderOverride::Mode requested) {
    auto& slot = state_[entry.shader_path];
    if (slot.pi == nullptr) slot.pi = &entry;

    // Same shader, same mode -> toggle off.
    if (slot.active == requested) {
        restore(entry);
        slot.active = ShaderOverride::Mode::None;
        std::fprintf(stderr, "[pick] -> off (%s)\n",
                     entry.shader_path.c_str());
        return;
    }

    // Mode swap: restore first so opacity/emission don't accumulate.
    if (slot.active != ShaderOverride::Mode::None) {
        restore(entry);
    }

    switch (requested) {
    case ShaderOverride::Mode::Neon:
        write_emissive(entry.shader_path, kNeonColor, kNeonIntensity);
        break;
    case ShaderOverride::Mode::Xray:
        write_opacity(entry.shader_path, kXrayOpacity);
        break;
    case ShaderOverride::Mode::XrayLight:
        write_opacity(entry.shader_path, kXrayLightOpacity);
        write_emissive(entry.shader_path,
                       kXrayLightColor, kXrayLightIntensity);
        break;
    case ShaderOverride::Mode::None:
        break;
    }
    slot.active = requested;
    std::fprintf(stderr, "[pick] -> %s (%s)\n",
                 mode_name(requested), entry.shader_path.c_str());
}
// [/snippet:material-toggle]

}  // namespace agv
