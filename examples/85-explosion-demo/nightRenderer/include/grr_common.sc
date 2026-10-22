// =============================================================================
// grr_common.sc -- what every pass of the game-ready renderer agrees on: the camera's projection, the
// light rig (sun, sky, the ambient they make together) and the fog.
//
// One place, because each of these is read by more than one pass and the passes must not disagree: the
// sky the G-buffer draws behind a silhouette is the colour the fog fades toward, and the camera ray the
// reconstruction rebuilds is the ray the G-buffer traced.
// =============================================================================
#ifndef GRR_COMMON_SC
#define GRR_COMMON_SC

#define GRR_PI  3.14159265
#define GRR_FOV 60.0   // vertical, degrees; main.cpp's CAMERA_VERTICAL_FOV_DEGREES

uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 sunDir;        // xyz toward the sun, unit length
// x = sun intensity, y = sky intensity, z = exposure, w = the finest voxel's edge length (world units)
uniform vec4 lightParams;
// x = fog extinction at the floor (per world unit, 0 = off), y = the height it thins over,
// z = the floor's y, w = sun inscatter strength
uniform vec4 fogParams;

// ---- Camera ---------------------------------------------------------------------------------------

void grrCameraBasis(vec3 direction, out vec3 forward, out vec3 right, out vec3 up) {
    forward = normalize(direction);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    right = normalize(cross(forward, worldUp));
    up = normalize(cross(right, forward));
}

// The ray through screen position uv -- the same map as the engine's rayStartDirection.
vec3 grrCameraRay(vec2 uv, vec2 res, vec3 direction) {
    vec3 forward, right, up;
    grrCameraBasis(direction, forward, right, up);
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float scale = tan(radians(GRR_FOV * 0.5));
    return normalize(right * (ndc.x * scale * res.x / res.y) + up * (ndc.y * scale) + forward);
}

// Its inverse: where on screen a world point lands, for a camera at camPos looking along camDir.
vec2 grrWorldToUV(vec3 P, vec3 camPos, vec3 camDir, vec2 res, out bool valid) {
    vec3 forward, right, up;
    grrCameraBasis(camDir, forward, right, up);
    vec3 v = P - camPos;
    float z = dot(v, forward);
    if (z <= 1e-4) { valid = false; return vec2(-1.0, -1.0); }
    float scale = tan(radians(GRR_FOV * 0.5));
    vec2 ndc = vec2((dot(v, right) / z) / (scale * res.x / res.y), (dot(v, up) / z) / scale);
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    valid = all(greaterThanEqual(uv, vec2(0.0, 0.0))) && all(lessThanEqual(uv, vec2(1.0, 1.0)));
    return uv;
}

// ---- The light rig: day or a moonlit night ------------------------------------------------------------
// Voxel Mayhem's rig, with a night beside its day (`skyParams.x`: 1 day, 0 night). By day `sunDir` is the
// sun -- warm, bright, a blue sky, a sunset as it nears the horizon. By night it is the MOON: a cold, dim
// key light, a near-black sky with stars and the moon's disk in it. Everything else derives from the two,
// so the fog, the ambient and the GI's sky misses all follow.
//
// The explosion is a second light: `flashParams` (xyz where the fire's light comes from, w its
// intensity) and `flashColor`. The G-buffer traces a shadow ray toward it, so debris silhouettes against
// the fireball.
uniform vec4 flashParams;
uniform vec4 flashColor;   // rgb, w = the radius the light falls off over (m)
uniform vec4 skyParams;    // x = 1 day, 0 night

bool grrDay() { return skyParams.x > 0.5; }

// How much daylight there is: 1 with the sun well up, fading through sunset to 0 below the horizon.
// None at night.
float grrDaylight() { return grrDay() ? smoothstep(-0.06, 0.18, sunDir.y) : 0.0; }

// The key light's radiance. The sun: white-warm at noon, orange as it sets -- the warmth is on the KEY
// light, so lit surfaces warm while sky-lit shadows stay cool. The moon: a pale blue-white.
vec3 grrSunColor() {
    vec3 tint = grrDay() ? mix(vec3(1.0, 0.42, 0.16), vec3(1.0, 0.91, 0.78), smoothstep(0.0, 0.45, sunDir.y))
                         : vec3(0.62, 0.72, 1.0);
    return tint * (lightParams.x * smoothstep(-0.02, 0.10, sunDir.y));
}

