// =============================================================================
// reconstruct_common.sc  --  Rebuilding the full-resolution viewport G-buffer from albedo_low.frag's voxels.
//
// Shared by the two passes that do it: reconstruct.frag answers every pixel it can prove and marks the
// rest, and reconstruct_trace.frag casts the real ray for exactly those. Split in two because a shader is
// allocated registers for everything it contains: with the march compiled into the rebuild, the ~99% of
// pixels that never trace ran at the march's occupancy, and the few that did held every wave they were in
// for a full traversal. Measured on DamagedHelmet_1024 at 2052x1308, that made the one-pass version
// SLOWER than tracing every pixel (19.9 ms against 14.9 ms a frame).
//
// While the camera moves the viewport traces fewer rays than it has pixels, and this pass fills in the
// rest. It writes exactly the four targets albedo.frag does, so shade, accumulate and display cannot
// tell which of the two produced the frame -- and when the camera stops, albedo.frag takes over again
// and the image it converges to is the full-resolution one, unchanged.
//
// ---- Why this can be exact rather than approximate ---------------------------------------------
//
// An upscaler for a lit image has to interpolate, because the colour varies continuously across a
// surface and the samples only know it at points. The viewport's colour does not vary: it is the
// voxel's stored albedo, constant across the whole voxel, and a voxel is a box. So one sample of a
// voxel describes all of it -- and a full-resolution pixel does not need to blend its neighbours'
// colours at all. It rebuilds its OWN ray (same camera, same jitter as albedo.frag would have used)
// and intersects it with the boxes its neighbours hit. The nearest box it actually passes through is
// the surface it would have hit, the entry point is the exact hit position, the entry face is the
// exact normal, and the albedo is that voxel's. Every edge lands on the full-resolution pixel grid,
// because each edge is decided by a ray-box test at full resolution, not by the coarse grid.
//
// This is the selection idea from the advanced renderer's upscale.frag, made exact: each candidate is
// the one FACE a coarse ray entered, tested as the face of a box in its chunk's own rotated frame, so
// the hit position and normal are the march's rather than an interpolation of them.
//
// ---- When it cannot know, it traces --------------------------------------------------------------
//
// The rebuild can only choose among voxels some coarse ray found. It is wrong exactly when the voxel
// this pixel should see was hit by none of them: something narrower than the coarse spacing. Two
// tests catch the cases where that is possible, and those pixels cast the real full-resolution ray
// (the same query as albedo.frag) instead of trusting the neighbourhood:
//
//   * THE RAY HITS NONE OF THE CANDIDATES, but some of them were geometry. The pixel is on a
//     silhouette, and what lies behind it -- sky or something farther away -- was not necessarily
//     sampled.
//   * THE VOXEL IT DID HIT IS SMALL ON SCREEN: under TRUST_SPACINGS coarse-sample spacings across.
//     A voxel at least one spacing across always contains a coarse sample point, and so do its
//     same-sized neighbours on the same surface -- so a large voxel's neighbourhood is fully known.
//     A small one's is not: the voxel next to it may have fallen between the samples. Margin above 1
//     covers perspective and partial occlusion shrinking a voxel's visible footprint below its size.
//
// A pixel whose whole neighbourhood saw sky is sky. That is the one case not traced, and the one way
// this differs from the full march while moving: a feature thinner than the coarse spacing, alone
// against the sky, can be missed for the frames the camera is in motion.
//
// reconstructParams.x selects a debug view: 1 tints the traced fallback pixels red, 2 traces EVERY
// pixel as well and paints any disagreement with the reconstruction magenta -- the check that this is
// exact, rather than an assumption that it is. Measured with 2 at 2052x1308, camera moving: 0 of 2.7M
// pixels disagree on StonehillCastle and DamagedHelmet_1024, 4 on Bistro from inside.
// =============================================================================
#define EDITOR_ALBEDO_LEAN 1
#define EDITOR_ALBEDO_NO_MAIN 1
#include "albedo.frag"

