// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

// ovrtx_renderer_t is an opaque struct typedef'd in <ovrtx/ovrtx_types.h>;
// avoid forward-declaring it (the tag matches the typedef name, which
// breaks `typedef struct X X` redeclaration on some compilers). Include
// directly instead.
#include <ovrtx/ovrtx_types.h>

#include "pick_table.h"

namespace agv {

// Apply "X-ray Neon" to N shaders simultaneously. v=0 restores original
// values (caller passes them via orig_color/orig_intensity arrays);
// v=1 = fully translucent + bright cyan emission on every shader.
// Returns true on success.
//
// Single batched ovrtx call per attribute writes all N shaders at once.
bool apply_global_xray_neon(ovrtx_renderer_t* renderer,
                            const std::vector<std::string>& shader_paths,
                            const std::vector<std::array<float, 3>>& orig_colors,
                            const std::vector<float>& orig_intensities,
                            double v);

// Write USD `visibility` token per mesh. Each entry in `mesh_paths`
// gets either "invisible" or "inherited" from `hide_mask` (parallel
// arrays). Caller pre-computes the mask using their preferred Y-clip
// predicate.
bool apply_visibility(ovrtx_renderer_t* renderer,
                      const std::vector<std::string>& mesh_paths,
                      const std::vector<bool>& hide_mask);

// Override every mesh's `rel material:binding` to point at one common
// target Material prim. Used when "Section Clip" is turned on — every
// mesh gets rebound to the custom clip MDL so per-fragment world-Y
// discard kicks in. One write per mesh (relationship arrays are
// PATH_STRING semantic, kDLUInt 128).
bool apply_material_rebind(ovrtx_renderer_t* renderer,
                           const std::vector<std::string>& mesh_paths,
                           const std::string& target_material_path);

// Restore each mesh's `rel material:binding` back to its original
// target Material prim. mesh_paths and original_material_paths are
// parallel arrays. Used when Section Clip is turned off.
bool apply_material_restore(ovrtx_renderer_t* renderer,
                            const std::vector<std::string>& mesh_paths,
                            const std::vector<std::string>& original_material_paths);

// One shader's per-mode override state. Mirrors the bindings dict in
// examples/python/agv/main.py:OvrtxBackend._ensure_sel — we cache one
// entry per shader path so re-clicks reuse the same binding shape.
struct ShaderOverride {
    const PickEntry* pi = nullptr;  // back-pointer for orig values
    enum class Mode { None, Neon, Xray, XrayLight };
    Mode active = Mode::None;
};

// Manages OmniPBR-input attribute writes on bound shaders.
// Not thread-safe; expected to live on the ovrtx worker thread.
class MaterialOverrides {
public:
    explicit MaterialOverrides(ovrtx_renderer_t* renderer);

    // Apply the currently-selected pick mode to `entry`'s shader.
    // Re-clicking the same mesh in the same mode restores it.
    // Switching mode + clicking the same mesh restores then re-applies.
    void apply(const PickEntry& entry, ShaderOverride::Mode requested);

private:
    void write_emissive(const std::string& shader_path,
                        const float color[3], float intensity);
    void write_opacity(const std::string& shader_path, float opacity);
    void restore(const PickEntry& entry);

    ovrtx_renderer_t* renderer_;
    std::unordered_map<std::string, ShaderOverride> state_;
};

ShaderOverride::Mode mode_from_string(const std::string& s);

}  // namespace agv
