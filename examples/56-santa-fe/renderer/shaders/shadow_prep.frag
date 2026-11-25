$input v_color0
$input v_texcoord0

// =============================================================================
// shadow_prep.frag -- the sun's shadow at the window's resolution, step 1 (santa-fe-reference S22c).
//
// The shadow comes from the trace's one hard shadow ray a voxel face (from the face's centre: every texel of a
// face agrees, so the reconstruction rebuilds it at the window's resolution with the voxels' own edges). Here,
// PCSS-style, the blocker distance of the shadowed pixels round this one sets the penumbra (`blocker *
// sfShadow.x` metres: a contact shadow stays sharp, a shadow cast from far is soft), in pixels; the A-Trous
// steps that follow (shadow_atrous.sc) blur by it. Nothing is random and nothing is kept between frames.
//
// Inputs: the reconstruction (the colour 0, the sun 1, reflections + the visibility 2, the blocker etc. 3, the
// face 4, normal + depth 5). Output: r the visibility, g the penumbra (pixels), b the depth.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>

SAMPLER2D(rReflVis, 2);   // a: the visibility
SAMPLER2D(rAux, 3);       // x: the blocker distance
SAMPLER2D(rND, 5);        // xyz the normal, w the depth (-1 the sky)

uniform vec4 sfShadow;    // x: penumbra per metre of blocker distance (0: hard), y: the blocker search's radius (pixels)

#define SF_SHADOW_SEARCH_TAPS 16

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    ivec2 size = ivec2(passTargetRes.xy);
    float vis = clamp(texelFetch(rReflVis, at, 0).a, 0.0, 1.0);
    float depth = texelFetch(rND, at, 0).w;
    if (depth < 0.0 || sfShadow.x <= 0.0) { gl_FragColor = vec4(vis, 0.0, depth, 0.0); return; }
    float pixelWorld = max(depth, 1e-3) * 2.0 * pjvViewTanHalfFov() / passTargetRes.y;
    float maxR = sfShadow.y;
    float blockSum = 0.0, blockN = 0.0;
    for (int i = 0; i < SF_SHADOW_SEARCH_TAPS; i++) {
        float r = sqrt((float(i) + 0.5) / float(SF_SHADOW_SEARCH_TAPS)) * maxR;
        float a = float(i) * 2.39996323;
        ivec2 q = clamp(at + ivec2(round(vec2(cos(a), sin(a)) * r)), ivec2(0, 0), size - 1);
        float b = texelFetch(rAux, q, 0).x;
        if (texelFetch(rReflVis, q, 0).a < 0.5 && b > 0.0) { blockSum += b; blockN += 1.0; }
    }
    float radius = blockN > 0.5 ? clamp((blockSum / blockN) * sfShadow.x / pixelWorld, 0.0, maxR) : 0.0;
    gl_FragColor = vec4(vis, radius, depth, 0.0);
}