// Slots 0 and 1 in both passes: each lists FBO 7 as its first input.
SAMPLER2D(lowMaterial, 0);   // (albedo.rgb, (headerIndex + 1) * 8 + face), 0 = miss. See albedo_low.frag.
SAMPLER2D(lowSurface,  1);   // (voxel centre, edge length), world space.

uniform vec4 passInputRes[8];       // [0] is the coarse G-buffer's (w, h, 1/w, 1/h).
uniform vec4 reconstructParams;     // x = debug view, y = TRUST_SPACINGS override (0 = default).

#define TRUST_SPACINGS_DEFAULT 1.5

// A chunk's rotation, from the one header texel that holds it. headers() would fetch all five, and this
// runs for up to nine candidates per pixel; consecutive candidates almost always share a chunk, which
// the caller exploits too.
mat3 chunkRotation(int headerIndex) {
    int headersPerRow = textureSize(headerData, 0).x / 5;
    int row = headerIndex / headersPerRow;
    int column = (headerIndex - row * headersPerRow) * 5;
    uvec4 q = texelFetch(headerData, ivec2(column + 3, row), 0);
    return rotationFromQuat(vec4(uintBitsToFloat(q.r), uintBitsToFloat(q.g),
                                 uintBitsToFloat(q.b), uintBitsToFloat(q.a)));
}

// Edge length of one voxel at distance t along this pixel's ray, in pixels of this pass's target.
float projectedPixels(float size, float t, vec3 direction) {
    if (cameraProjection.x > 0.5) {
        return size * passTargetRes.y / max(cameraProjection.y, 1e-6);
    }
    float depth = max(t * dot(direction, normalize(cameraDir.xyz)), 1e-6);
    return size * (passTargetRes.y * 0.5 / tan(radians(FOV * 0.5))) / depth;
}

// The five targets albedo.frag writes, in its layout. Built by value and written once at the end of
// main: bgfx's HLSL path turns gl_FragData into main's output parameters, so no helper can write them.
struct ViewportGBuffer {
    vec4 color;      // albedo, a = 1 on geometry
    vec4 normal;     // face normal, a = voxel edge length
    vec4 position;   // world hit, a = 1 on geometry
    vec4 glow;       // always zero on the plain path
    float distance;  // along the primary ray, 0 on background
};

ViewportGBuffer backgroundTexel(Ray ray) {
    ViewportGBuffer g;
    g.color = vec4(backgroundColor(ray.direction), 0.0);
    g.normal = vec4(0.0, 0.0, 0.0, 0.0);
    g.position = vec4(0.0, 0.0, 0.0, 0.0);
    g.glow = vec4(0.0, 0.0, 0.0, 0.0);
    g.distance = 0.0;
    return g;
}

ViewportGBuffer surfaceTexel(vec3 albedo, vec3 normal, float size, Ray ray, float t) {
    vec3 position = ray.origin + ray.direction * t;
    ViewportGBuffer g;
    g.color = vec4(albedo, 1.0);
    g.normal = vec4(normal, size);
    g.position = vec4(position, 1.0);
    g.glow = vec4(0.0, 0.0, 0.0, 0.0);
    g.distance = t;
    return g;
}

bool isMiss(SceneIntersectData hit) {
    return hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(hit.normal, hit.normal) < 0.5;
}

