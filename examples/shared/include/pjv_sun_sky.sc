// =============================================================================
// pjv_sun_sky.sc  --  Shared sun / sky description for all renderers.
//
// Single source of truth for the light rig so every renderer shows the same scene under the same
// light. The sun DIRECTION is now a runtime uniform (`sunDir`, driven by the scroll wheel in
// main.cpp) and the sun + sky COLOURS are derived from the sun's elevation (sunDir.y), so scrolling
// the sun down toward the horizon warms the light and reddens the sky (sunset) and pushing it below
// the horizon darkens everything toward night -- a full day cycle from one control.
//
// IMPORTANT: `sunDir` must be declared in each renderer's resources.json "uniforms" list and
// uploaded every frame (uploadCommonUniforms does this). A renderer whose .bin is recompiled
// against this header but is missing the uniform declaration would get a zero sun.
//
// The SUN_DIR / SUN_COLOR / SKY_* names are preserved as macros so existing consumers are unchanged.
// =============================================================================

uniform vec4 sunDir;   // xyz = normalized sun direction (from main.cpp); w unused.

#define SUN_DIR     (sunDir.xyz)
#define SUN_ANGULAR 0.3            // Half-angle of the sun disk (rad); constant.

// ---- Day-cycle colours, all functions of the sun elevation e = sunDir.y in [-1,1] --------------
// e = 1 sun straight overhead (noon), e = 0 sun on the horizon, e < 0 sun below the horizon (dusk
// -> night). At the default sun (~49 deg, e ~ 0.76) these reproduce the previous clear-day constants.

// Direct sunlight (normal-incidence irradiance). Neutral-white high in the sky, warm/orange near
// the horizon, fading to black once the sun sets.
vec3 pjvSunColor() {
    float e = sunDir.y;
    vec3  tint      = mix(vec3(1.0, 0.42, 0.15), vec3(1.0, 0.96, 0.90), smoothstep(0.0, 0.35, e));
    float intensity = 8.0 * smoothstep(-0.05, 0.15, e);   // fades out below the horizon
    return tint * intensity;
}

// Sky gradient endpoints. `day` fades the whole sky from a dark night palette to the clear-day one;
// `warm` adds a sunset/sunrise glow to the horizon band while the sun is low but still up.
vec3 pjvSkyZenith() {
    float day = smoothstep(-0.18, 0.22, sunDir.y);
    return mix(vec3(0.02, 0.03, 0.06), vec3(0.30, 0.50, 0.95) * 2.2, day);
}
vec3 pjvSkyHorizon() {
    float e    = sunDir.y;
    float day  = smoothstep(-0.18, 0.22, e);
    float warm = day * (1.0 - smoothstep(0.0, 0.30, e));   // strongest as the sun nears the horizon
    vec3  base = mix(vec3(0.03, 0.04, 0.08), vec3(0.75, 0.85, 1.00) * 1.6, day);
    return mix(base, vec3(1.0, 0.50, 0.25) * 1.8, warm * 0.85);
}
vec3 pjvSkyGround() {
    float day = smoothstep(-0.18, 0.22, sunDir.y);
    return mix(vec3(0.01, 0.01, 0.02), vec3(0.25, 0.24, 0.22) * 0.6, day);
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

// Atmosphere + a hard-edged sun disk, for renderers that display the sky directly (background
// pixels, or a bounce ray that happens to escape toward the sun).
vec3 skyColor(vec3 dir) {
    vec3 sky = skyGradient(dir);
    if (dot(dir, SUN_DIR) > cos(SUN_ANGULAR)) sky = SUN_COLOR;
    return sky;
}
