$input v_color0
$input v_texcoord0

// =============================================================================
// gbuffer.frag  --  Pass 1 of the radiance-cascade renderer.
//
// One primary ray per pixel. Produces the G-buffer that every later pass consumes and,
// crucially, the DIRECT-LIT colour buffer (gDirect) that the screen-space cascade rays
// sample when they hit a surface -- that's how bounced sunlight enters the GI.
//
// Outputs (FBO 1):
//   0 gPos     rgb = world hit position, a = camera-space depth   (a < 0  => sky pixel)
//   1 gNormal  rgb = world normal,       a = hit mask (1 hit / 0 sky)
//   2 gAlbedo  rgb = surface albedo
//   3 gDirect  rgb = surface outgoing radiance (albedo * direct sun, hard shadow);
//                    for sky pixels this holds the sky colour for the background.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 frameCount;

#define PI 3.14159265358979
#define FOV 60.0

#include <pjv_sun_sky.sc>

// Hard sun shadow ray: is the sun disk visible from p (surface facing n)?
// Keep finishLOD=0 (coarse-LOD shadow rays over-occlude -> dim bounced light).
bool sunVisible(vec3 p, vec3 n) {
    Ray shadow;
    shadow.origin = p + n * 0.02;
    shadow.direction = SUN_DIR;
    RayQuery rq;
    rq.maxRaySteps = 48u;
    rq.startLOD = 0u; rq.finishLOD = 0u; rq.distanceToFinishLOD = 30u;
    return raySceneIntersect(shadow, rq).rayT < 0.0;
}

// ---- Water normal perturbation ---------------------------------------------
// Uses ANALYTIC derivatives of sine-wave height fields to compute surface normals.
// Finite differences at the scale of this scene's huge wavelengths produce near-zero
// gradients; analytic derivatives give correct tilt regardless of wavelength.
vec3 perturbWaterNormal(vec3 P, vec3 N, float time) {
    vec2 uv = P.xz;
    float speed = time * 0.4;

    // Wave 1: long gentle swell, analytic gradient
    vec2 d1 = normalize(vec2(1.0, 0.3));
    float f1 = 0.008;
    float phase1 = dot(uv, d1) * f1 + speed * 0.7;
    float h1 = 1.5 * sin(phase1);
    vec2 grad1 = 1.5 * f1 * d1 * cos(phase1);

    // Wave 2: shorter chop, analytic gradient
    vec2 d2 = normalize(vec2(-0.6, 0.8));
    float f2 = 0.02;
    float phase2 = dot(uv, d2) * f2 + speed * 1.3;
    float h2 = 1.2 * sin(phase2);
    vec2 grad2 = 1.2 * f2 * d2 * cos(phase2);

    // Combined height and gradient
    float h = h1 + h2;
    vec2 grad = grad1 + grad2;

    // Convert gradient to normal: normal = normalize(-grad.x, 1, -grad.y) in local (0,1,0) space
    vec3 bumpN = normalize(vec3(-grad.x, 1.0, -grad.y) * 1000);

    // Blend with original geometric normal (which is (0,1,0) for flat water)
    return normalize(mix(N, bumpN * 1000, 0.8));
}

vec3 perturbTreeNormal(vec3 P, vec3 N, float time) {
    vec2 windDir = vec2(0.8, 0.5);
    float windStrength = 0.2 + 0.1 * sin(time * 0.4 + P.x * 0.2 + P.z * 0.3);
    float phase = P.x * 0.2 + P.z * 0.3 + time * 1.5;
    float wave = sin(phase) * windStrength;
    vec3 windVec = vec3(windDir.x, 0.0, windDir.y);
    return normalize(N + windVec * wave * 0.3);
}

bool isWaterColor(vec3 c) {
    return c.b > c.g * 1.3 && c.b > c.r * 2.0 && c.g > c.r * 1.3;
}

bool isTreeColor(vec3 c, float wy) {
    if (wy < 398.0) return false;
    return c.g > c.r * 1.1 && c.g > c.b;
}

// Van der Corput / Halton for the sub-pixel jitter (base 2 and 3) that feeds the TAA pass.
float halton(int i, int base) {
    float f = 1.0;
    float r = 0.0;
    for (int k = 0; k < 16; k++) {
        if (i <= 0) break;
        f /= float(base);
        r += f * float(i - (i / base) * base);
        i /= base;
    }
    return r;
}

void main() {
    // Sub-pixel jitter of the primary ray: the AA sample offset that taa.frag resolves into
    // anti-aliased edges. +-0.5px Halton so each frame samples a different point within the pixel.
    int  frame  = int(frameCount.x);
    vec2 jitter = vec2(halton(frame + 1, 2), halton(frame + 1, 3)) - 0.5;
    vec2 uvJit  = v_texcoord0 + jitter / windowRes.xy;

    Ray ray;
    ray.origin = cameraPos.xyz;
    ray.direction = rayStartDirection(uvJit, windowRes.xy, cameraPos.xyz,
                                      normalize(cameraDir.xyz), FOV);

    // Primary ray: keep LOD at 0 (coarse LOD on the visible surface looks bad).
    // Reduce step count only — most rays hit within 256 steps.
    RayQuery rq;
    rq.maxRaySteps         = 256u;
    rq.startLOD            = 0u;
    rq.finishLOD           = 0u;
    rq.distanceToFinishLOD = 100000u;

    SceneIntersectData hit = raySceneIntersect(ray, rq);
    vec3 n = hit.normal;

    // Miss / degenerate boundary hit -> sky background.
    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(n, n) < 0.5) {
        vec3 sky = skyColor(ray.direction);
        gl_FragData[0] = vec4(ray.origin + ray.direction * 1e5, -1.0); // a<0 => sky
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);                     // hit mask 0
        gl_FragData[2] = vec4(0.0);
        gl_FragData[3] = vec4(sky, 0.0);
        return;
    }
    n = normalize(n);

    vec3  P = ray.origin + ray.direction * hit.rayT;
    vec3  albedo = fetchVoxelColor(hit.foundBox, hit.headerIndex);

    // --- Water & vegetation normal perturbation ---
    float time = float(frameCount.x) / 60.0;

    // Water: any top-facing surface near the water level
    if (P.y > 390.0 && P.y < 410.0 && n.y > 0.9) {
        n = perturbWaterNormal(P, n, time);
    } else if (isTreeColor(albedo, P.y)) {
        n = perturbTreeNormal(P, n, time);
    }
    n = normalize(n);

    // Direct sun (hard shadow) -- this is the light the GI bounces.
    vec3 direct = vec3(0.0);
    float NdotL = dot(n, SUN_DIR);
    if (NdotL > 0.0 && sunVisible(P, n)) {
        direct = (albedo / PI) * NdotL * SUN_COLOR;
    }

    float camDist = dot(P - cameraPos.xyz, normalize(cameraDir.xyz));

    gl_FragData[0] = vec4(P, camDist);
    gl_FragData[1] = vec4(n, 1.0);
    gl_FragData[2] = vec4(albedo, 1.0);
    gl_FragData[3] = vec4(direct, 1.0);
}