// ---- THE ONE CASE A SEEN FACE CAN STILL BE WRONG: A STEP NOBODY SAMPLED ------------------------
//
// A surface that is not axis-aligned voxelizes into a staircase, and each step shows a front face and
// a thin side face. Seen at an angle, the side faces are narrower than the coarse spacing and go
// unsampled. A pixel whose ray truly hits step W's side face then passes THROUGH W, out into the empty
// cell in front of the next step V, and into V's front face -- a face that is genuinely visible and
// was sampled, so the face test above lets it through. The answer is one voxel off, a sliver wide.
//
// Whatever hid the face this pixel reached must sit in the one-voxel layer directly in front of it,
// on the stretch of that layer the ray crossed on its way in -- for a step, that is all it can be. So
// that stretch is walked cell by cell and each cell probed in the tree: an occupancy lookup, not a
// march. Any solid cell means the pixel is traced. So does a stretch longer than FRONT_LAYER_MAX_CELLS
// (a face at a grazing angle -- nothing to save there anyway), a cell outside the chunk (a neighbour
// chunk could fill it), and a coarse LOD hit (whose cell is not one voxel).
#define FRONT_LAYER_MAX_CELLS 3.0
#define OCCLUDER_DEPTH_VOXELS 2.0

bool occludedInFrontLayer(int headerIndex, vec3 centre, float size, int face, vec3 localEntry, vec3 localDirection) {
    int axis = face / 2;
    float outward = (face - axis * 2) == 1 ? 1.0 : -1.0;
    float dNormal = axis == 0 ? localDirection.x : (axis == 1 ? localDirection.y : localDirection.z);
    // In cell units from here on: one cell is 1.0.
    vec3 entry = localEntry / size;
    vec3 back = -localDirection / max(abs(dNormal), 1e-6);   // Back along the ray, one layer of depth.
    int ta = axis == 0 ? 1 : 0;
    int tb = axis == 2 ? 1 : 2;
    vec2 start = vec2(entry[ta], entry[tb]);
    vec2 delta = vec2(back[ta], back[tb]);
    if (max(abs(delta.x), abs(delta.y)) > FRONT_LAYER_MAX_CELLS) return true;

    // Most rays never leave the cell directly in front of the face they enter -- which is empty, or the
    // face could not have been seen -- so there is nothing to probe and no header to fetch.
    vec2 position2 = start + 0.5;               // Cell (0,0) spans [0,1) on both axes.
    ivec2 cell = ivec2(floor(position2));
    vec2 end = position2 + delta;
    ivec2 endCell = ivec2(floor(end));
    if (cell.x == endCell.x && cell.y == endCell.y) return false;

    chunkHeader header = headers(headerIndex);
    float voxelSize = header.scale / float(header.resolution);
    if (abs(size - voxelSize) > voxelSize * 1e-3) return true;   // A coarse LOD node, not a voxel.

    // V's integer cell, recovered from its centre. Rounding a value that is within float error of an
    // integer plus one half is exact for any chunk resolution a float can address.
    mat3 rotation = rotationFromQuat(header.rotation);
    vec3 position = vec3(header.positionX, header.positionY, header.positionZ);
    vec3 cellCentre = ((centre - position) * rotation) / voxelSize;
    ivec3 voxel = ivec3(floor(cellCentre));
    ivec3 normalStep = ivec3(0, 0, 0);
    normalStep[axis] = int(outward);

    // 2D DDA over the tangent plane, from the entry point (cell 0,0 -- the empty cell in front of the
    // seen face) to where the ray crossed the layer's outer plane.
    ivec2 stepDirection = ivec2(delta.x > 0.0 ? 1 : -1, delta.y > 0.0 ? 1 : -1);
    vec2 safeDelta = vec2(abs(delta.x) < 1e-9 ? 1e-9 : delta.x, abs(delta.y) < 1e-9 ? 1e-9 : delta.y);
    vec2 tDelta = abs(1.0 / safeDelta);
    vec2 nextBoundary = vec2(stepDirection.x > 0 ? float(cell.x + 1) : float(cell.x),
                             stepDirection.y > 0 ? float(cell.y + 1) : float(cell.y));
    vec2 tNext = (nextBoundary - position2) / safeDelta;
    for (int i = 0; i < 8; i++) {
        if (cell.x != 0 || cell.y != 0) {
            ivec3 probe = voxel + normalStep;
            probe[ta] += cell.x;
            probe[tb] += cell.y;
            if (any(lessThan(probe, ivec3(0, 0, 0))) ||
                any(greaterThanEqual(probe, ivec3(int(header.resolution))))) return true;
            uint unusedOffset;
            if (pjvProbeTree(header.geometryStartIndex, header.resolution, probe, unusedOffset)) return true;
        }
        if (cell.x == endCell.x && cell.y == endCell.y) break;
        if (tNext.x < tNext.y) { cell.x += stepDirection.x; tNext.x += tDelta.x; }
        else                   { cell.y += stepDirection.y; tNext.y += tDelta.y; }
        if (min(tNext.x, tNext.y) > 1.0 && (cell.x != endCell.x || cell.y != endCell.y)) {
            // Float disagreement between the walk and the end cell: finish on the end cell.
            cell = endCell;
        }
    }
    return false;
}

