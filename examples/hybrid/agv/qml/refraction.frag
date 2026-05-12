// Liquid Glass — single-pass fragment shader.
//
// Refactor: collapsed three competing highlights (bevel + overhead
// specular + rim glow) into ONE rim-lighting system with an
// omnidirectional component and a directional component. Border is
// drawn in-shader from the same SDF as the alpha mask, so silhouette
// + outline are pixel-perfect aligned even under heavy magnification.
//
// Pipeline:
//   1. SDF rounded-rect + outward normal.
//   2. UV displacement = inward edge refraction + (optional) noise.
//   3. RGB sample with chromatic dispersion split along normal.
//   4. Tint mix.
//   5. Rim light: omnidirectional band glow + directional facing term.
//   6. Border drawn just inside the rim using the same SDF.
//   7. Anti-aliased rounded alpha mask.
//
// Compile: pyside6-qsb --qt6 -o refraction.frag.qsb refraction.frag

#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    // Geometry
    vec2  itemSize;
    float cornerRadius;
    // Lens magnification
    float refractWidth;        // px band depth from rim
    float refractStrength;     // px peak inward displacement
    float chromaticDispersion; // px RGB split along normal
    // Optional surface noise (feTurbulence analogue)
    float noiseFrequency;
    float noiseStrength;
    // Surface
    vec4  tintColor;
    // Unified rim light
    float rimWidth;            // px band depth where rim lighting lives
    float rimBrightness;       // 0..1 — omnidirectional inner glow
    float rimSpecular;         // 0..1 — directional facing highlight
    float rimLightX;           // light direction X (screen-space)
    float rimLightY;           // light direction Y (down is +1)
    // Border
    float borderWidth;         // px (0 = no border)
    vec4  borderColor;
    // Shape — 0 = rounded rectangle, 1 = ellipse. Continuous values
    // in between morph between the two shapes (mix of SDFs).
    float ellipseMode;
};

layout(binding = 1) uniform sampler2D source;       // BLURRED backdrop
layout(binding = 2) uniform sampler2D sourceSharp;  // RAW (pre-blur) backdrop

// ─── 2D gradient noise + 2-octave fbm (CSS feTurbulence analogue) ───
vec2 hash22(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)),
             dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}
float noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float n00 = dot(hash22(i + vec2(0.0, 0.0)), f - vec2(0.0, 0.0));
    float n10 = dot(hash22(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float n01 = dot(hash22(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float n11 = dot(hash22(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));
    return mix(mix(n00, n10, u.x),
               mix(n01, n11, u.x), u.y);
}
float fbm2(vec2 p) {
    return 0.65 * noise2(p) + 0.35 * noise2(p * 2.13 + vec2(13.0, 7.0));
}

// ─── Signed distance fields ─────────────────────────────────────────
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Cheap ellipse SDF — exact on the boundary and at the centre, slight
// magnitude error elsewhere. Sufficient for masking + finite-diff
// gradient. (Full IQ accurate version is ~30 lines; not needed here.)
float sdEllipse(vec2 p, vec2 ab) {
    return (length(p / ab) - 1.0) * min(ab.x, ab.y);
}

// Unified shape SDF — morphs between rounded rectangle and ellipse.
float sdShape(vec2 p, vec2 b, float r, float em) {
    return mix(sdRoundedBox(p, b, r), sdEllipse(p, b), em);
}

void main() {
    // ── 1. Geometry: SDF + outward normal ───────────────────────────
    vec2 uv = qt_TexCoord0;
    vec2 pos = (uv - 0.5) * itemSize;
    vec2 halfSize = itemSize * 0.5;

    float sdf = sdShape(pos, halfSize, cornerRadius, ellipseMode);

    const float h = 1.0;
    float dx = sdShape(pos + vec2( h, 0.0), halfSize, cornerRadius, ellipseMode)
             - sdShape(pos + vec2(-h, 0.0), halfSize, cornerRadius, ellipseMode);
    float dy = sdShape(pos + vec2(0.0,  h), halfSize, cornerRadius, ellipseMode)
             - sdShape(pos + vec2(0.0, -h), halfSize, cornerRadius, ellipseMode);
    vec2 normal = normalize(vec2(dx, dy));

    // ── 2. UV displacement (inward magnification + optional noise) ──
    // weight = pow(1-band, 3): bounded, smooth, rim-concentrated.
    // Approximates the squircle slope shape without the infinity-at-
    // rim singularity that previously required a clamp(). The clamped
    // saturated rim zone was creating a visible "translation strip"
    // where text content got pasted into the band — gone with this
    // formulation because the curve is finite at every point.
    //
    // mixT keeps its smoothstep curve for the blur↔sharp blend.
    float band = clamp(-sdf / refractWidth, 0.0, 1.0);
    float u    = 1.0 - band;
    float weight = u * u * u;
    float mixT = smoothstep(0.0, 1.0, 1.0 - band);

    vec2 noiseDisp = vec2(0.0);
    if (noiseStrength > 0.0) {
        vec2 nuv = uv * noiseFrequency;
        noiseDisp = vec2(fbm2(nuv), fbm2(nuv + vec2(91.7, 47.3)));
    }
    vec2 totalDisp = (-normal * weight * refractStrength
                      + noiseDisp * noiseStrength) / itemSize;

    // ── 3. Dual-source sample: SHARP at rim (lens), BLUR at centre ─
    // Real frosted glass: bulk scatters light → blur, but the curved
    // rim acts as a lens → focused/sharp. We sample both textures and
    // blend by `weight` (1 at rim, 0 deep inside). Chromatic dispersion
    // applies only to the sharp lens path — blur would smear it out.
    vec2 chrom = (normal * weight * chromaticDispersion) / itemSize;
    vec2 uvR = clamp(uv + totalDisp + chrom, vec2(0.0), vec2(1.0));
    vec2 uvG = clamp(uv + totalDisp,         vec2(0.0), vec2(1.0));
    vec2 uvB = clamp(uv + totalDisp - chrom, vec2(0.0), vec2(1.0));

    vec3 sharpRgb;
    sharpRgb.r = texture(sourceSharp, uvR).r;
    sharpRgb.g = texture(sourceSharp, uvG).g;
    sharpRgb.b = texture(sourceSharp, uvB).b;

    vec3 blurRgb = texture(source, uvG).rgb;

    // Smooth blend: rim = sharp lens, centre = pure frost. Uses mixT
    // (smoothstep) instead of `weight` (pow) so the boundary fades
    // gracefully instead of forming a visible ring.
    vec3 rgb = mix(blurRgb, sharpRgb, mixT);

    // ── 4. Tint ─────────────────────────────────────────────────────
    rgb = mix(rgb, tintColor.rgb, tintColor.a);

    // ── 5. Unified rim light: omni + directional, one band ─────────
    float rimBand   = clamp(-sdf / rimWidth, 0.0, 1.0);
    float rimWeight = pow(1.0 - rimBand, 1.6);

    // Omnidirectional inner glow — surface tension look, all-around.
    float omni = rimWeight * rimBrightness;

    // Directional facing highlight — light from rimLight direction.
    // Tight pow() lobe; light direction is normalised in the shader so
    // QML can pass any magnitude.
    vec2 light = normalize(vec2(rimLightX, rimLightY));
    float facing = max(dot(normal, light), 0.0);
    float spec   = pow(facing, 4.0) * rimWeight * rimSpecular;

    rgb += vec3(omni + spec);

    // ── 6. In-shader border — perfectly aligned with the mask ──────
    // 2-px AA ramps (was 1 px). Wider AA reads as visually smoother
    // around the rounded corners while still tracing a thin border.
    if (borderWidth > 0.0) {
        float bdInner = -borderWidth;
        float onBorder = smoothstep(bdInner - 1.0, bdInner + 1.0, sdf)
                       * (1.0 - smoothstep(-1.0, 1.0, sdf));
        rgb = mix(rgb, borderColor.rgb, onBorder * borderColor.a);
    }

    // ── 7. Anti-aliased rounded-rect alpha mask (2-px AA) ──────────
    float alpha = 1.0 - smoothstep(-1.0, 1.0, sdf);

    fragColor = vec4(rgb, 1.0) * alpha * qt_Opacity;
}
