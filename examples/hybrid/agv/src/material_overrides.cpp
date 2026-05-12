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

// Batched float-attribute write: ONE ovrtx call, N prims, ONE value
// per prim (the value is broadcast to all of them). Used by the global
// X-ray Neon slider where every material gets the same opacity etc.
//
// `lanes` packs the components (3 for color, 1 for scalar).
bool write_inline_broadcast(ovrtx_renderer_t* renderer,
                            const std::vector<ovx_string_t>& prim_paths,
                            const char* attr_name,
                            uint8_t dtype_code, uint8_t dtype_bits,
                            uint16_t lanes, void* data) {
    if (prim_paths.empty()) return true;

    ovrtx_prim_list_t prim_list{};
    prim_list.prim_paths = const_cast<ovx_string_t*>(prim_paths.data());
    prim_list.num_paths = prim_paths.size();

    ovrtx_attribute_type_t at{};
    at.dtype = {dtype_code, dtype_bits, lanes};
    at.is_array = false;

    ovrtx_binding_desc_t b{};
    b.prim_list = prim_list;
    b.attribute_name.string = {attr_name, std::strlen(attr_name)};
    b.attribute_type = at;
    b.prim_mode = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
    b.flags = OVRTX_BINDING_FLAG_NONE;

    ovrtx_binding_desc_or_handle_t boh{};
    boh.binding_desc = b;

    // One tensor — broadcast across all prims in prim_list (ovrtx's
    // semantics when tensor_count == 1 and num_paths > 1).
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

bool apply_global_xray_neon(ovrtx_renderer_t* renderer,
                            const std::vector<std::string>& shader_paths,
                            const std::vector<std::array<float, 3>>& orig_colors,
                            const std::vector<float>& orig_intensities,
                            double v) {
    if (shader_paths.empty()) return true;

    // Build prim_list once — reused across the 4 batched attribute writes.
    std::vector<ovx_string_t> paths;
    paths.reserve(shader_paths.size());
    for (const auto& s : shader_paths) paths.push_back({s.c_str(), s.size()});

    // ovrtx_write_attribute doesn't broadcast a single tensor across
    // multiple prims — passing num_paths=N + tensor_count=1 writes
    // only the first prim. So we iterate: one write per shader per
    // attribute. Slow on paper (N×5 calls per slider edit) but only
    // fires on slider change, not per frame.

    if (v <= 0.0) {
        // Restore — each shader gets its own original color + intensity.
        for (std::size_t i = 0; i < shader_paths.size(); ++i) {
            float c[3] = {orig_colors[i][0], orig_colors[i][1], orig_colors[i][2]};
            write_inline(renderer, shader_paths[i], "inputs:emissive_color",
                         kDLFloat, 32, 3, c);
            float intensity = orig_intensities[i];
            write_inline(renderer, shader_paths[i], "inputs:emissive_intensity",
                         kDLFloat, 32, 1, &intensity);
            std::uint8_t emit_flag = intensity > 0.0f ? 1 : 0;
            write_inline(renderer, shader_paths[i], "inputs:enable_emission",
                         kDLUInt, 8, 1, &emit_flag);

            float opacity_one = 1.0f;
            write_inline(renderer, shader_paths[i], "inputs:opacity_constant",
                         kDLFloat, 32, 1, &opacity_one);
            std::uint8_t opac_off = 0;
            write_inline(renderer, shader_paths[i], "inputs:enable_opacity",
                         kDLUInt, 8, 1, &opac_off);
        }
        return true;
    }

    // Apply: linear interpolate opacity from 1.0 → 0.10 (very translucent
    // so the camera can actually see behind) and emission from 0 → 1200
    // (modest cyan rim — bright enough to outline shapes, dim enough not
    // to obscure depth perception). Color is cyan throughout.
    float opacity = 1.0f - 0.90f * float(v);
    float intensity = 1200.0f * float(v);
    float color[3] = {0.30f, 0.90f, 1.00f};   // neon cyan
    std::uint8_t emit_on = 1;
    std::uint8_t opac_on = 1;

    for (const auto& sp : shader_paths) {
        write_inline(renderer, sp, "inputs:emissive_color",
                     kDLFloat, 32, 3, color);
        write_inline(renderer, sp, "inputs:emissive_intensity",
                     kDLFloat, 32, 1, &intensity);
        write_inline(renderer, sp, "inputs:enable_emission",
                     kDLUInt, 8, 1, &emit_on);
        write_inline(renderer, sp, "inputs:opacity_constant",
                     kDLFloat, 32, 1, &opacity);
        write_inline(renderer, sp, "inputs:enable_opacity",
                     kDLUInt, 8, 1, &opac_on);
    }
    return true;
}

bool apply_visibility(ovrtx_renderer_t* renderer,
                      const std::vector<std::string>& mesh_paths,
                      const std::vector<bool>& hide_mask) {
    if (mesh_paths.size() != hide_mask.size() || mesh_paths.empty())
        return true;

    // Tokens are passed as `ovx_string_t` (16 bytes = 128 bits) per
    // ovrtx's binding spec at bindings.py:427 —
    //   "OVRTX_SEMANTIC_TOKEN_STRING ... ovx_string_t (kDLUInt, 128, 1)"
    // NOT the raw string bytes with bits=8. Storing the {ptr,length}
    // struct in the tensor lets ovrtx index the token's content
    // directly.
    static const char* kInvisible = "invisible";
    static const char* kInherited = "inherited";

    bool all_ok = true;
    for (std::size_t i = 0; i < mesh_paths.size(); ++i) {
        const char* token_str = hide_mask[i] ? kInvisible : kInherited;
        ovx_string_t token_value = {token_str, std::strlen(token_str)};

        ovx_string_t pp = {mesh_paths[i].c_str(), mesh_paths[i].size()};
        ovrtx_prim_list_t prim_list{};
        prim_list.prim_paths = &pp;
        prim_list.num_paths = 1;

        ovrtx_attribute_type_t at{};
        at.dtype = {kDLUInt, 128, 1};       // ← per ovrtx token spec
        at.is_array = false;
        at.semantic = OVRTX_SEMANTIC_TOKEN_STRING;

        ovrtx_binding_desc_t b{};
        b.prim_list = prim_list;
        b.attribute_name.string = {"visibility", 10};
        b.attribute_type = at;
        b.prim_mode = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
        b.flags = OVRTX_BINDING_FLAG_NONE;

        ovrtx_binding_desc_or_handle_t boh{};
        boh.binding_desc = b;

        DLTensor t{};
        t.data = &token_value;              // pass the ovx_string_t struct
        t.device = {kDLCPU, 0};
        t.ndim = 1;
        int64_t shape[1] = {1};             // one token, not N bytes
        t.shape = shape;
        t.dtype = {kDLUInt, 128, 1};

        ovrtx_input_buffer_t in_buf{};
        in_buf.tensors = &t;
        in_buf.tensor_count = 1;

        auto wr = ovrtx_write_attribute(renderer, &boh, &in_buf,
                                        OVRTX_DATA_ACCESS_SYNC);
        if (check(wr, "visibility")) all_ok = false;
    }
    return all_ok;
}

// Internal: write `rel material:binding = [target_path]` to ONE mesh.
// Relationships in USD are array-typed (a mesh can bind multiple
// materials), but we always write a 1-element list. PATH_STRING
// semantic = ovx_string_t entries, kDLUInt 128, is_array=true.
static bool write_material_binding_one(ovrtx_renderer_t* renderer,
                                       const std::string& mesh_path,
                                       const std::string& target_path) {
    ovx_string_t pp = {mesh_path.c_str(), mesh_path.size()};
    ovrtx_prim_list_t prim_list{};
    prim_list.prim_paths = &pp;
    prim_list.num_paths = 1;

    ovrtx_attribute_type_t at{};
    at.dtype = {kDLUInt, 128, 1};
    at.is_array = true;                                 // array relationship
    at.semantic = OVRTX_SEMANTIC_PATH_STRING;

    ovrtx_binding_desc_t b{};
    b.prim_list = prim_list;
    b.attribute_name.string = {"material:binding",
                               std::strlen("material:binding")};
    b.attribute_type = at;
    b.prim_mode = OVRTX_BINDING_PRIM_MODE_EXISTING_ONLY;
    b.flags = OVRTX_BINDING_FLAG_NONE;

    ovrtx_binding_desc_or_handle_t boh{};
    boh.binding_desc = b;

    // ONE-element path array. The ovx_string_t is the relationship's
    // target prim path.
    ovx_string_t target = {target_path.c_str(), target_path.size()};

    DLTensor t{};
    t.data = &target;
    t.device = {kDLCPU, 0};
    t.ndim = 1;
    int64_t shape[1] = {1};                              // array length 1
    t.shape = shape;
    t.dtype = {kDLUInt, 128, 1};

    ovrtx_input_buffer_t in_buf{};
    in_buf.tensors = &t;
    in_buf.tensor_count = 1;

    auto wr = ovrtx_write_attribute(renderer, &boh, &in_buf,
                                    OVRTX_DATA_ACCESS_SYNC);
    return !check(wr, "material:binding");
}

bool apply_material_rebind(ovrtx_renderer_t* renderer,
                           const std::vector<std::string>& mesh_paths,
                           const std::string& target_material_path) {
    bool all_ok = true;
    for (const auto& mp : mesh_paths) {
        if (!write_material_binding_one(renderer, mp, target_material_path))
            all_ok = false;
    }
    return all_ok;
}

bool apply_material_restore(ovrtx_renderer_t* renderer,
                            const std::vector<std::string>& mesh_paths,
                            const std::vector<std::string>& original_material_paths) {
    if (mesh_paths.size() != original_material_paths.size()) return false;
    bool all_ok = true;
    for (std::size_t i = 0; i < mesh_paths.size(); ++i) {
        // Empty original = mesh had no MDL surface to begin with;
        // skip those (we never rebound them anyway).
        if (original_material_paths[i].empty()) continue;
        if (!write_material_binding_one(renderer, mesh_paths[i],
                                        original_material_paths[i]))
            all_ok = false;
    }
    return all_ok;
}

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
