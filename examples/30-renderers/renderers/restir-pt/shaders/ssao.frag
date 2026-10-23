$input v_color0
$input v_texcoord0

// ssao.frag -- pass 10, render resolution: screen-space ambient occlusion, kept subtle.
//
// Twelve points in the hemisphere over the surface, out to restirLook.x, each carried to the screen; one
// is occluded when the G-buffer there is nearer the camera than it is (by more than a sliver), and only
// within the radius -- something far in front of the point is not round it. The kernel's rotation is a
// fixed 4x4 pattern per texel, not per frame, so the result does not crawl; ao_apply's 4x4 blur takes
// the pattern out again.
//
// It darkens only the indirect light (ao_apply): the sun's own shadows are traced, and are exact.
//
// Inputs: 0-5 the G-buffer (gPos, gNormal, ...). Output: ao (r occlusion-free share, g distance; < 0 sky).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);

#define RESTIR_AO_SAMPLES 12

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(gPos, p, 0);
    if (surface.w < 0.0) { gl_FragColor = vec4(1.0, -1.0, 0.0, 0.0); return; }
    vec3 P = surface.xyz;
    vec3 N = normalize(texelFetch(gNormal, p, 0).xyz);
    float radius = restirLook.x;
    vec2 res = passTargetRes.xy;

    // The 4x4 Bayer matrix's entry, as the kernel's turn.
    int bx = p.x & 3;
    int by = p.y & 3;
    // B4 = 4 B2(low bits) + B2(high bits), with B2(x, y) = 2 (x xor y) + y: [[0, 2], [3, 1]].
    int bayer = 4 * ((((bx ^ by) & 1) << 1) | (by & 1)) + ((((bx ^ by) >> 1) << 1) | (by >> 1));
    float turn = (float(bayer) + 0.5) / 16.0 * 2.0 * RESTIR_PI;
    vec3 t;
    vec3 b;
    restirBasis(N, t, b);

    float open = 0.0;
    for (int i = 0; i < RESTIR_AO_SAMPLES; i++) {
        float u = (float(i) + 0.5) / float(RESTIR_AO_SAMPLES);
        float phi = turn + float(i) * 2.39996;                 // the golden angle
        float up = sqrt(1.0 - u);                              // cosine-weighted elevation
        float out_ = sqrt(u);
        float reach = radius * mix(0.2, 1.0, u * u);           // more samples close in
        vec3 Q = P + N * (radius * 0.04) + (t * (cos(phi) * out_) + b * (sin(phi) * out_) + N * up) * reach;
        bool inFront;
        vec2 uv = restirWorldToUV(Q, cameraPos.xyz, cameraDir.xyz, res, inFront);
        if (!inFront || uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) { open += 1.0; continue; }
        vec4 there = texelFetch(gPos, ivec2(uv * res), 0);
        float depthQ = distance(Q, cameraPos.xyz);
        if (there.w < 0.0 || there.w > depthQ - radius * 0.05) { open += 1.0; continue; }
        // Occluded -- unless what is in front is well outside the radius of P.
        open += 1.0 - clamp(radius / max(abs(surface.w - there.w), 1e-4) - 1.0, 0.0, 1.0);
    }
    gl_FragColor = vec4(open / float(RESTIR_AO_SAMPLES), surface.w, 0.0, 0.0);
}