float grrHash3(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

// The sky along a direction, without the sun's or the moon's disk.
vec3 grrSky(vec3 d) {
    vec3 sky;
    float h = 1.0 - clamp(d.y, 0.0, 1.0);
    float mu = max(dot(d, sunDir.xyz), 0.0);
    if (grrDay()) {
        float day = grrDaylight();
        vec3 zenith  = mix(vec3(0.010, 0.014, 0.030), vec3(0.16, 0.31, 0.66), day);
        vec3 horizon = mix(vec3(0.025, 0.030, 0.050), vec3(0.58, 0.68, 0.82), day);
        // Sunset: the horizon on the sun's side warms as the sun nears it.
        float low = (1.0 - smoothstep(0.0, 0.35, sunDir.y)) * day;
        float sunward = 0.5 + 0.5 * dot(normalize(vec3(d.x, 0.0, d.z) + 1e-5), normalize(vec3(sunDir.x, 0.0, sunDir.z) + 1e-5));
        horizon = mix(horizon, vec3(0.95, 0.52, 0.28), low * sunward * 0.75);
        sky = mix(horizon, zenith, 1.0 - h * h * h);
        // Below the horizon: the haze darkening toward a ground the sky never shows.
        if (d.y < 0.0) sky = mix(horizon, horizon * 0.4, clamp(-d.y * 4.0, 0.0, 1.0));
        // The aureole: the haze glowing around the sun, wide and faint, then tight and bright.
        sky += grrSunColor() * (0.012 * pow(mu, 6.0) + 0.06 * pow(mu, 120.0));
    } else {
        vec3 zenith  = vec3(0.0030, 0.0050, 0.0140);
        vec3 horizon = vec3(0.0110, 0.0145, 0.0260);
        sky = mix(horizon, zenith, 1.0 - h * h * h);
        if (d.y < 0.0) sky = mix(horizon, horizon * 0.35, clamp(-d.y * 4.0, 0.0, 1.0));
        // The moon's halo in the haze.
        sky += grrSunColor() * (0.010 * pow(mu, 8.0) + 0.05 * pow(mu, 200.0));
    }
    return sky * lightParams.y;
}

// What the camera sees: the sky with the sun's disk in it by day; the stars and the moon's disk by night.
vec3 grrSkyWithSun(vec3 d) {
    vec3 sky = grrSky(d);
    float mu = dot(d, sunDir.xyz);
    if (grrDay()) return sky + grrSunColor() * (8.0 * smoothstep(0.99955, 0.99975, mu));
    // Stars: one in a few hundred cells of a fine grid on the sphere, dimmer toward the horizon.
    vec3 cell = floor(d * 420.0);
    float pick = grrHash3(cell);
    if (pick > 0.9978 && d.y > 0.0) {
        vec3 centre = normalize((cell + 0.5) / 420.0);
        float core = smoothstep(0.999997, 1.0, dot(d, centre)) + 0.3 * smoothstep(0.99999, 1.0, dot(d, centre));
        float bright = (pick - 0.9978) / 0.0022;
        vec3 tint = mix(vec3(0.75, 0.82, 1.0), vec3(1.0, 0.86, 0.7), grrHash3(cell + 7.0));
        // Not scaled by the sky's brightness: they light nothing, and a darker sky only shows them better.
        sky += tint * core * (0.15 + 1.6 * bright * bright) * smoothstep(0.0, 0.25, d.y);
    }
    // The moon: a disk a little larger than life, with darker maria.
    float disk = smoothstep(0.99975, 0.99982, mu);
    if (disk > 0.0) {
        vec3 up = abs(sunDir.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        vec3 right = normalize(cross(up, sunDir.xyz));
        vec3 upv = cross(sunDir.xyz, right);
        vec2 q = vec2(dot(d, right), dot(d, upv)) / 0.019;
        float maria = 0.75 + 0.25 * sin(q.x * 9.0 + 1.3) * sin(q.y * 7.0 - 0.4) - 0.2 * smoothstep(0.3, 0.0, length(q - vec2(0.25, 0.2)));
        sky += vec3(0.95, 0.96, 1.0) * (6.0 * disk * maria);
    }
    return sky;
}

// ---- Ambient: the cheap GI ---------------------------------------------------------------------------
// Light arriving from everywhere except the sun's own direction, for a surface facing n. Two
// hemispheres, blended by facing:
//
//   above  the sky's average radiance (a cosine-weighted mean of grrSky, taken at three heights)
//   below  the sun's light bounced off the ground -- a warm fill that lifts the underside of an arch,
//          the shadowed face of a wall, a ceiling, from the black a sky-only ambient leaves them.
//
// What it does not know is WHERE it is: inside a room it would light as though the room were open. The
// two-scale AO in ssao.frag is what supplies that, and the two multiplied are the whole indirect term --
// a low-frequency GI made from one blur and no rays.
#define GRR_GROUND_ALBEDO  vec3(0.32, 0.29, 0.25)
// The sky's share of a shaded surface's light. Below 1 because the sky a real surface sees is partly
// blocked by everything around it beyond the AO's reach; the AO handles the near part.
#define GRR_SKY_AMBIENT    0.55
#define GRR_SKY_AMBIENT_SATURATION 0.7
// The sun's light bounced back off the ground and walls, relative to a flat ground of GRR_GROUND_ALBEDO.
// Above 1 because a courtyard or a street has sunlit walls as well as a floor.
#define GRR_BOUNCE         1.4
vec3 grrAmbient(vec3 n) {
    vec3 skyUp      = (grrSky(vec3(0.0, 1.0, 0.0)) * 0.30
                    +  grrSky(normalize(vec3(sunDir.x, 0.6, sunDir.z))) * 0.30
                    +  grrSky(normalize(vec3(-sunDir.x, 0.25, -sunDir.z))) * 0.40) * GRR_SKY_AMBIENT;
    // The sky's colour, pulled halfway to grey: a shaded surface is lit by the sky AND by everything
    // sunlit around it, and the second is warm. Full sky blue made every shadow read as cold.
    float skyLuma = dot(skyUp, vec3(0.2126, 0.7152, 0.0722));
    skyUp = mix(vec3(skyLuma, skyLuma, skyLuma), skyUp, GRR_SKY_AMBIENT_SATURATION);
    vec3 groundUp   = GRR_GROUND_ALBEDO * grrSunColor() * (GRR_BOUNCE * max(sunDir.y, 0.0) / GRR_PI)
                    + GRR_GROUND_ALBEDO * skyUp * 0.5;
    float up = n.y * 0.5 + 0.5;
    return mix(groundUp, skyUp, up * up * (3.0 - 2.0 * up));
}

// ---- The explosion's light -------------------------------------------------------------------------
// The fire's light reaching a point P facing N, before any shadow: an inverse-square falloff softened
// over flashColor.w metres so a surface inside the fireball is not infinitely bright.
vec3 grrFlashLight(vec3 P, vec3 N) {
    if (flashParams.w <= 0.0) return vec3(0.0, 0.0, 0.0);
    vec3 toLight = flashParams.xyz - P;
    float d2 = dot(toLight, toLight);
    float nl = max(dot(N, toLight * inversesqrt(max(d2, 1e-6))), 0.0);
    return flashColor.rgb * (flashParams.w * nl / (d2 + flashColor.w * flashColor.w));
}

// ---- Height fog ---------------------------------------------------------------------------------------
// Exponential in height, so it pools in the low parts of a scene and thins upward, with a closed-form
// optical depth along the ray: no marching, two exp()s. Sky pixels go through it too (at a long
// distance), so distant geometry and the horizon behind it meet at the same colour.

float grrFogOpticalDepth(vec3 origin, vec3 dir, float dist) {
    if (fogParams.x <= 0.0) return 0.0;
    float H = max(fogParams.y, 1e-3);
    float densityAtEye = fogParams.x * exp(-(origin.y - fogParams.z) / H);
    // Floored so a long ray DOWN into the fog cannot overflow exp(); it is fully fogged long before.
    float k = max(dir.y * dist / H, -40.0);
    // (1 - e^-k) / k is the mean of the density along the ray relative to the eye's; it tends to 1 for a
    // level ray, where the plain form would divide zero by zero.
    float meanFactor = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0 - 0.5 * k;
    return densityAtEye * dist * meanFactor;
}

vec3 grrFogColor(vec3 dir) {
    vec3 horizonDir = normalize(vec3(dir.x, max(dir.y, 0.0) * 0.3 + 0.02, dir.z));
    float mu = max(dot(dir, sunDir.xyz), 0.0);
    return grrSky(horizonDir) * 0.9 + grrSunColor() * (fogParams.w * pow(mu, 8.0) * 0.12);
}

vec3 grrApplyFog(vec3 colour, vec3 origin, vec3 dir, float dist) {
    float transmittance = exp(-grrFogOpticalDepth(origin, dir, dist));
    return mix(grrFogColor(dir), colour, transmittance);
}

#endif // GRR_COMMON_SC
