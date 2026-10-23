// restir_common.sc -- what every pass of the restir-pt renderer shares: its uniforms, a hash RNG, the
// camera's inverse, the packings, and the ReSTIR PT reservoir -- a path, not a point -- with its layout.
//
// THE PIPELINE (render.json). "render" is the render resolution, half the window each way; "GI" is the
// path resolution, half that again (a sixteenth of the window's pixels; --restir-gi 2: a quarter).
//    1 gbuffer        render     camera ray + sun shadow ray: position, normal, albedo, material, emission,
//                                the sun's visibility and blocker, the face and rotation for the reconstruction
//    2 surface        GI         each GI pixel's visible point: one render texel of its block, compacted
//    3 sample         GI         a new path from it (three bounces, BSDF-sampled, NEE to the sun), the
//                                history's path validated (a quarter a frame) and shifted here: temporal reuse
//    4 spatial        GI         five neighbours' paths shifted here: spatial reuse; resolves to the diffuse
//                                and specular indirect light, each demodulated, and hands its reservoirs on
//    5 gi_accumulate  GI         temporal accumulation of both, with luminance moments
//    6-9 atrous*      GI         a-trous filter, steps 1, 2, 4, 8, both lobes; the specular by its roughness
//   10 ssao           render     screen-space ambient occlusion, in a fixed 4x4 pattern
//   10b shadow_soft   render     the sun through the BSDF (highlight included), its shadow softened
//   11 composite      render     edge-aware upsample of both: direct; diffuse x albedo + specular x its albedo
//   12 ao_apply       render     the SSAO on the indirect light, the sun, the fog; the views (G)
//   13 reconstruct    window     the voxel renderer's reconstruction: voxel by voxel, antialiased
//   14 resolve        window     temporal accumulation of the image
//   15-17             window     meter, exposure, depth of field; display: the post and the display transform
//
// A GI pixel takes its visible point from one render texel of its 2x2 block -- a different one each frame
// (restirJitter.xy), so over four frames every texel's surface is sampled.

#ifndef RESTIR_COMMON_SC
#define RESTIR_COMMON_SC

#include <pjv_path_light.sc>   // the sun and sky pt-reference renders by, and pjv_bsdf.sc

uniform vec4 passTargetRes;   // engine-set: xy = this pass's target size, zw = 1 / size
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 prevCameraPos;   // last frame's camera, for every temporal pass
uniform vec4 prevCameraDir;
uniform vec4 frameCount;      // x = frame index, y = 1 when the camera or the sun moved this frame, z = 1 when the sun did
uniform vec4 restirReuse;     // x temporal reuse, y spatial reuse, z visibility re-test, w temporal M cap
uniform vec4 restirDenoise;   // x a-trous on, y GI accumulation on, z view (G), w the diffuse GI history cap
uniform vec4 restirJitter;    // xy this frame's offset inside the block, z the GI stride in render texels (2; 1 for
                              // --restir-gi 2), w 1 = start every history over
uniform vec4 jitterParams;    // xy: where this frame's render texels are traced, off their centres, in texels
uniform vec4 restirLook;      // x SSAO radius (world units), y its strength on the indirect light, z the sun's
                              // penumbra width per unit of blocker distance (0: hard shadows), w the fog's
                              // density, per world unit (0: none)
uniform vec4 restirPath;      // x bounces after the visible point, y the sample pass's ray budget, z the spatial
                              // pass's, w validation: the relative change in a path's light that counts (0: off)
#define RESTIR_FOV 60.0       // the gallery's, as every renderer here

uniform vec4 restirSettings;  // x the G-buffer's trace loop bound (a uniform, so it is not unrolled), y surface
                              // offset (world units, a twentieth of the finest voxel), z the image's longest
                              // history (resolve), w one render texel's width per unit of distance

#define RESTIR_PI 3.14159265

// The paths see every surface as rough: glossiness is capped at RESTIR_GI_MAX_GLOSSINESS (GGX alpha 0.25),
// so every vertex can be reconnected at and a shift never has to re-trace a path -- ReSTIR GI's cost,
// with a specular lobe. What the cap loses is sharp reflection: surfaces at RESTIR_MIRROR_GLOSSINESS
// and above take theirs from the screen instead (composite.frag), the paths' blurred one where it misses.
#define RESTIR_GI_MAX_GLOSSINESS 0.5
#define RESTIR_MIRROR_GLOSSINESS 0.9

