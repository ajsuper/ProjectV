$input v_color0
$input v_texcoord0

// =============================================================================
// face_light.frag  --  Pass 2 of the face renderer (the actual lighting).
//
// Shades each voxel face as ONE flat colour, with full GI + direct sun. The trick
// that makes this both cheap to denoise and flat-per-face: all rays originate at the
// FACE CENTRE (from gFace) and all sample directions are seeded ONLY by the face's
// identity (gKey) plus the frame index. So every pixel covering a given face this
// frame produces the byte-identical colour -- the face is uniform with ZERO spatial
// noise, so there is nothing for a spatial blur to do (we don't have one). The only
// residual noise is temporal (a few GI rays per frame), which the next pass averages
// out per-face over time.
//
// Lighting model (full GI, view-independent):
//   direct   = hard sun shadow ray from the face centre           (crisp direct sun)
//   indirect = albedo * mean over a cosine hemisphere of:
//                escaped ray  -> that direction's sky radiance
//                blocked ray  -> the hit surface's own (direct sun + ambient-sky)
//                                radiance = colour bleed + INDIRECT SUN into creases
// That's one explicit bounce sampled with rays (colour bleeding + bounced sunlight)
// plus an analytic sky term at the bounce, i.e. effectively two-bounce GI. Because a
// face's irradiance is view-independent and the scene static, accumulating this over
// frames converges to the true value and stays put no matter how the camera moves.
//
// Inputs (flattened: FBO 1's four textures, then resourceTexture 1):
//   0 gAlbedo  1 gPos  2 gFace  3 gKey     (gPos unused here)
//   4 blueNoise (resourceTexture, texID 1)
// Output (FBO 2):
//   0 litRaw   rgb = this frame's flat face colour
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gAlbedo, 0);
SAMPLER2D(gFace,   2);
SAMPLER2D(gKey,    3);
SAMPLER2D(blueNoise, 4);
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 frameCount;

#define PI 3.14159265358979

#define GI_SAMPLES  1      // Hemisphere bounce rays per face per frame (amortised over frames).
#define GI_STEPS    40u    // DDA step budget per bounce ray (+dither); LOD ramp lets it escape.
#define AMBIENT_GI  0.0

#include <pjv_sun_sky.sc>

// Analytic hemispherical sky irradiance for a surface facing n -- the ambient a
// bounce surface receives, used as the (cheap, ray-free) terminal ambient term.
vec3 skyAmbient(vec3 n) {
    return mix(SKY_GROUND, SKY_HORIZON, clamp(n.y * 0.5 + 0.5, 0.0, 1.0)) * AMBIENT_GI;
}

// Orthonormal basis around a unit normal (Duff et al.).
void basis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float d = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * d, -s * n.x);
    b = vec3(d, s + n.y * n.y * a, -n.y);
}

float radicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), radicalInverse_VdC(i));
}

