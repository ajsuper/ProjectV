// =============================================================================
// reconstruct_trace.frag  --  The real ray, for only the pixels reconstruct.frag could not prove.
//
// Writes FBO 1 -- the G-buffer textures reconstruct.frag has just filled -- and DISCARDS every pixel the
// mask does not flag, so those keep the rebuilt value. The march is compiled in here and nowhere else in
// the reduced-resolution path: this is the one pass whose occupancy it is allowed to set, and on a
// typical frame almost every wave of it leaves at the discard without touching the tree.
//
// Inputs: FBO 7 (the coarse G-buffer, slots 0-1, for validation) and FBO 9 (reconstructMask, slot 2).
// =============================================================================
#include "reconstruct_common.sc"

SAMPLER2D(reconstructMask, 2);

void main() {
    if (texture2DLod(reconstructMask, v_texcoord0, 0.0).x < 0.5) discard;

    Ray ray = reconstructionRay(v_texcoord0);
    SceneIntersectData hit = raySceneIntersect(ray, viewportPrimaryQuery()).hit;
    bool traceMissed = isMiss(hit);
    int debugView = int(reconstructParams.x + 0.5);

    ViewportGBuffer result;
    if (debugView == 2) {
        // Validation: reconstruct.frag flagged every pixel. Recompute what it would have decided, and
        // where it would have answered by itself, check that answer against the ray.
        Reconstruction r = reconstructPixel(ray, v_texcoord0);
        bool agrees = true;
        if (r.trusted) {
            agrees = !traceMissed &&
                     distance(fetchVoxelColorFromHit(hit), r.albedo) < 1e-3 &&
                     abs(hit.rayT - r.t) <= max(r.size * 0.01, r.t * 1e-4) &&
                     dot(normalize(hit.normal), r.normal) > 0.99;
        } else if (r.allSky) {
            agrees = traceMissed;
        }
        if (!agrees) {
            result = surfaceTexel(vec3(1.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0), 1.0, ray, 10.0);
        } else if (traceMissed) {
            result = backgroundTexel(ray);
        } else {
            vec3 albedo = fetchVoxelColorFromHit(hit);
            if (!r.trusted) albedo = mix(albedo, vec3(1.0, 0.0, 0.0), 0.6);
            result = surfaceTexel(albedo, normalize(hit.normal), hit.foundBox.size, ray, hit.rayT);
        }
    } else if (traceMissed) {
        result = backgroundTexel(ray);
    } else {
        vec3 albedo = fetchVoxelColorFromHit(hit);
        if (debugView == 1) albedo = mix(albedo, vec3(1.0, 0.0, 0.0), 0.6);
        result = surfaceTexel(albedo, normalize(hit.normal), hit.foundBox.size, ray, hit.rayT);
    }

    gl_FragData[0] = result.color;
    gl_FragData[1] = result.normal;
    gl_FragData[2] = result.position;
    gl_FragData[3] = result.glow;
    gl_FragData[4] = vec4(result.distance, 0.0, 0.0, 0.0);
}