// The views (G, restirDenoise.z).
#define RESTIR_VIEW_IMAGE    0
#define RESTIR_VIEW_INDIRECT 1
#define RESTIR_VIEW_DIRECT   2
#define RESTIR_VIEW_AO       3
#define RESTIR_VIEW_DIFFUSE  4
#define RESTIR_VIEW_SPECULAR 5
bool restirView(int view) { return abs(restirDenoise.z - float(view)) < 0.5; }

// PCG hash (Jarzynski & Olano), one state word per pixel.
uint restirPcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float restirRandom(inout uint state) {
    return float(restirPcg(state) >> 8u) * (1.0 / 16777216.0);
}

// Where world point P lands on screen (uv, top-left origin) for a camera at `position` looking along
// `forward` -- the inverse of the engine's rayStartDirection, basis and flip included.
vec2 restirWorldToUV(vec3 P, vec3 position, vec3 forward, vec2 res, out bool inFront) {
    vec3 f = normalize(forward);
    vec3 worldUp = abs(f.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(f, worldUp));
    vec3 up = normalize(cross(right, f));
    vec3 d = P - position;
    float z = dot(d, f);
    inFront = z > 1e-4;
    float scale = tan(radians(RESTIR_FOV * 0.5));
    vec2 ndc = vec2(dot(d, right) / (z * scale * (res.x / res.y)), dot(d, up) / (z * scale));
    return vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
}

// An orthonormal basis round unit n.
void restirBasis(vec3 n, out vec3 t, out vec3 b) {
    t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    b = cross(n, t);
}

// Octahedral normal encoding (two floats in -1..1).
vec2 restirOctWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}
vec2 restirOctEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : restirOctWrap(n.xy);
}
vec3 restirOctDecode(vec2 e) {
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = restirOctWrap(n.xy);
    return normalize(n);
}

// Packings into one float each. A float32 holds every integer below 2^24 exactly, so up to 24 bits of
// fields go in as an integer: no bit casts, nothing a render target could flush or canonicalise.
//   a unit vector     octahedral, 12 bits a coordinate (0.05 degrees)
//   a colour 0..1     8 bits a channel, square-root encoded (dark albedo keeps its steps)
//   a material        glossiness and metallic, 8 bits each (as decodeMaterial reads them)
float restirPackDir(vec3 n) {
    uvec2 q = uvec2(clamp(restirOctEncode(n) * 0.5 + 0.5, 0.0, 1.0) * 4095.0 + 0.5);
    return float(q.x * 4096u + q.y);
}
vec3 restirUnpackDir(float f) {
    uint u = uint(f);
    return restirOctDecode(vec2(float(u >> 12u), float(u & 4095u)) / 4095.0 * 2.0 - 1.0);
}
float restirPackColour(vec3 c) {
    uvec3 q = uvec3(sqrt(clamp(c, 0.0, 1.0)) * 255.0 + 0.5);
    return float((q.r << 16u) | (q.g << 8u) | q.b);
}
vec3 restirUnpackColour(float f) {
    uint u = uint(f);
    vec3 s = vec3(float(u >> 16u), float((u >> 8u) & 255u), float(u & 255u)) / 255.0;
    return s * s;
}
float restirPackMaterial(float glossiness, float metallic) {
    return float(uint(clamp(glossiness, 0.0, 1.0) * 255.0 + 0.5) * 256u + uint(clamp(metallic, 0.0, 1.0) * 255.0 + 0.5));
}
vec2 restirUnpackMaterial(float f) {
    uint u = uint(f);
    return vec2(float((u >> 8u) & 255u), float(u & 255u)) / 255.0;
}

// The surface pass's two texels: surf0 = (x1, t1; t1 < 0 none), surf1 = (oct n1, material, albedo).
struct RestirSurface {
    vec3  x;
    float t;          // < 0: the sky, no surface
    vec3  n;
    vec3  albedo;
    float glossiness;
    float metallic;
};
RestirSurface restirSurface(vec4 a, vec4 b) {
    RestirSurface s;
    s.x = a.xyz;
    s.t = a.w;
    s.n = restirOctDecode(b.xy);
    vec2 m = restirUnpackMaterial(b.z);
    s.glossiness = m.x;
    s.metallic = m.y;
    s.albedo = restirUnpackColour(b.w);
    return s;
}

float restirLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// The camera ray through uv (0..1 across this pass's target, the jitter already added), built as the
// engine's rayStartDirection builds it.
vec3 restirViewDirection(vec2 uv) {
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float scale = tan(radians(RESTIR_FOV * 0.5));
    float aspect = passTargetRes.x / passTargetRes.y;
    vec3 forward = normalize(cameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(forward, worldUp));
    vec3 up = normalize(cross(right, forward));
    return normalize(right * (ndc.x * scale * aspect) + up * (ndc.y * scale) + forward);
}

// Distance fog: the light from a surface t away along d, seen through air (restirLook.w per unit). The air
// takes on the colour of the sky at the horizon in that direction -- so the far distance melts into the
// horizon it stands against -- warmed towards the sun. It starts a little way out (RESTIR_FOG_START of
// the fog's own length), so what is near stays crisp, and never quite hides anything (RESTIR_FOG_MAX).
#define RESTIR_FOG_START 0.1
#define RESTIR_FOG_MAX   0.85
vec3 restirFog(vec3 colour, vec3 d, float t) {
    if (restirLook.w <= 0.0 || t < 0.0) return colour;
    float amount = RESTIR_FOG_MAX * (1.0 - exp(-max(t * restirLook.w - RESTIR_FOG_START, 0.0)));
    vec3 horizon = pathSky(normalize(vec3(d.x, max(d.y, 0.0) * 0.5 + 0.02, d.z)));
    vec3 air = horizon + SUN_COLOR * 0.04 * pow(max(dot(d, normalize(SUN_DIR)), 0.0), 6.0);
    return mix(colour, air, amount);
}

// A ReSTIR PT reservoir: one path, kept as what a shift needs to rebuild it from another visible point,
// plus the weights (docs/plans/restir-pt.md). The path is x1 (the visible point) .. x4.
//
//   seed   its random numbers: each vertex draws from its own stream, hashed from the seed and the vertex's
//          index, so a REPLAY from another visible point takes the same decisions vertex by vertex
//   k      the reconnection vertex: the first rough vertex, more than a few voxels from a rough vertex
//          before it. 0: none (an all-glossy path), replayed whole. 2: x1 -> x2 is the reconnection --
//          on a diffuse scene every path is this, ReSTIR GI's case. 3 or 4: x1 (and x2) are glossy and
//          replayed, then reconnected -- a diffuse wall seen in a mirror
//   x, n, albedo, glossiness, metallic   x_k and its material, to evaluate its BSDF for a new incoming
//          direction; for a reconnection to the sky, a point 10^4 out along it
//   Le     x_k's emission (or the sky's radiance)
//   vis    whether x_k sees the sun (its next-event estimate, re-evaluated through the BSDF on a shift)
//   wk, L  the direction out of x_k and the light the rest of the path brought back along it. wk was
//          sampled cosine-weighted -- not from the BSDF, whose density would depend on the incoming
//          direction a shift changes -- so the reflected light is pi f(new in, wk) L, whatever x_{k-1} is
//   q      cos / d^2 at x_k seen from x_{k-1}, the reconnection's geometry: a shift's Jacobian is the new
//          one's over this (< 0: the sky, Jacobian 1)
//   M, W   the resampling weights: candidates seen, and the unbiased contribution weight
//   x1, n1 the visible point this reservoir was formed at (for the temporal pass's same-surface test)
//
// Layout, five RGBA32F texels:
//   0: x.xyz, q                    1: L.rgb, dir(n)              2: Le.rgb, dir(wk)
//   3: seed|k|vis, M|glossiness|metallic, W, colour(albedo)      4: x1.xyz, dir(n1) (-1: no surface)
struct Reservoir {
    uint  seed;
    int   k;
    vec3  x;
    vec3  n;
    vec3  albedo;
    float glossiness;
    float metallic;
    vec3  Le;
    float vis;
    vec3  wk;
    vec3  L;
    float q;
    float M;
    float W;
    float wSum;   // only while resampling
    vec3  x1;
    vec3  n1;
    bool  formed; // x1 is a surface
};

