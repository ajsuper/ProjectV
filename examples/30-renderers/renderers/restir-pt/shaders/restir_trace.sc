// restir_trace.sc -- what the tracing passes (gbuffer, sample, spatial) include: the engine's scene query
// behind one small function, then the pipeline's shared pieces.
//
// Every ray goes through raySceneIntersectFrom, the engine's bare nearest-solid query -- the one the
// voxel renderer uses (no transparency peel, no refraction). Each pass calls restirTrace from ONE place,
// inside a loop whose bound is a uniform: a traversal inlined at a second call site makes every ray in
// the shader slower, and an unrollable bound would copy it anyway.

#define PJV_NO_ANIMATION   // the gallery's scenes do not move; compiling the animated march out is faster
#include <pjv_utils_DDA.sc>
#include "restir_common.sc"

#define RESTIR_KIND_CAMERA 0   // nearest surface, its material
#define RESTIR_KIND_BOUNCE 1   // nearest surface, its material; coarse in the distance (seen only indirectly)
#define RESTIR_KIND_SHADOW 2   // is anything in the way? nothing else is read
#define RESTIR_KIND_SHADOW_INDIRECT 3   // the same, from a bounce's surface: its light is seen indirectly
#define RESTIR_KIND_BOUNCE_SHARP 4      // a bounce seen sharply (off a glossy surface, from the camera): exact
#define RESTIR_STEPS 512u

// Coarse tracing, by what the ray is for. A distance is in voxels from the ray's origin; past it, a
// non-empty 4^3 brick counts as solid (its material: the first voxel the tree finds in it). 0 = exact.
//
// The cost: a brick is solid where any voxel of it is, so an opening that is not brick-aligned shrinks
// for the rays that light a room through it -- in the material room (a skylight and a window) the
// indirect light came out about a fifth dark against pt-reference. Set both to 0 for exact rays.
#ifndef RESTIR_BOUNCE_LOD
#define RESTIR_BOUNCE_LOD 64            // the ray lab's mode 10: bounces see bricks past 64 voxels
#endif
#ifndef RESTIR_SHADOW_INDIRECT_LOD
#define RESTIR_SHADOW_INDIRECT_LOD 16
#endif
#ifndef RESTIR_SHADOW_LOD
#define RESTIR_SHADOW_LOD 0             // the camera hit's sun: exact
#endif
#ifndef RESTIR_CAMERA_FOOTPRINT
#define RESTIR_CAMERA_FOOTPRINT 0       // camera rays coarsen where a node is smaller than a pixel
#endif

struct RestirHit {
    bool  hit;
    float t;          // world units along the (unit) direction
    vec3  normal;     // the face the ray entered; zero when it started inside a voxel
    vec3  albedo;
    vec3  emission;   // radiance
    float glossiness;
    float metallic;
    vec4  face;       // camera rays only: the hit face's centre, w = the voxel's edge (0: no hit)
    vec4  rotation;   // camera rays only: the chunk's rotation (the voxel's own axes)
};

RestirHit restirTrace(vec3 origin, vec3 direction, int kind, float maxDistance) {
    Ray ray;
    ray.origin = origin;
    ray.direction = direction;
    bool shadow = kind == RESTIR_KIND_SHADOW || kind == RESTIR_KIND_SHADOW_INDIRECT;
    RayQuery query = shadow ? pjvShadowQuery(RESTIR_STEPS, maxDistance) : pjvPrimaryQuery(RESTIR_STEPS);
#if RESTIR_BOUNCE_LOD
    if (kind == RESTIR_KIND_BOUNCE) pjvQueryLinearLOD(query, 1u, uint(RESTIR_BOUNCE_LOD));
#endif
#if RESTIR_SHADOW_INDIRECT_LOD
    if (kind == RESTIR_KIND_SHADOW_INDIRECT) pjvQueryLinearLOD(query, 1u, uint(RESTIR_SHADOW_INDIRECT_LOD));
#endif
#if RESTIR_SHADOW_LOD
    if (kind == RESTIR_KIND_SHADOW) pjvQueryLinearLOD(query, 1u, uint(RESTIR_SHADOW_LOD));
#endif
#if RESTIR_CAMERA_FOOTPRINT
    // Where one voxel covers one pixel: the target's height over the view's tangent span.
    if (kind == RESTIR_KIND_CAMERA) pjvQueryLODFootprint(query, uint(passTargetRes.y / (2.0 * tan(radians(RESTIR_FOV * 0.5)))), 2u);
#endif
    SceneIntersectData found = raySceneIntersectFrom(ray, query, 0.0);
    RestirHit h;
    h.hit = false;
    h.t = -1.0;
    h.normal = vec3(0.0, 0.0, 0.0);
    h.albedo = vec3(0.0, 0.0, 0.0);
    h.emission = vec3(0.0, 0.0, 0.0);
    h.glossiness = 0.0;
    h.metallic = 0.0;
    h.face = vec4(0.0, 0.0, 0.0, 0.0);
    h.rotation = vec4(0.0, 0.0, 0.0, 1.0);
    // The engine's own miss test, as every renderer writes it.
    if (found.foundBox.size < 0.0 || found.rayT <= 0.0 || dot(found.normal, found.normal) < 0.5) return h;
    h.hit = true;
    h.t = found.rayT;
    h.normal = found.normal;
    if (!shadow) {
        VoxelMaterial material = fetchVoxelMaterialFromHit(found);
        h.albedo = material.albedo;   // linear: the engine decodes the palette's sRGB (pjv_color.sc)
        h.emission = material.emission;
        // The paths' cap (restir_common.sc); the camera ray keeps the real one, for the sun's highlight
        // and the screen-space reflection.
        h.glossiness = kind == RESTIR_KIND_CAMERA ? material.glossiness : min(material.glossiness, RESTIR_GI_MAX_GLOSSINESS);
        h.metallic = material.metallic;
    }
    if (kind == RESTIR_KIND_CAMERA) {
        // What the reconstruction needs to know the whole voxel: its face's centre, its edge, and its
        // chunk's rotation (the voxel's minimum LOCAL corner plus the turned half-diagonal is its centre).
        h.rotation = headers(int(found.headerIndex)).rotation;
        float edge = found.foundBox.size;
        vec3 centre = found.foundBox.position + rotationFromQuat(h.rotation) * vec3(edge * 0.5, edge * 0.5, edge * 0.5);
        h.face = vec4(centre + normalize(found.normal) * (edge * 0.5), edge);
    }
    return h;
}