// Hash the face key -> a scalar in [0,1). Used to give each face its own base sample
// rotation so neighbouring faces don't share the same few directions (which would
// correlate their noise); combined with a per-frame advance it decorrelates over time.
float hashKey(vec4 k, float salt) {
    vec3 p = fract((k.xyz + k.w * 17.0 + salt) * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Reconstruct the outward face normal from the packed face id (0..5).
vec3 normalFromFaceId(float faceId) {
    int fa = int(faceId + 0.5);
    int axis = fa / 2;
    float s = (fa - axis * 2 == 1) ? 1.0 : -1.0;
    if (axis == 0) return vec3(s, 0.0, 0.0);
    if (axis == 1) return vec3(0.0, s, 0.0);
    return vec3(0.0, 0.0, s);
}

// Hard sun shadow ray: is the sun disk visible from p (surface facing n)?
bool sunVisible(vec3 p, vec3 n) {
    Ray shadow;
    shadow.origin = p + n * (0.02 * WORLD_SCALE);
    shadow.direction = SUN_DIR;
    RayQuery rq;
    rq.maxRaySteps = 128u;
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 30u;
    // Visibility only -- skip fetchVoxelData. rayT < 0 == genuine miss == unoccluded.
    return raySceneIntersect(shadow, rq).rayT < 0.0;
}

// Direct sun reflected off a diffuse surface (albedo/PI * NdotL * sun, hard shadow).
vec3 directSun(vec3 p, vec3 n, vec3 albedo) {
    float NdotL = dot(n, SUN_DIR);
    if (NdotL <= 0.0) return vec3(0.0);
    if (!sunVisible(p, n)) return vec3(0.0);
    return (albedo / PI) * NdotL * SUN_COLOR;
}

void main() {
    vec2 uv = v_texcoord0;

    vec4 albedoH = texture2D(gAlbedo, uv);
    // Sky / background: gbuffer already wrote the sky colour here (hit mask 0). The
    // sky is genuinely view-dependent, so it is NOT accumulated per-face -- just pass
    // it straight through for the accumulate pass to show as-is.
    if (albedoH.a < 0.5) {
        gl_FragData[0] = vec4(albedoH.rgb, 0.0);
        return;
    }

    vec3  albedo  = albedoH.rgb;
    vec4  face    = texture2D(gFace, uv);
    vec3  faceCtr = face.xyz;
    vec4  key     = texture2D(gKey,  uv);
    vec3  N       = normalFromFaceId(key.w);

    // Direct sun on the face itself.
    vec3 color = directSun(faceCtr, N, albedo);

    // Indirect: cosine-weighted hemisphere gather from the face centre. Sample rotation
    // depends ONLY on (face key, frame) so it is identical for every pixel of the face.
    vec3 t, b;
    basis(N, t, b);
    float frame = frameCount.x;
    vec2  rot   = fract(vec2(hashKey(key, 0.0), hashKey(key, 7.0))
                        + frame * vec2(0.7548776662, 0.5698402910));

    vec3 Li = vec3(0.0);
    for (int i = 0; i < GI_SAMPLES; i++) {
        vec2 u = fract(hammersley(uint(i), uint(GI_SAMPLES)) + rot);
        float cosT = sqrt(1.0 - u.x);
        float sinT = sqrt(u.x);
        float phi  = 2.0 * PI * u.y;
        vec3  dir  = normalize((cos(phi) * sinT) * t + (sin(phi) * sinT) * b + cosT * N);

        Ray ray;
        ray.origin    = faceCtr + N * (0.03 * WORLD_SCALE);
        ray.direction = dir;
        RayQuery rq;
        rq.maxRaySteps         = GI_STEPS + uint(hashKey(key, float(i) + frame) * 24.0);
        rq.startLOD            = 0u;
        rq.finishLOD           = 1u;   // coarse boxes far away so long rays stay cheap
        rq.distanceToFinishLOD = 24u;

        SceneIntersectData h = raySceneIntersect(ray, rq);
        if (h.rayT < 0.0 || h.foundBox.size < 0.0) {
            // Escaped -> this direction sees open sky (gradient only; the sun disk is
            // excluded so a stray ray onto it can't spike this averaged estimator).
            Li += skyGradient(dir);
        } else {
            // Blocked -> one bounce: the blocker's own outgoing radiance = its direct
            // sun (indirect sunlight bounced into the crease) + its ambient sky term.
            vec3 hP = ray.origin + ray.direction * h.rayT;
            vec3 hN = normalize(h.normal);
            Voxel hv = fetchVoxelData(h.foundBox, h.headerIndex);
            Li += directSun(hP, hN, hv.color) + hv.color * skyAmbient(hN);
        }
    }
    Li /= float(GI_SAMPLES);

    // Diffuse response to the gathered incoming radiance (cosine sampling folds in the
    // 1/PI and cosine, so it is simply albedo * mean(Li)).
    color += albedo * Li;

    gl_FragData[0] = vec4(color, 1.0);
}