Reservoir restirEmpty() {
    Reservoir r;
    r.seed = 0u;
    r.k = 0;
    r.x = vec3(0.0, 0.0, 0.0);
    r.n = vec3(0.0, 1.0, 0.0);
    r.albedo = vec3(0.0, 0.0, 0.0);
    r.glossiness = 0.0;
    r.metallic = 0.0;
    r.Le = vec3(0.0, 0.0, 0.0);
    r.vis = 0.0;
    r.wk = vec3(0.0, 1.0, 0.0);
    r.L = vec3(0.0, 0.0, 0.0);
    r.q = -1.0;
    r.M = 0.0;
    r.W = 0.0;
    r.wSum = 0.0;
    r.x1 = vec3(0.0, 0.0, 0.0);
    r.n1 = vec3(0.0, 1.0, 0.0);
    r.formed = false;
    return r;
}

Reservoir restirUnpack(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e) {
    Reservoir r;
    r.x = a.xyz;
    r.q = a.w;
    r.L = b.rgb;
    r.n = restirUnpackDir(b.w);
    r.Le = c.rgb;
    r.wk = restirUnpackDir(c.w);
    uint path = uint(d.x);
    r.seed = path >> 4u;
    r.k = int((path >> 1u) & 7u);
    r.vis = float(path & 1u);
    uint weights = uint(d.y);
    r.M = float(weights >> 16u);
    r.glossiness = float((weights >> 8u) & 255u) / 255.0;
    r.metallic = float(weights & 255u) / 255.0;
    r.W = d.z;
    r.albedo = restirUnpackColour(d.w);
    r.x1 = e.xyz;
    r.formed = e.w >= 0.0;
    r.n1 = restirUnpackDir(max(e.w, 0.0));
    r.wSum = 0.0;
    return r;
}

float restirPackPath(Reservoir r) {
    return float(((r.seed & 0xFFFFFu) << 4u) | (uint(r.k) << 1u) | (r.vis > 0.5 ? 1u : 0u));
}
float restirPackWeights(Reservoir r) {
    uint m = uint(clamp(r.M, 0.0, 255.0) + 0.5);
    return float((m << 16u) | (uint(r.glossiness * 255.0 + 0.5) << 8u) | uint(r.metallic * 255.0 + 0.5));
}

// A macro, not a function: bgfx's HLSL-family translation only lets main() write the outputs. Outputs
// 0-4; the spatial pass writes its light after them.
#define restirWrite(RESTIR_R) \
    gl_FragData[0] = vec4((RESTIR_R).x, (RESTIR_R).q); \
    gl_FragData[1] = vec4((RESTIR_R).L, restirPackDir((RESTIR_R).n)); \
    gl_FragData[2] = vec4((RESTIR_R).Le, restirPackDir((RESTIR_R).wk)); \
    gl_FragData[3] = vec4(restirPackPath(RESTIR_R), restirPackWeights(RESTIR_R), (RESTIR_R).W, restirPackColour((RESTIR_R).albedo)); \
    gl_FragData[4] = vec4((RESTIR_R).x1, (RESTIR_R).formed ? restirPackDir((RESTIR_R).n1) : -1.0)

// Is (x, n) the same surface as (x0, n0) seen at distance t0, as far as a GI pixel can tell? Off its
// plane by more than a sliver is another surface -- but the sliver grows with the GI pixel's width at
// that distance, and once that width passes a voxel the normals stop counting: one GI pixel then spans
// several voxels and faces, and the render texel it samples (a different one each frame, jittered)
// lands on any of them. Holding that to the exact face threw the history away every frame in the
// distance -- the shimmer.
bool restirSameSurface(vec3 x, vec3 n, vec3 x0, vec3 n0, float t0) {
    float giPixel = t0 * restirSettings.w * restirJitter.z;
    float voxel = 20.0 * restirSettings.y;
    float minCos = giPixel > voxel ? -0.5 : 0.9;
    return dot(n, n0) > minCos && abs(dot(x - x0, n0)) < 0.004 * t0 + 4.0 * restirSettings.y + 2.0 * giPixel;
}

// The GI resolution's pixel, and the render texel it takes its visible point from this frame.
ivec2 restirFullPixel(ivec2 giPixel, ivec2 fullSize) {
    int stride = int(restirJitter.z);
    return min(giPixel * stride + ivec2(restirJitter.xy), fullSize - ivec2(1, 1));
}

#endif
