// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#pragma once

#include <string>
#include <unordered_map>

// ovrtx_renderer_t is an opaque struct typedef'd in <ovrtx/ovrtx_types.h>;
// avoid forward-declaring it (the tag matches the typedef name, which
// breaks `typedef struct X X` redeclaration on some compilers). Include
// directly instead.
#include <ovrtx/ovrtx_types.h>

#include "pick_table.h"

namespace agv {

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
