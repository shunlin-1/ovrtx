// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
// property and proprietary rights in and to this material, related
// documentation and any modifications thereto. Any use, reproduction,
// disclosure or distribution of this material and related documentation
// without an express license agreement from NVIDIA CORPORATION or
// its affiliates is strictly prohibited.

#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// Bindless indices:
//   scene_index — ovrtx render product (CUDA-shared)
//   ui_index    — Ultralight HTML overlay, premultiplied BGRA8.
//                 0xFFFFFFFFu disables the overlay entirely.
layout(push_constant) uniform PushConstants {
    uint scene_index;
    uint ui_index;
} push;

layout(set = 0, binding = 0) uniform sampler2D textures[];

// 5x5 Gaussian blur of the scene under any UI pixel — gives glass panels
// a real backdrop blur of the 3D scene. CSS backdrop-filter inside the
// Ultralight page only sees other HTML, not our externally-composited
// scene, so this has to live in the composite shader.
vec3 blur_scene(uint idx, vec2 uv) {
    vec2 ts = vec2(textureSize(textures[nonuniformEXT(idx)], 0));
    vec2 px = 1.0 / ts;
    // Tuned for ~14 px panel-blur radius at 1920x1080.
    const float STEP_PX = 3.5;
    const float SIGMA   = 2.5;
    vec3 sum = vec3(0.0);
    float w_total = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            float d2 = float(i*i + j*j);
            float w = exp(-d2 / (2.0 * SIGMA * SIGMA));
            vec2 off = vec2(float(i), float(j)) * px * STEP_PX;
            sum += w * texture(
                textures[nonuniformEXT(idx)], uv + off).rgb;
            w_total += w;
        }
    }
    return sum / w_total;
}

void main() {
    if (push.ui_index == 0xFFFFFFFFu) {
        out_color = vec4(
            texture(textures[nonuniformEXT(push.scene_index)], in_uv).rgb,
            1.0);
        return;
    }

    vec4 ui = texture(textures[nonuniformEXT(push.ui_index)], in_uv);

    // Scene sample: sharp where there's no UI, blurred under panels.
    // Cross-fade keeps panel edges smooth instead of hard blur boundaries.
    vec3 scene_sharp = texture(
        textures[nonuniformEXT(push.scene_index)], in_uv).rgb;
    vec3 scene;
    if (ui.a < 0.01) {
        scene = scene_sharp;
    } else {
        vec3 scene_blur = blur_scene(push.scene_index, in_uv);
        // Blend blur in over the first ~10% of alpha so soft shadows
        // don't smear the scene; full blur once panel is opaque enough.
        float t = smoothstep(0.0, 0.30, ui.a);
        scene = mix(scene_sharp, scene_blur, t);
    }

    // Ultralight outputs *premultiplied* BGRA when is_transparent=true,
    // so the correct over-composite is: out = scene*(1-a) + ui.rgb.
    vec3 rgb = scene * (1.0 - ui.a) + ui.rgb;
    out_color = vec4(rgb, 1.0);
}

