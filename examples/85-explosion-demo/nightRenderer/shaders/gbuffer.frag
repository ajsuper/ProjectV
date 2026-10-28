$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer.frag -- pass 1, at RENDER resolution (0.4 of the window on each axis by default).
//
// One primary ray per pixel and one sun shadow ray, both through the SAME scene query in a loop of one
// or two. The march is enormous once inlined, and a second call site makes every ray in the shader
// slower; the loop's bound comes from a uniform so the compiler cannot unroll it back into two copies.
// (The physics demos' renderer, which this one grew from, measured both.)
//
// ---- WHY raySceneIntersectFrom AND NOT raySceneIntersect ------------------------------------------
// raySceneIntersect is the engine's full query: it wraps the march in a loop that peels transparent
// layers, bends at refracting interfaces and resolves materials, and all of that is compiled into the
// shader whether a ray asks for it or not. raySceneIntersectFrom is the bare nearest-solid-voxel query
// underneath it. Measured here, at 0.4 scale:
//
//                                    Cave meadow    SanMiguel courtyard
//     raySceneIntersect               2.19 ms        4.74 ms
//       ...with transparency off      2.11 ms        4.67 ms
//     raySceneIntersectFrom           1.21 ms        2.60 ms
//
// So the cost was never the transparency a scene uses -- it is the machinery's mere presence. The
// march counters say why it matters so much: a ray here takes only about ten tree steps across one or
// two chunks (see GRR_STATS below), so fixed per-ray work and the shader's size are most of the bill.
// What this gives up is see-through voxels: every voxel is opaque, to the camera and to the sun.
//
// Outputs (FBO 1):
//   0 gPos     xyz world hit, w = distance along the ray (w < 0: sky)
//   1 gNormal  xyz face normal, w = the hit voxel's edge length
//   2 gAlbedo  rgb surface colour (sky: the sky's colour), a = emission, as a multiple of the albedo
//   3 gLight   x = sun visibility (0 or 1), y = n . sun
//   4 gFace    xyz the voxel FACE's centre, w = voxel edge length (0: sky)
//   5 gRot     the chunk's rotation quaternion -- the voxel's own axes, for the reconstruction
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_random.sc>     // pjvHalton
// Nothing in this renderer animates, so the engine's animated march -- a second tree march inlined
// into every scene query -- is left out of the shader. Its size alone costs occupancy.
#define PJV_NO_ANIMATION
// GRR_STATS -- a measurement build, never the shipped one. 1 writes the PRIMARY ray's march counters
// into gAlbedo (r tree-loop iterations, g chunks marched, b grid cells visited, a = 1 if it hit), 2 the
// SHADOW ray's (a = 1 where one was cast). Build with it defined and read the target back with
// GAME_CAPTURE_TEXTURE=3. Measured on the default views it is ~10 iterations over 1-2 chunks a ray.
#ifndef GRR_STATS
#define GRR_STATS 0
#endif
#if GRR_STATS
#define PJV_MARCH_STATS
#endif
#include <pjv_utils_DDA.sc>
#include <grr_common.sc>

// ---- The cloud's embers ------------------------------------------------------------------------------
// The mushroom cloud's embers are not scene components -- thousands of small moving chunks are what
// this renderer's scene structure is slowest at -- but voxel lumps the CPU lists every frame, binned into
// a coarse grid. A lump is a turned 3x3x3 block of voxels with some missing (a pattern of its own, from
// its seed), so it is rough rather than a perfect cube. The primary ray walks the grid (a DDA) up to the
// voxel scene's hit, and inside each lump it meets, walks the lump's 27 voxels; the nearest voxel hit
// wins, and everything after the G-buffer -- AO, GI, the shade, the voxel reconstruction, TAA -- treats
// it as a voxel like any other.
SAMPLER2D(emberCubes, 0);   // four texels a lump: (centre, half edge), (rotation quaternion), (colour, glow), (seed)
SAMPLER2D(emberCells, 1);   // a texel a grid cell: (first entry in emberIndex, how many)
SAMPLER2D(emberIndex, 2);   // lump numbers, cell by cell
uniform vec4 emberGrid;     // xyz the grid's min corner, w the cell's edge
uniform vec4 emberInfo;     // x cells a side, y emberCells' width, z emberCubes' and emberIndex's width, w on

