// =============================================================================
// pjv_cascade_common.sc  --  Sampler-free math shared across the radiance-cascade
// renderer's passes (cascade gather/merge, resolve, temporal accumulate).
//
// Holds the cascade parameterization, the octahedral direction mapping, and the
// world->screen projection. Kept free of any SAMPLER2D so passes that only need the
// math (resolve, accumulate) can include it without having to declare the cascade's
// G-buffer samplers. The sampler-using gather/merge lives in pjv_cascade.sc.
// =============================================================================

#include <pjv_sun_sky.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;

#define PI 3.14159265358979
#define FOV 60.0

// World-unit scale factor. These shaders were tuned when a voxel was ~0.0727 world units
// (fit-to-extent voxelizer). The voxelizer now emits voxelScale = 1 (1 voxel = 1 world unit),
// so the scene is ~13.76x larger in world units. Every world-space DISTANCE constant below is
// multiplied by WORLD_SCALE to preserve the same look (voxel-relative and step-count constants
// are left alone). Effectively this re-expresses the old constants in voxels.
#ifndef WORLD_SCALE
#define WORLD_SCALE 13.76
#endif

// ---- Cascade constants -----------------------------------------------------
#define NUM_CASCADES     4
#define BASE_TILE        8         // T0; Tc = BASE_TILE << c  -> 4,8,16,32.
                                   // T0=4 => cascade 0 has 16 directions -> the final
                                   // per-pixel integration samples the hemisphere finely
                                   // enough to show directional sky occlusion + clear bounce
                                   // (T0=2 gave only ~2 hemisphere dirs -> flat, no AO).
#define CASCADE_BASE_LEN (0.6 * WORLD_SCALE)  // L0: world length of cascade 0's ray interval.
#define SKY_GI           1         // 1 = sky is a GI light source; 0 = sun + its bounces only
                                   // (escaped rays carry no sky -> no flat sky on surfaces).
#define MARCH_STEPS      24        // Screen-space march samples per gather ray.
#define NORMAL_BIAS      (0.01 * WORLD_SCALE)  // Push ray start off the surface to avoid self-hit.
#define DEPTH_THICKNESS_SCALE 4.0  // Occluder crossing thickness, in units of the step size.

// ---- World-space probe quantization (renderer 6) ---------------------------
// Snap a probe's world position to a fixed world-space grid so the SAME world location
// maps to the SAME probe sample regardless of camera motion. Screen-anchored probes drift
// sub-voxel every frame as the camera moves, which jitters the gathered radiance and makes
// temporal accumulation reproject a moving target; snapping to a stable world grid removes
// that drift so reprojection just re-finds the same converged cell.
//
// The grid is a FIXED 1 voxel for EVERY cascade -- it must NOT grow with the cascade. A cell
// larger than a voxel snaps the probe more than half a voxel along the surface, which on any
// non-planar geometry (diagonal/staircase walls, window recesses, vertical relief) pushes the
// gather-ray origin into a neighbouring SOLID voxel -> the DDA reports a "started inside"
// degenerate hit -> opaque black -> big black bands (an earlier per-cascade `* 2^c` scaling did
// exactly this). At <= 1 voxel the tangent-plane projection below just snaps the probe to its own
// voxel-face centre and can never cross into an adjacent voxel, so the origin stays on the surface.
#define PROBE_QUANT_BASE 0.00000000000001       // World grid cell (voxels), same for all cascades.
// Bilateral (geometry-aware) probe interpolation. Rejecting/attenuating neighbour probes that
// sit on a different surface stops radiance leaking across depth discontinuities and around
// corners (the main light-leak source). Higher = sharper off-plane rejection.
// Off-plane distance (planar) is a WORLD distance, so it grows with WORLD_SCALE; the rejection
// term PLANE_LEAK_SCALE * planar^2 must shrink by WORLD_SCALE^2 to keep the same geometric cutoff.
#define PLANE_LEAK_SCALE (800.0 / (WORLD_SCALE * WORLD_SCALE))

int   tileSizeOf(int c)   { return BASE_TILE << c; }        // Tc (screen-space renderer only)
float pow4(int c)         { return float(1 << (2 * c)); }   // 4^c
float cascadeStart(int c) { return CASCADE_BASE_LEN * (pow4(c) - 1.0) / 3.0; }
float cascadeLen(int c)   { return CASCADE_BASE_LEN * pow4(c); }

