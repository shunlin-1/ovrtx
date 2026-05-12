// Edge-detection post-process for the ovrtx LdrColor frame.
//
// Sobel filter on luminance — outlines anything with strong color
// contrast to its neighbors. Works without G-buffer access (only the
// final RGB image is needed), at the cost of being a *color-edge*
// detector: same-colored objects against same-colored backgrounds
// won't get outlined. For "real" geometric outlines you need depth +
// normal AOVs — see RENDERER_ROADMAP.md.
//
// Compile: ~/Qt/6.8.1/gcc_64/bin/qsb -o outline.frag.qsb outline.frag --glsl 440

#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    // Per-pixel sampling step: should be (1/width, 1/height) of the
    // source texture so `thickness` is in pixel units.
    vec2  pixelStep;
    // 0..1 blend of outline-colour over the unmodified source.
    float outlineStrength;
    // Minimum Sobel magnitude to register as an edge. Lower = more
    // edges (also more noise); higher = only the strongest contours.
    float threshold;
    // Multiplier on pixelStep — controls line thickness. 1.0 = 1 px,
    // 2.0 = 2 px, etc. Higher values look stylised but blur fine detail.
    float thickness;
    vec4  outlineColor;
};

layout(binding = 1) uniform sampler2D source;

// Rec.601 luma (matches what most edge-detect filters use).
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 px = pixelStep * thickness;

    // 3x3 luminance samples around the current pixel.
    float l00 = luma(texture(source, qt_TexCoord0 + px * vec2(-1.0, -1.0)).rgb);
    float l10 = luma(texture(source, qt_TexCoord0 + px * vec2( 0.0, -1.0)).rgb);
    float l20 = luma(texture(source, qt_TexCoord0 + px * vec2( 1.0, -1.0)).rgb);
    float l01 = luma(texture(source, qt_TexCoord0 + px * vec2(-1.0,  0.0)).rgb);
    float l21 = luma(texture(source, qt_TexCoord0 + px * vec2( 1.0,  0.0)).rgb);
    float l02 = luma(texture(source, qt_TexCoord0 + px * vec2(-1.0,  1.0)).rgb);
    float l12 = luma(texture(source, qt_TexCoord0 + px * vec2( 0.0,  1.0)).rgb);
    float l22 = luma(texture(source, qt_TexCoord0 + px * vec2( 1.0,  1.0)).rgb);

    // Sobel kernels:
    //   Gx = [-1 0 1 / -2 0 2 / -1 0 1]
    //   Gy = [-1 -2 -1 / 0 0 0 / 1 2 1]
    float gx = -l00 + l20 + (-2.0 * l01) + (2.0 * l21) + (-l02) + l22;
    float gy = -l00 - (2.0 * l10) - l20 + l02 + (2.0 * l12) + l22;
    float edge = length(vec2(gx, gy));

    // Smooth threshold = crisp lines but no aliasing on near-edges.
    float mask = smoothstep(threshold, threshold + 0.08, edge) * outlineStrength;

    // Composite: keep source colour, mix outline colour where edge fires.
    vec3 srcColor = texture(source, qt_TexCoord0).rgb;
    vec3 finalColor = mix(srcColor, outlineColor.rgb, mask);

    fragColor = vec4(finalColor, 1.0) * qt_Opacity;
}