#define EMBER_STEPS    200
#define EMBER_PER_CELL 256

ivec2 texelOf(int index, int width) { return ivec2(index - (index / width) * width, index / width); }

float lumpHash(vec3 g, float seed) { return fract(sin(dot(g + seed * 17.13, vec3(12.9898, 78.233, 37.719))) * 43758.5453); }

// Whether voxel g (0..2 each way) of a lump with this seed is there: the core always, the faces' middles
// mostly, the edges often, the corners seldom.
bool lumpSolid(vec3 g, float seed) {
    vec3 e = abs(g - 1.0);
    float outer = e.x + e.y + e.z;
    if (outer < 0.5) return true;
    return lumpHash(g, seed) > (outer > 2.5 ? 0.72 : outer > 1.5 ? 0.42 : 0.15);
}

// The nearest lump voxel along o + t d with t < tLimit: its distance, face normal, lump and voxel.
bool traceEmbers(vec3 o, vec3 d, float tLimit, out float tHit, out vec3 nHit, out int cubeHit, out vec3 voxelHit) {
    tHit = tLimit;
    nHit = vec3(0.0, 1.0, 0.0);
    cubeHit = -1;
    voxelHit = vec3(1.0, 1.0, 1.0);
    if (emberInfo.w < 0.5) return false;
    float N = emberInfo.x, L = emberGrid.w;
    int cellsWide = int(emberInfo.y), dataWide = int(emberInfo.z);
    vec3 safe = vec3(abs(d.x) < 1e-6 ? 1e-6 : d.x, abs(d.y) < 1e-6 ? 1e-6 : d.y, abs(d.z) < 1e-6 ? 1e-6 : d.z);
    vec3 t0 = (emberGrid.xyz - o) / safe, t1 = (emberGrid.xyz + vec3(N, N, N) * L - o) / safe;
    vec3 tNear = min(t0, t1), tFar = max(t0, t1);
    float tEnter = max(max(tNear.x, tNear.y), max(tNear.z, 0.0));
    float tExit = min(min(tFar.x, tFar.y), min(tFar.z, tLimit));
    if (tExit <= tEnter) return false;

    vec3 u = clamp((o + d * (tEnter + 1e-4) - emberGrid.xyz) / L, vec3(0.0, 0.0, 0.0), vec3(N - 1e-3, N - 1e-3, N - 1e-3));
    vec3 c = floor(u);
    vec3 stepDir = vec3(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    vec3 tDelta = abs(vec3(L, L, L) / safe);
    vec3 tNext = tEnter + ((c + max(stepDir, vec3(0.0, 0.0, 0.0))) - u) * L / safe;
    bool found = false;
    float t = tEnter;
    for (int i = 0; i < EMBER_STEPS; i++) {
        if (t > tExit || t > tHit) break;
        if (any(lessThan(c, vec3(0.0, 0.0, 0.0))) || any(greaterThanEqual(c, vec3(N, N, N)))) break;
        int cell = int(c.z) * int(N) * int(N) + int(c.y) * int(N) + int(c.x);
        vec2 entry = texelFetch(emberCells, texelOf(cell, cellsWide), 0).xy;
        int first = int(entry.x), count = int(entry.y);
        for (int j = 0; j < EMBER_PER_CELL; j++) {
            if (j >= count) break;
            int cube = int(texelFetch(emberIndex, texelOf(first + j, dataWide), 0).x);
            vec4 a = texelFetch(emberCubes, texelOf(cube * 4, dataWide), 0);
            vec4 q = texelFetch(emberCubes, texelOf(cube * 4 + 1, dataWide), 0);
            mat3 R = rotationFromQuat(q);
            vec3 lo = (o - a.xyz) * R;   // into the lump's frame (transpose(R) times)
            vec3 ld = d * R;
            vec3 sl = vec3(abs(ld.x) < 1e-6 ? 1e-6 : ld.x, abs(ld.y) < 1e-6 ? 1e-6 : ld.y, abs(ld.z) < 1e-6 ? 1e-6 : ld.z);
            vec3 s0 = (-a.w - lo) / sl, s1 = (a.w - lo) / sl;
            vec3 sNear = min(s0, s1), sFar = max(s0, s1);
            float enter = max(max(sNear.x, sNear.y), sNear.z), leave = min(min(sFar.x, sFar.y), sFar.z);
            if (enter <= 0.0 || enter > leave || enter >= tHit) continue;
            // Inside the lump: a DDA over its 3x3x3 voxels from where the ray came in.
            float seed = texelFetch(emberCubes, texelOf(cube * 4 + 3, dataWide), 0).x;
            float sub = a.w * (2.0 / 3.0);
            vec3 axis = sNear.x >= sNear.y && sNear.x >= sNear.z ? vec3(1.0, 0.0, 0.0)
                      : sNear.y >= sNear.z ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
            vec3 inStep = vec3(ld.x >= 0.0 ? 1.0 : -1.0, ld.y >= 0.0 ? 1.0 : -1.0, ld.z >= 0.0 ? 1.0 : -1.0);
            vec3 pin = (lo + ld * enter + a.w) / sub;
            vec3 g = clamp(floor(pin), vec3(0.0, 0.0, 0.0), vec3(2.0, 2.0, 2.0));
            vec3 gDelta = abs(vec3(sub, sub, sub) / sl);
            vec3 gNext = enter + ((g + max(inStep, vec3(0.0, 0.0, 0.0))) - pin) * sub / sl;
            vec3 localNormal = -axis * inStep;
            float tv = enter;
            for (int k = 0; k < 7; k++) {
                if (tv >= tHit || tv > leave + 1e-4) break;
                if (lumpSolid(g, seed)) {
                    tHit = tv;
                    nHit = R * localNormal;
                    cubeHit = cube;
                    voxelHit = g;
                    found = true;
                    break;
                }
                if (gNext.x < gNext.y && gNext.x < gNext.z) { tv = gNext.x; gNext.x += gDelta.x; g.x += inStep.x; localNormal = vec3(-inStep.x, 0.0, 0.0); }
                else if (gNext.y < gNext.z)                 { tv = gNext.y; gNext.y += gDelta.y; g.y += inStep.y; localNormal = vec3(0.0, -inStep.y, 0.0); }
                else                                        { tv = gNext.z; gNext.z += gDelta.z; g.z += inStep.z; localNormal = vec3(0.0, 0.0, -inStep.z); }
                if (any(lessThan(g, vec3(0.0, 0.0, 0.0))) || any(greaterThan(g, vec3(2.0, 2.0, 2.0)))) break;
            }
        }
        float cellExit = min(min(tNext.x, tNext.y), tNext.z);
        if (found && tHit <= cellExit) return true;
        if (tNext.x < tNext.y && tNext.x < tNext.z) { t = tNext.x; tNext.x += tDelta.x; c.x += stepDir.x; }
        else if (tNext.y < tNext.z)                 { t = tNext.y; tNext.y += tDelta.y; c.y += stepDir.y; }
        else                                        { t = tNext.z; tNext.z += tDelta.z; c.z += stepDir.z; }
    }
    return found;
}

#include <pjv_engine.sc>   // passTargetRes, passInputRes: engine-set
// x = frame index, y = jitter amplitude in render pixels (0 = none), z = TAA on, w = camera moved
uniform vec4 frameParams;
// x = shadows on, y = AO on, z = reconstruction bypass, w = debug view
uniform vec4 renderParams;

// Where the shadow ray leaves the surface, in voxels. In voxels rather than world units because scenes
// span a thousand-fold range of scale, and a fixed epsilon is either a skipped occluder or a self-hit.
#define SHADOW_BIAS_VOXELS 0.25
#define PRIMARY_STEPS 768u
#define SHADOW_STEPS  256u
// The shadow ray coarsens to tree level 2 (16^3 blocks) over this many voxels away from the surface.
#define SHADOW_FINISH_LOD 2u
#define SHADOW_LOD_RAMP_VOXELS 32u
// The primary ray takes no LOD at all: at a reduced render scale "a node smaller than a render pixel"
// is several output pixels, and the merged blocks read as a visible change of detail with distance.

bool isMiss(SceneIntersectData h) {
    return h.foundBox.size < 0.0 || h.rayT <= 0.0 || dot(h.normal, h.normal) < 0.5;
}

void main() {
    // Sub-pixel jitter for TAA, off unless E turns it on (frameParams.y is then 1).
    int frame = int(frameParams.x);
    int phase = frame - (frame / 16) * 16 + 1;
    vec2 jitter = (vec2(pjvHalton(phase, 2), pjvHalton(phase, 3)) - 0.5) * frameParams.y;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = grrCameraRay(v_texcoord0 + jitter * passTargetRes.zw, passTargetRes.xy, cameraDir.xyz);
    vec3 primaryDirection = ray.direction;
    vec3 sun = normalize(sunDir.xyz);
    RayQuery query = pjvPrimaryQuery(PRIMARY_STEPS);

#if GRR_STATS
    vec4 statsOut = vec4(0.0, 0.0, 0.0, 0.0);
#endif
    SceneIntersectData hit;
    float visibility = 1.0, flashVisibility = 0.0;
    // Up to three rays through ONE call of the march: the primary, the moon's shadow, and the shadow
    // toward the explosion's light. `next` says which the loop traces after the current one.
    // 0 = done, 1 = moon shadow, 2 = flash shadow.
    int stage = 0;
    vec3 surface = vec3(0.0, 0.0, 0.0), n = vec3(0.0, 1.0, 0.0);
    float bias = 0.0;
    bool wantMoon = false, wantFlash = false;
    bool ember = false;
    float emberT = 0.0;
    vec3 emberNormal = vec3(0.0, 1.0, 0.0);
    int emberCube = -1;
    vec3 emberVoxel = vec3(1.0, 1.0, 1.0);
    for (int i = 0; i < 3; i++) {
        SceneIntersectData found = raySceneIntersectFrom(ray, query, 0.0);
        if (stage == 0) {
            hit = found;
#if GRR_STATS == 1
            statsOut = vec4(float(pjvStatSteps), float(pjvStatMarches), float(pjvStatVisits),
                            isMiss(found) ? 0.0 : 1.0);
#endif
            // An ember cube nearer than the scene's voxel wins.
            ember = traceEmbers(cameraPos.xyz, primaryDirection, isMiss(found) ? 1.0e9 : found.rayT, emberT, emberNormal, emberCube, emberVoxel);
            if (!ember && isMiss(found)) break;
            if (ember) {
                n = emberNormal;
                surface = cameraPos.xyz + primaryDirection * emberT;
                bias = SHADOW_BIAS_VOXELS * 0.3;
            } else {
                n = normalize(found.normal);
                surface = cameraPos.xyz + primaryDirection * found.rayT;
                bias = SHADOW_BIAS_VOXELS * found.foundBox.size;
            }
            wantMoon = renderParams.x > 0.5 && dot(n, sun) > 0.0;
            // The fire's shadow too only with shadows on; without, what faces the fire is lit by it.
            vec3 toFlash = flashParams.xyz - surface;
            bool facesFlash = flashParams.w > 0.0 && dot(n, toFlash) > 0.0;
            wantFlash = renderParams.x > 0.5 && facesFlash;
            flashVisibility = facesFlash && renderParams.x < 0.5 ? 1.0 : 0.0;
        } else if (stage == 1) {
            visibility = isMiss(found) ? 1.0 : 0.0;
        } else {
            flashVisibility = isMiss(found) ? 1.0 : 0.0;
        }
        // What to trace next.
        if (stage == 0 && wantMoon) {
            stage = 1;
            ray.origin = surface + n * bias;
            ray.direction = sun;
            query = pjvShadowQuery(SHADOW_STEPS, 1e6);
            float rampVoxels = float(SHADOW_LOD_RAMP_VOXELS) / max(dot(n, sun), 0.05);
            pjvQueryLinearLOD(query, SHADOW_FINISH_LOD, uint(rampVoxels));
        } else if (stage <= 1 && wantFlash) {
            stage = 2;
            vec3 toFlash = flashParams.xyz - surface;
            float dist = length(toFlash);
            ray.origin = surface + n * bias;
            ray.direction = toFlash / dist;
            // Stops short of the light: the glowing embers round it are the light, not its shadow.
            query = pjvShadowQuery(SHADOW_STEPS, max(dist - flashColor.w * 0.6, 0.0));
            wantFlash = false;
        } else {
            break;
        }
    }
    if (!wantMoon && (ember || !isMiss(hit))) visibility = renderParams.x > 0.5 ? 0.0 : 1.0;

    if (ember) {
        int wide = int(emberInfo.z);
        vec4 a = texelFetch(emberCubes, texelOf(emberCube * 4, wide), 0);
        vec4 q = texelFetch(emberCubes, texelOf(emberCube * 4 + 1, wide), 0);
        vec4 look = texelFetch(emberCubes, texelOf(emberCube * 4 + 2, wide), 0);
        float seed = texelFetch(emberCubes, texelOf(emberCube * 4 + 3, wide), 0).x;
        // The voxel: a third of the lump, its colour checkered and jittered as the old voxel chunks were.
        float sub = a.w * (2.0 / 3.0);
        vec3 voxelCentre = a.xyz + rotationFromQuat(q) * ((emberVoxel + 0.5) * sub - a.w);
        float checker = mod(emberVoxel.x + emberVoxel.y + emberVoxel.z, 2.0) < 0.5 ? 0.84 : 1.0;
        vec3 colour = look.rgb * checker * (0.9 + 0.2 * lumpHash(emberVoxel + 3.0, seed));
        gl_FragData[0] = vec4(surface, emberT);
        gl_FragData[1] = vec4(emberNormal, sub);
        gl_FragData[2] = vec4(colour, look.a);
        gl_FragData[3] = vec4(visibility, dot(emberNormal, sun), flashVisibility, 0.0);
        gl_FragData[4] = vec4(voxelCentre + emberNormal * (sub * 0.5), sub);
        gl_FragData[5] = q;
        return;
    }

    if (isMiss(hit)) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[2] = vec4(grrSkyWithSun(primaryDirection), 0.0);
#if GRR_STATS
        gl_FragData[2] = statsOut;
#endif
        gl_FragData[3] = vec4(1.0, 0.0, 0.0, 0.0);
        gl_FragData[4] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[5] = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 normal = normalize(hit.normal);
    vec3 P = cameraPos.xyz + primaryDirection * hit.rayT;
    // The geometry-only query resolves no material, so the hit's is fetched here: one descent, for the
    // primary ray only.
    VoxelMaterial material = fetchVoxelMaterialFromHit(hit);
    vec3 albedo = material.albedo;
    // Emission travels as a multiple of the albedo, so it fits the albedo target's spare channel: an
    // emitter's glow is its own colour, scaled.
    float albedoPeak = max(max(albedo.r, albedo.g), max(albedo.b, 0.04));
    float emission = max(max(material.emission.r, material.emission.g), material.emission.b) / albedoPeak;

    // The voxel's centre is its minimum LOCAL corner plus the half-diagonal turned by the chunk's
    // rotation, so the face centre is right for a turned chunk as well as an axis-aligned one.
    vec4 rotation = headers(int(hit.headerIndex)).rotation;
    float voxelSize = hit.foundBox.size;
    vec3 centre = hit.foundBox.position + rotationFromQuat(rotation) * vec3(voxelSize * 0.5);
    vec3 faceCentre = centre + normal * (voxelSize * 0.5);

    gl_FragData[0] = vec4(P, hit.rayT);
    gl_FragData[1] = vec4(normal, voxelSize);
    gl_FragData[2] = vec4(albedo, emission);
#if GRR_STATS
    gl_FragData[2] = statsOut;
#endif
    gl_FragData[3] = vec4(visibility, dot(normal, sun), flashVisibility, 0.0);
    gl_FragData[4] = vec4(faceCentre, voxelSize);
    gl_FragData[5] = rotation;
}
