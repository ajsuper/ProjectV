// =============================================================================
// reconstruct.frag  --  Every viewport pixel the coarse trace can answer, answered; the rest marked.
//
// The first of the two passes in reconstruct_common.sc (read that for how and why this is exact). No
// march is compiled in here -- a candidate test is a few texel fetches and a slab test -- so this runs at
// the occupancy of a cheap full-screen pass. Pixels it cannot prove are written as background and
// flagged in reconstructMask, and reconstruct_trace.frag overwrites exactly those with a real ray.
//
// Writes FBO 8: the five targets of albedo.frag's G-buffer (the same textures FBO 1 holds) and the mask.
// =============================================================================
#include "reconstruct_common.sc"

void main() {
    Ray ray = reconstructionRay(v_texcoord0);
    Reconstruction r = reconstructPixel(ray, v_texcoord0);
    // In validation mode every pixel goes to the trace pass, which recomputes this and compares.
    bool validate = int(reconstructParams.x + 0.5) == 2;

    ViewportGBuffer result = backgroundTexel(ray);
    float needsTrace = 1.0;
    if (!validate && r.trusted) {
        result = surfaceTexel(r.albedo, r.normal, r.size, ray, r.t);
        needsTrace = 0.0;
    } else if (!validate && r.allSky) {
        needsTrace = 0.0;
    }

    gl_FragData[0] = result.color;
    gl_FragData[1] = result.normal;
    gl_FragData[2] = result.position;
    gl_FragData[3] = result.glow;
    gl_FragData[4] = vec4(result.distance, 0.0, 0.0, 0.0);
    gl_FragData[5] = vec4(needsTrace, 0.0, 0.0, 1.0);
}
