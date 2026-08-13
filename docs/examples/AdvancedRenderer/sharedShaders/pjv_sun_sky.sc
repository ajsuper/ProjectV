// =============================================================================
// pjv_sun_sky.sc  --  The renderer's sun / sky description.
//
// Single source of truth for the light rig so every pass shows the scene under the same light. The
// sun DIRECTION is a runtime uniform (`sunDir`, driven by the scroll wheel in main.cpp) and the sun
// + sky COLOURS are derived from the sun's elevation (sunDir.y), so scrolling the sun down toward
// the horizon warms the light and reddens the sky (sunset) and pushing it below the horizon darkens
// everything toward night -- a full day cycle from one control.
//
// The curves are the scene editor's, term for term (see its gi.frag / path_trace.frag), including
// the two intensity multipliers that used to be hard-coded here. That is deliberate: this example
// and the editor should agree about what a scene looks like, so a scene lit here reads the same way
// in the editor's Render tab. Both files carry their own copy rather than sharing a header, because
// a light rig is not part of the engine and one example does not reach into another's files.
//
// IMPORTANT: `sunDir` and `renderParams` must both be declared in resources.json's "uniforms" list
// and uploaded every frame (uploadFrameUniforms does this). A shader recompiled against this header
// whose renderer is missing either declaration gets a black sun and a black sky.
//
// The SUN_DIR / SUN_COLOR / SKY_* names are preserved as macros so existing consumers are unchanged.
// =============================================================================

uniform vec4 sunDir;   // xyz = normalized sun direction; w = the sun disk's angular radius (rad).
// x = spare (the editor puts its bounce count here), y = sun intensity, z = sky intensity,
// w = spare (the editor's firefly clamp -- this renderer's gather has no unbounded path to clamp).
uniform vec4 renderParams;

#define SUN_DIR     (sunDir.xyz)
// Half-angle of the sun disk, in radians. Runtime rather than constant so the drawn disk tracks the
// same control the editor calls "sun size". Floored above zero: a zero-radius disk is invisible at
// any resolution, and `cos(0)` makes the disk test below exact-equality.
#define SUN_ANGULAR (max(sunDir.w, 0.0002))

// ---- Day-cycle colours, all functions of the sun elevation e = sunDir.y in [-1,1] --------------
// e = 1 sun straight overhead (noon), e = 0 sun on the horizon, e < 0 sun below the horizon (dusk
// -> night). At the default sun (~49 deg, e ~ 0.76) these reproduce the previous clear-day constants.

// Direct sunlight (normal-incidence irradiance). Neutral-white high in the sky, warm/orange near
// the horizon, fading to black once the sun sets.
vec3 pjvSunColor() {
    float e = sunDir.y;
    vec3  tint      = mix(vec3(1.0, 0.42, 0.15), vec3(1.0, 0.96, 0.90), smoothstep(0.0, 0.35, e));
    float intensity = renderParams.y * smoothstep(-0.05, 0.15, e);   // fades out below the horizon
    return tint * intensity;
}

// Sky gradient endpoints. `day` fades the whole sky from a dark night palette to the clear-day one;
// `warm` adds a sunset/sunrise glow to the horizon band while the sun is low but still up.
vec3 pjvSkyZenith() {
    float day = smoothstep(-0.18, 0.22, sunDir.y);
    return mix(vec3(0.02, 0.03, 0.06), vec3(0.30, 0.50, 0.95) * 2.2, day) * renderParams.z;
}
vec3 pjvSkyHorizon() {
    float e    = sunDir.y;
    float day  = smoothstep(-0.18, 0.22, e);
    float warm = day * (1.0 - smoothstep(0.0, 0.30, e));   // strongest as the sun nears the horizon
    vec3  base = mix(vec3(0.03, 0.04, 0.08), vec3(0.75, 0.85, 1.00) * 1.6, day);
    return mix(base, vec3(1.0, 0.50, 0.25) * 1.8, warm * 0.85) * renderParams.z;
}
vec3 pjvSkyGround() {
    float day = smoothstep(-0.18, 0.22, sunDir.y);
    return mix(vec3(0.01, 0.01, 0.02), vec3(0.25, 0.24, 0.22) * 0.6, day) * renderParams.z;
}

#define SUN_COLOR   pjvSunColor()
#define SKY_ZENITH  pjvSkyZenith()
#define SKY_HORIZON pjvSkyHorizon()
#define SKY_GROUND  pjvSkyGround()

// Radiance of the bare atmosphere in a given direction -- no sun disk, just the
// zenith/horizon/ground gradient.
vec3 skyGradient(vec3 dir) {
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = mix(SKY_HORIZON, SKY_ZENITH, up);
    sky = mix(SKY_GROUND, sky, smoothstep(-0.05, 0.05, dir.y));
    return sky;
}

// Atmosphere + the sun disk, for passes that display the sky directly (background pixels, or a
// bounce ray that happens to escape toward the sun). The disk is exactly the cone
// pjvSampleSunCone samples, so widening the sun softens the shadows and grows the drawn disk
// together instead of letting the two disagree.
vec3 skyColor(vec3 dir) {
    vec3 sky = skyGradient(dir);
    if (dot(dir, SUN_DIR) > cos(SUN_ANGULAR)) sky = SUN_COLOR;
    return sky;
}

// ---- Sampling the sun as a disk rather than as a point -----------------------------------------
// The scene editor's soft-shadow machinery, which is what makes its shadow edges as wide as the sun
// is and not one pixel wide. A pass that wants a hard shadow simply traces along SUN_DIR and never
// calls any of this.

// PCG-style hash step. One multiply-xorshift per call, which is what keeps a per-pixel stream cheap
// enough to draw from inside a shading loop.
float pjvRandomUnit(inout uint seed) {
    seed = seed * 747796405u + 2891336453u;
    uint word = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}

// Seed from (pixel x, pixel y, frame). The pixel terms decorrelate neighbours so the penumbra reads
// as noise a temporal mean can remove rather than as a pattern it cannot; the frame term is what the
// TAA pass averages away over the frames since the camera last moved.
uint pjvHashSeed(uvec3 v) {
    uint h = v.x * 1973u + v.y * 9277u + v.z * 26699u;
    h = (h ^ (h >> 15u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    return h ^ (h >> 16u);
}

mat3 pjvBasisAround(vec3 n) {
    vec3 tangent = abs(n.z) < 0.999 ? normalize(cross(vec3(0.0, 0.0, 1.0), n))
                                    : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    return mat3(tangent, cross(n, tangent), n);
}

// A direction inside the sun's cone, uniformly. Uniform over the cone (not over its projection) is
// the right measure here because the estimator that uses it divides by the same solid angle, which
// is why widening SUN_ANGULAR softens every shadow without changing the image's exposure.
vec3 pjvSampleSunCone(inout uint seed) {
    vec3  axis     = normalize(SUN_DIR);
    float cosMax   = cos(SUN_ANGULAR);
    float cosTheta = mix(cosMax, 1.0, pjvRandomUnit(seed));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi      = 6.28318530718 * pjvRandomUnit(seed);
    return normalize(pjvBasisAround(axis) * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}