// ---- Decoupled probe-spacing / direction-tile layout (world-space renderer) ----------------
// The single tile size Tc above is BOTH the probe spacing AND the direction-grid dimension, so
// directions = spacing^2 (d = s^2) and every cascade lands at exactly full-res -- there is no way
// to shrink it. Decoupling the two (independent probe spacing s and direction tile D, with D < s)
// makes a cascade's footprint (W/s)(H/s) * D^2 = W*H*(D/s)^2, i.e. only (D/s)^2 of the texture, so
// the cascade renders into a corner and the rest is skipped -> the actual radiance-cascade cost
// reduction. Both s and D double per cascade (textbook RC: 1/4 the probes, 4x the directions), so
// every cascade uses the SAME (W*D0/s0)x(H*D0/s0) sub-rectangle.
// s0/D0 are #ifndef-guarded so a renderer can override them by #define-ing BEFORE including this
// header (renderer 6, the per-face variant, uses a COARSER s0 for far fewer probes/gather rays --
// its per-face temporal accumulate keeps the sparse result clean). All passes of a given renderer
// that touch the probe layout (cascades, resolve, compose) MUST agree on these two values.
#ifndef PROBE_SPACING0
#define PROBE_SPACING0 8   // s0: cascade-0 probe spacing, screen px (doubles per cascade)
#endif
#ifndef DIR_TILE0
#define DIR_TILE0      4   // D0: cascade-0 direction-tile dim (D0xD0 dirs; doubles per cascade).
                           // MUST be < PROBE_SPACING0 to shrink below full-res. 4 -> 16 dirs at
                           // c0 and region = (4/8)^2 = 1/4 of the texture (~4x fewer gather rays).
                           // Knob for near-field angular resolution (raise if GI looks flat / lacks
                           // directional AO -- but keep it < PROBE_SPACING0).
#endif
int probeSpacingOf(int c) { return PROBE_SPACING0 << c; }  // s_c
int dirTileOf(int c)      { return DIR_TILE0 << c; }        // D_c

// Snap P to this cascade's world grid, then project the snapped point back onto the surface's
// tangent plane (through P, normal N) so the probe stays ON the surface instead of being buried
// in a wall or lifted into the air by the snap -- important because the gather ray starts here.
// For axis-aligned voxel faces N is an axis, so this snaps the two in-face coordinates and keeps
// the face's own axis coordinate exact.
vec3 quantizeProbePos(vec3 P, vec3 N, int c) {
    float s = PROBE_QUANT_BASE;              // fixed 1-voxel grid (see note above -- do NOT scale)
    vec3  g = (floor(P / s) + 0.5) * s;      // nearest cell centre
    return g - N * dot(N, g - P);            // project back onto the tangent plane
}

// Geometry similarity weight of a neighbour probe for bilateral interpolation, relative to the
// surface being shaded (P,N). probeValid < 0 marks a sky/background probe (no surface) -> 0.
// Combines a sharp normal term (rejects perpendicular faces at corners) with an off-plane
// distance term (rejects surfaces at a different depth / behind an occluder edge). In-plane
// distance is deliberately NOT penalised here -- the bilinear weight already handles that.
float geomWeight(vec3 P, vec3 N, vec3 probeP, vec3 probeN, float probeValid) {
    if (probeValid < 0.0) return 0.0;
    float nw = max(dot(N, probeN), 0.0);
    nw *= nw; nw *= nw;                                   // ^4: sharp corner cutoff
    float planar = abs(dot(probeP - P, N));              // off-plane distance (voxels)
    float dw = 1.0 / (1.0 + PLANE_LEAK_SCALE * planar * planar);
    return nw * dw;
}

// ---- Octahedral direction mapping ------------------------------------------
// Tile coord (u,v) in [0,1]^2 -> unit direction on the whole sphere. Using the SAME
// mapping at every cascade makes a cascade-c direction's four children in c+1 be exactly
// the 2x2 sub-tile, so merging is a clean angular refinement.
vec3 octDecode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Direction for tile coord (dx,dy) within a Tc x Tc tile, rotated per frame by a sub-tile
// jitter so temporal accumulation can smooth the coarse angular banding.
vec3 dirForTile(ivec2 tile, int Tc, vec2 jitter) {
    vec2 uv = (vec2(tile) + 0.5 + jitter) / float(Tc);
    uv = clamp(uv, vec2(0.001), vec2(0.999));
    return octDecode(uv);
}

// Per-frame sub-tile jitter shared by every texel of a cascade this frame.
vec2 cascadeJitter() {
    return fract(frameCount.x * vec2(0.7548776662, 0.5698402910)) - 0.5;
}

// ---- Camera projection (world -> screen UV), exact inverse of rayStartDirection. ----
vec2 worldToUV(vec3 P, vec3 camPos, vec3 camDirV, vec2 res, float fov, out bool valid) {
    vec3 forward = normalize(camDirV);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));
    vec3 v = P - camPos;
    float z = dot(v, forward);
    if (z <= 1e-4) { valid = false; return vec2(-1.0); }
    float scale  = tan(radians(fov * 0.5));
    float aspect = res.x / res.y;
    float ndcx = (dot(v, right) / z) / (scale * aspect);
    float ndcy = (dot(v, up)    / z) / scale;
    vec2 flip = (vec2(ndcx, ndcy) + 1.0) * 0.5;
    vec2 uv = vec2(flip.x, 1.0 - flip.y);
    valid = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}