// What the neighbourhood says about one pixel's ray.
struct Reconstruction {
    bool  trusted;   // The answer below is provably the march's.
    bool  allSky;    // Every neighbour saw background: this pixel is background too.
    float t;
    vec3  normal;
    vec3  albedo;
    float size;
};

// This pass's own ray, exactly as albedo.frag would cast it for this pixel.
// uv is the pixel's v_texcoord0, passed in: bgfx's HLSL path makes varyings main's parameters.
Ray reconstructionRay(vec2 uv) {
    return primaryRay(uv + viewportJitter() / passTargetRes.xy);
}

Reconstruction reconstructPixel(Ray ray, vec2 uv) {
    ivec2 lowSize = ivec2(passInputRes[0].xy + 0.5);
    // The coarse texel whose centre is nearest this pixel's, and its 3x3 ring. With the coarse grid at
    // half resolution that reaches 1.5 coarse texels -- three full pixels -- each way.
    ivec2 nearest = ivec2(floor(uv * passInputRes[0].xy));

    float bestT = 1e30;
    vec3  bestNormal = vec3(0.0, 0.0, 0.0);
    vec3  bestAlbedo = vec3(0.0, 0.0, 0.0);
    float bestSize = 0.0;
    int   bestHeader = -1;
    int   bestFace = 0;
    vec3  bestCentre = vec3(0.0, 0.0, 0.0);
    vec3  bestLocalEntry = vec3(0.0, 0.0, 0.0);   // Entry point relative to the voxel centre, chunk axes.
    vec3  bestLocalDirection = vec3(0.0, 0.0, 1.0);
    int   geometryCandidates = 0;
    float nearestCandidate = 1e30;   // Nearest any candidate's voxel could possibly be.
    int   cachedHeader = -1;
    mat3  rotation = mat3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 texel = clamp(nearest + ivec2(dx, dy), ivec2(0, 0), lowSize - 1);
            vec4 material = texelFetch(lowMaterial, texel, 0);
            if (material.w < 0.5) continue;
            geometryCandidates++;
            vec4 surface = texelFetch(lowSurface, texel, 0);

            int packed = int(material.w + 0.5);
            int headerIndex = packed / 8 - 1;
            int seenFace = packed - (packed / 8) * 8;
            // How near this candidate's voxel can possibly be: its centre less half its diagonal.
            float nearest = length(surface.xyz - ray.origin) - 0.8661 * surface.w;
            nearestCandidate = min(nearestCandidate, nearest);
            if (headerIndex != cachedHeader) {
                rotation = chunkRotation(headerIndex);
                cachedHeader = headerIndex;
            }

            // Slab test in the voxel's own frame, centred on it. v * R is transpose(R) * v: into the
            // chunk's axes. The ray direction is unit length and rotation preserves that, so t comes
            // back in world units directly.
            vec3 origin = (ray.origin - surface.xyz) * rotation;
            vec3 direction = ray.direction * rotation;
            vec3 safeDirection = vec3(abs(direction.x) < 1e-12 ? 1e-12 : direction.x,
                                      abs(direction.y) < 1e-12 ? 1e-12 : direction.y,
                                      abs(direction.z) < 1e-12 ? 1e-12 : direction.z);
            vec3 inverse = 1.0 / safeDirection;
            float halfSize = surface.w * 0.5;
            vec3 t0 = (vec3(-halfSize, -halfSize, -halfSize) - origin) * inverse;
            vec3 t1 = (vec3( halfSize,  halfSize,  halfSize) - origin) * inverse;
            vec3 tNear = min(t0, t1);
            vec3 tFar = max(t0, t1);
            float tEnter = max(max(tNear.x, tNear.y), tNear.z);
            float tExit = min(min(tFar.x, tFar.y), tFar.z);
            if (tEnter > tExit || tEnter <= 0.0 || tEnter >= bestT) continue;

            // The entry face is the slab entered last. Its outward normal faces back along the ray.
            vec3 localNormal;
            int entryFace;
            if (tEnter == tNear.x) {
                localNormal = vec3(direction.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
                entryFace = direction.x > 0.0 ? 0 : 1;
            } else if (tEnter == tNear.y) {
                localNormal = vec3(0.0, direction.y > 0.0 ? -1.0 : 1.0, 0.0);
                entryFace = direction.y > 0.0 ? 2 : 3;
            } else {
                localNormal = vec3(0.0, 0.0, direction.z > 0.0 ? -1.0 : 1.0);
                entryFace = direction.z > 0.0 ? 4 : 5;
            }
            // ONLY THROUGH A FACE A COARSE RAY SAW. A voxel's box has six faces and most of them are
            // buried against its neighbours. A ray that crosses a surface at a voxel nobody sampled
            // carries on a fraction of a voxel into the solid and can clip the side of a sampled
            // neighbour -- and that side is internal: nothing can ever see it, but the box test says
            // "hit", one voxel off, behind the real surface. On a wall seen at a grazing angle, where
            // faces are squeezed thinner than the coarse spacing, that was a stripe of wrong pixels.
            // A face some ray did enter is a visible face, so it cannot be one of those.
            if (entryFace != seenFace) continue;

            bestT = tEnter;
            bestNormal = rotation * localNormal;
            bestAlbedo = material.rgb;
            bestSize = surface.w;
            bestHeader = headerIndex;
            bestFace = entryFace;
            bestCentre = surface.xyz;
            bestLocalEntry = origin + direction * tEnter;
            bestLocalDirection = direction;
        }
    }

    bool found = bestT < 1e29;
    float trustSpacings = reconstructParams.y > 0.0 ? reconstructParams.y : TRUST_SPACINGS_DEFAULT;
    float spacingPixels = passTargetRes.y * passInputRes[0].w;   // Full pixels per coarse texel.
    // A neighbour standing clearly in front of the chosen hit -- more than OCCLUDER_DEPTH_VOXELS nearer --
    // means this pixel is beside that neighbour's silhouette, and a sliver of whatever casts it,
    // narrower than the coarse spacing, could cover this pixel unsampled: foliage in front of a wall,
    // a cornice over a window. occludedInFrontLayer only reaches the one layer directly in front of the
    // face, which is where a staircase's own steps sit; anything farther forward is caught here. These
    // pixels are traced.
    bool occluderNearby = nearestCandidate < bestT - OCCLUDER_DEPTH_VOXELS * bestSize;
    bool trusted = found && !occluderNearby &&
                   projectedPixels(bestSize, bestT, ray.direction) >= trustSpacings * spacingPixels &&
                   !occludedInFrontLayer(bestHeader, bestCentre, bestSize, bestFace, bestLocalEntry, bestLocalDirection);
    Reconstruction r;
    r.trusted = trusted;
    r.allSky = geometryCandidates == 0;
    r.t = bestT;
    r.normal = bestNormal;
    r.albedo = bestAlbedo;
    r.size = bestSize;
    return r;
}
