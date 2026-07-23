#include "DK2ShaderTypes.h"

// Solve for the world XY that projects to a given NDC on the ground plane
// Z=0. In DK2 world space X/Y are the horizontal map axes and Z is height, so
// the floor is a Z=const plane; the near-top-down camera never sees it
// edge-on, so there is no horizon singularity. World X (col 0) and Y (col 1)
// are the unknowns; Z (col 2) is pinned to the plane height. Only clip rows
// x/y/w are used (row z is the piecewise-depth far branch, unreliable). Two
// equations, two unknowns.
//
// CAVEAT: this assumes the water/lava surface sits exactly at world Z=0.
// That has not been measured against the engine's actual floor-height
// constant (see Obj57AD20.cpp retained.origin.z, which is per-mesh, not a
// verified global floor height) - if the true height differs, the
// reconstructed XY is parallax-shifted by an amount that grows with camera
// tilt/distance. It still pans/scales with the camera (same fixed offset
// under a given view), just not guaranteed pixel-exact to the mesh.
//
// Also computed per-VERTEX and linearly interpolated across the triangle,
// not per-fragment: this function is nonlinear in `ndc` (division by a
// per-ndc determinant), so on a large water polygon the interpolated result
// is an approximation, not the exact per-pixel unprojection. Fine for the
// small per-cell tiles DK2 uses; would need moving the call into
// dk2_fragment (using input.position.xy reconstructed to NDC) for exactness
// on bigger polygons.
float2 dk2_unproject_ground(constant DK2MeshCamera &camera, float2 ndc) {
    const float4 A = camera.viewProj[0];      // world x column
    const float4 B = camera.viewProj[1];      // world y column
    const float4 D = camera.viewProj[3];      // translation column (world Z=0 term)
    const float a00 = A.x - ndc.x * A.w, a01 = B.x - ndc.x * B.w;
    const float a10 = A.y - ndc.y * A.w, a11 = B.y - ndc.y * B.w;
    const float b0 = ndc.x * D.w - D.x;
    const float b1 = ndc.y * D.w - D.y;
    const float det = a00 * a11 - a01 * a10;
    if (abs(det) < 1e-12f) return float2(0.0f);
    const float inv = 1.0f / det;
    return float2((b0 * a11 - a01 * b1) * inv, (a00 * b1 - b0 * a10) * inv);
}

// Value noise + fbm for the ripple height field (cheap, tileless).
static inline float dk2_hash21(float2 p) {
    p = fract(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return fract(p.x * p.y);
}
static inline float dk2_vnoise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = dk2_hash21(i);
    float b = dk2_hash21(i + float2(1.0f, 0.0f));
    float c = dk2_hash21(i + float2(0.0f, 1.0f));
    float d = dk2_hash21(i + float2(1.0f, 1.0f));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
static inline float dk2_fbm(float2 p) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sum += amp * dk2_vnoise(p);
        p *= 2.03f;
        amp *= 0.5f;
    }
    return sum;
}

// Simple, tasteful top-down water/lava. Standard practice: two slow scrolling
// fbm layers give a height field; its gradient is the surface normal; a
// view-independent crest term lifts the steep flanks of wavelets (no fake
// "sun" in a dungeon). Calm palette, no high-frequency sparkle spam. The
// domain is the world XY reconstructed on the floor plane, so it is
// physically fixed to the level. Only base.a (tile/shore shape) is kept from
// the engine texture; base.rgb luminance is kept to ground the effect in the
// tile's own lighting (torch glow, shadow) instead of pasting a flat sheet.
float4 dk2_water_overlay(float4 base, float2 worldXY, float t, bool lava) {
    const float lum = dot(base.rgb, float3(0.299f, 0.587f, 0.114f));

    const float kScale = 0.22f;               // world units -> ripple size (tune)
    const float2 p = worldXY * kScale;
    const float flow = lava ? 0.3f : 1.0f;    // lava creeps, water flows
    const float2 a = p        + float2(0.030f, 0.021f) * t * flow;
    const float2 b = p * 1.7f - float2(0.019f, 0.034f) * t * flow;
    const float e = 0.15f;
    const float h  = dk2_fbm(a)                * 0.6f + dk2_fbm(b)                * 0.4f;
    const float hx = dk2_fbm(a + float2(e, 0)) * 0.6f + dk2_fbm(b + float2(e, 0)) * 0.4f;
    const float hy = dk2_fbm(a + float2(0, e)) * 0.6f + dk2_fbm(b + float2(0, e)) * 0.4f;
    const float slope = lava ? 1.4f : 2.2f;
    const float3 N = normalize(float3((h - hx) * slope / e, (h - hy) * slope / e, 1.0f));
    // View-independent crest term: bright on the steep flanks of wavelets. No
    // camera/sun direction needed, so the tilt of the camera never breaks it.
    const float crest = pow(saturate(1.0f - N.z), lava ? 1.5f : 2.2f);

    float3 col;
    if (lava) {
        const float3 cool = float3(0.32f, 0.05f, 0.02f);
        const float3 hot  = float3(1.7f, 0.60f, 0.13f);
        col = mix(cool, hot, saturate(h + 0.2f));
        col *= 0.6f + lum * 0.8f;                            // grounded in scene
        col += crest * float3(1.5f, 0.55f, 0.15f) * 0.7f;    // molten crest glow
    } else {
        const float3 deep    = float3(0.04f, 0.14f, 0.19f);
        const float3 shallow = float3(0.10f, 0.28f, 0.34f);
        col = mix(deep, shallow, saturate(h + 0.3f));
        col *= 0.35f + lum * 1.1f;                           // grounded in scene
        col += crest * lum * float3(0.5f, 0.7f, 0.8f) * 0.6f; // ripple catches light
    }
    return float4(col, base.a);
}
