// =============================================================================
// pjv_sun_sky.sc  --  Shared sun / real atmosphere model for all renderers.
//
// Single source of truth for the light rig so every renderer shows the same scene under the same
// light. `sunDir` is a runtime uniform (set in main.cpp); everything else -- the sun's own colour,
// the sky seen directly by the camera, and the sky colour the GI's escaped rays pick up as ambient
// light -- is DERIVED from one closed-form single-scattering atmosphere model, atmosphere(dir),
// so there is exactly one "real sky" that can be sampled at any direction and every consumer
// agrees with it (no independent hand-placed gradients to keep in sync).
//
// The model is a standard Rayleigh + Mie single-scatter approximation over a homogeneous
// exponential atmosphere (see e.g. GPU Gems 2 ch.16 / Bruneton & Neyret), evaluated in CLOSED
// FORM (no ray marching -- this is called per-pixel for the visible sky and per escaped GI ray,
// so it has to be cheap): an "airmass" term stands in for the raymarched optical-depth integral,
// and the view ray's in-scattered light is additionally attenuated by the SUN's own transmittance
// through its own airmass -- that second term is what actually produces red/orange sunsets (light
// loses its blue on the way from the sun to the scattering volume, not just on the way to the eye).
// The same transmittance, evaluated at the sun's own elevation, gives the sun disc its colour, so
// the sun and the sky it lights always physically agree.
//
// IMPORTANT: `sunDir` must be declared in each renderer's resources.json "uniforms" list and
// uploaded every frame (uploadCommonUniforms does this). A renderer whose .bin is recompiled
// against this header but is missing the uniform declaration would get a zero sun.
// =============================================================================

#ifndef PI
#define PI 3.14159265358979
#endif

uniform vec4 sunDir;   // xyz = normalized sun direction (from main.cpp); w unused.

#define SUN_DIR     (sunDir.xyz)
#define SUN_ANGULAR 0.01             // Half-angle of the sun's visible disc (rad); constant.

// ---- Real single-scattering atmosphere ---------------------------------------------------------
// Coefficients are relative, in units where multiplying by 0.001 * ATMO_THICKNESS gives the zenith
// optical depth. The Rayleigh triple lands on ~(0.046, 0.108, 0.265), which is essentially the real
// atmosphere's (0.049, 0.113, 0.278) at 680/550/440nm -- so the blue of the daytime sky is not a
// tuned value, it falls out of the physics.
#define ATMO_BETA_R    vec3(5.8, 13.5, 33.1)  // Rayleigh scattering (air molecules), R<G<B
#define ATMO_BETA_M    2.5                     // Mie scattering (aerosol/haze), ~grey. Sized for a
                                                // zenith optical depth near 0.02, the standard clear-
                                                // air aerosol load. The old 0.7 was a quarter of that,
                                                // which is why distant terrain had almost no haze.
// Ozone. Absorbs (never scatters) in the Chappuis band, taking out green and some red while barely
// touching blue -- the reason a real sky stays blue overhead as the sun goes down instead of
// sliding through olive into grey. Its absence is what made the low-sun sky here come out green.
#define ATMO_BETA_O    vec3(0.650, 1.881, 0.085)
#define ATMO_MIE_G     0.62                    // Mie anisotropy -- forward-scattering sun glow
                                                // (kept well under ~0.8: closer to 1 makes the
                                                // forward peak both taller and narrower, which is
                                                // exactly what blew the sunset glow out before)
#define ATMO_THICKNESS 8.0                     // optical thickness of the whole sky column
#define ATMO_SUN_INTENSITY 3.4                 // sunlight scale used for SHADING the scene
#define ATMO_DISC_INTENSITY 40.0               // radiance of the sun's visible disc. Far brighter
                                                // than the shading scale on purpose: the disc has to
                                                // outrun its own surrounding glow, or it reads as a
                                                // dark hole punched in the brightest part of the sky.
#define ATMO_SKY_INTENSITY 21.0                // sky in-scatter brightness scale

// How much atmosphere a ray at elevation `cosZen` (dot with local up) punches through before
// reaching space -- 1 straight up, growing toward the horizon. That growth is what reddens and dims
// a low sun and hazes/pales a low view angle.
//
// The flat-slab 1/cos is right overhead but diverges at the horizon, where it claims an infinite
// path (the old clamp capped it at 50). The real atmosphere is a shell wrapped on a curved planet,
// so the horizon path tops out near 38 airmasses. This is the standard cheap fit to that: identical
// to 1/cos above ~20 degrees, and rolling over to ~40 at the horizon instead of running away.
float atmoAirmass(float cosZen) {
    float c = max(cosZen, 0.0);
    return 1.0 / (c + 0.025 * exp(-11.0 * c));
}

// Extinction = everything that removes light from a ray: both scattering species plus ozone's pure
// absorption. This is the denominator of the closed-form in-scatter and the exponent of every
// transmittance, so ozone has to be in here and NOT in the scattering source term below.
vec3 atmoBetaExtinction() {
    return ATMO_BETA_R * 0.001 + vec3(ATMO_BETA_M * 0.001) + ATMO_BETA_O * 0.001;
}

// Rayleigh phase (roughly uniform, mild forward/back symmetry) and Mie phase (Henyey-Greenstein;
// sharply forward-peaked -- this is what puts a soft glow around the sun itself, not just the hard
// disc) as functions of mu = dot(viewDir, sunDir).
float atmoPhaseRayleigh(float mu) {
    return 3.0 / (16.0 * PI) * (1.0 + mu * mu);
}
float atmoPhaseMie(float mu) {
    const float g = ATMO_MIE_G, gg = g * g;
    return 3.0 / (8.0 * PI) * ((1.0 - gg) * (1.0 + mu * mu))
         / ((2.0 + gg) * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

// Atmospheric transmittance along a direction of elevation `cosZen` -- how much of any given
// wavelength survives punching through the sky in that direction. Used both for the view ray
// (fading in-scatter with distance/haze) and, evaluated at the SUN's own elevation, for how the
// atmosphere tints the sunlight itself before it ever scatters toward the eye.
vec3 atmoTransmittance(float cosZen) {
    return exp(-atmoBetaExtinction() * atmoAirmass(cosZen) * ATMO_THICKNESS);
}

// Sample the real sky in world direction `dir` for the current sun direction. Valid for any dir
// (elevation clamped internally so below-horizon directions saturate to the horizon value rather
// than blowing up) -- this is THE atmosphere: the camera's background, the GI's sky ambient, and
// (via a second call at dir = SUN_DIR) the sun's own tint, all read the same function.
vec3 atmosphere(vec3 dir) {
    float cosZen = max(dir.y, 0.0);
    vec3  betaR  = ATMO_BETA_R * 0.001;
    float betaM  = ATMO_BETA_M * 0.001;
    vec3  betaExt = atmoBetaExtinction();

    vec3  Tview = exp(-betaExt * atmoAirmass(cosZen) * ATMO_THICKNESS);

    float mu = dot(dir, SUN_DIR);
    float phaseR = atmoPhaseRayleigh(mu);
    float phaseM = atmoPhaseMie(mu);

    // How attenuated the sunlight is by the time it reaches the air this view ray is looking at.
    //
    // Taking that straight from the sun's own elevation -- one number for the whole sky -- is what
    // breaks a sunset. It says every scattering event happens at sea level, so at dusk the ONLY
    // light available anywhere in the sky has been through ~40 airmasses and is pure red; the model
    // then paints a red zenith, which is not what a sunset looks like. The real zenith stays deep
    // blue because the air overhead is lit near the top of the atmosphere, where the sun's path in
    // is short and has barely been reddened at all.
    //
    // So the elevation used for the sun's transmittance rises with the view direction: rays looking
    // up sample air that is high, lit by nearly unattenuated sunlight (blue survives), while rays
    // looking at the horizon sample air near the ground on the long reddened path (orange survives).
    // One term, and the sunset gets its blue-overhead-to-orange-at-the-horizon gradient. The small
    // constant on top is the same effect for the observer's own horizon: the lit volume stays in
    // sunlight for a while after the ground below it does not, which is what makes dusk last.
    float sunElev = clamp(SUN_DIR.y, -1.0, 1.0);
    float scatterElev = min(sunElev + 0.045 + 0.35 * cosZen * cosZen, 1.0);
    vec3  Tsun = atmoTransmittance(max(scatterElev, 0.0));

    // Closed-form single-scatter in-scattered radiance of a homogeneous slab: (1-T)/beta is the
    // standard substitute for integrating exp(-beta*s) ds along the view ray; Tsun applies the
    // sun's own colour loss on the way in (the real source of a red/orange horizon glow). Ozone is
    // in betaExt but not in the numerator -- it absorbs, it never scatters light toward the eye.
    vec3 inscatter = (betaR * phaseR + vec3(betaM * phaseM)) * (vec3(1.0) - Tview) / max(betaExt, vec3(1e-6));
    inscatter *= Tsun;

    // Ramp reaching well below the horizon so dusk fades out over roughly the real twilight arc
    // rather than switching off at sunset.
    float dayIntensity = ATMO_SKY_INTENSITY * smoothstep(-0.22, 0.12, sunElev);
    vec3  sky = inscatter * dayIntensity;

    // Night floor: a dim, slightly blue ambient (airglow/starlight/moon stand-in) so a night sky
    // reads as deep blue rather than clipping to pure black, which no real sky ever does.
    vec3 nightFloor = vec3(0.004, 0.006, 0.014) * smoothstep(0.12, -0.24, sunElev);
    return sky + nightFloor;
}

// Sunlight colour/intensity: the same atmospheric transmittance the sky itself uses, evaluated at
// the sun's own elevation, so a low sun is naturally dim and red for the same physical reason the
// sky it lights goes orange -- both read one model.
vec3 pjvSunColor() {
    float e = SUN_DIR.y;
    vec3  tint = atmoTransmittance(max(e, 0.0));
    float intensity = ATMO_SUN_INTENSITY * smoothstep(-0.02, 0.08, e);   // fades out at the horizon
    return tint * intensity;
}

// Radiance of the sun's visible DISC, as opposed to pjvSunColor()'s irradiance-for-shading. Same
// tint, much larger scale: the two are different physical quantities (they differ by the sun's
// solid angle) and conflating them is what left the disc dimmer than the glow around it.
vec3 pjvSunDiscRadiance() {
    float e = SUN_DIR.y;
    vec3  tint = atmoTransmittance(max(e, 0.0));
    return tint * ATMO_DISC_INTENSITY * smoothstep(-0.02, 0.06, e);
}

// Ground bounce colour for directions looking below the horizon (not part of the atmosphere
// proper -- there's no sky down there -- but it should still track day/night like everything
// else does).
vec3 pjvSkyGround() {
    float day = smoothstep(-0.10, 0.15, SUN_DIR.y);
    return mix(vec3(0.010, 0.010, 0.020), vec3(0.24, 0.22, 0.19) * 0.55, day);
}

#define SUN_COLOR   pjvSunColor()
#define SKY_GROUND  pjvSkyGround()
#define SKY_ZENITH  atmosphere(vec3(0.0, 1.0, 0.0))         // back-compat alias (straight up)
#define SKY_HORIZON pjvSkyHorizonAmbient()                   // back-compat alias (direction-less callers)

// For call sites with no view direction available (e.g. a normal-based ambient term), approximate
// "the horizon colour" as the atmosphere just above the horizon toward the sun's own bearing.
vec3 pjvSkyHorizonAmbient() {
    vec3 dir = normalize(vec3(SUN_DIR.x, 0.06, SUN_DIR.z) + vec3(1e-5, 0.0, 1e-5));
    return atmosphere(dir);
}

// Full sky radiance in a direction. atmosphere() already clamps its internal elevation term at
// the horizon, so any direction pointed below the horizon (dir.y < 0) simply reads back the same
// colour as looking exactly along the horizon in that heading -- i.e. the sky never fades to a
// separate "ground" tone here, it just holds the horizon colour looking downward, same as the
// real sky appears to do when you tip your view below eye level over open air.
vec3 skyGradient(vec3 dir) {
    return atmosphere(dir);
}

// skyGradient() plus the sun's own disc, for renderers that display the sky directly (background
// pixels, or a bounce ray that happens to escape toward the sun). The soft glow AROUND the disc
// falls out of atmosphere()'s Mie phase term automatically; the disc itself blends in smoothly
// (smoothstep, not a hard cutoff) and is taken as max(glow, SUN_COLOR) rather than a flat override
// -- a flat value strictly inside a smoothly-varying, potentially brighter glow would show up as a
// dark hole exactly at the sun's centre the moment the glow outshines it (that's what a hard
// override into a dimmer flat colour was doing here).
vec3 skyColor(vec3 dir) {
    vec3  sky  = skyGradient(dir);
    float disc = smoothstep(cos(SUN_ANGULAR * 1.2), cos(SUN_ANGULAR * 0.8), dot(dir, SUN_DIR));
    return mix(sky, max(sky, pjvSunDiscRadiance()), disc);
}

// ---- Height + distance fog -----------------------------------------------------------------
// Closed-form exponential height fog (same family of trick as the atmosphere above: an analytic
// stand-in for a raymarched integral), referenced to the CAMERA's own altitude rather than a
// fixed world height: density is exactly FOG_DENSITY -- "its current level" -- at and BELOW the
// camera, so fog never thins out (or, worse, blows up) in valleys and dips below the camera; it
// only tapers off, gradually, for the part of a ray that climbs ABOVE the camera's altitude
// (large FOG_HEIGHT_FALLOFF scale height => a slow taper spread over a lot of vertical distance,
// not a sharp layer). fogOpticalDepth is the exact integral of that piecewise density along a ray
// from the camera to a surface point. applyFog then blends the shaded surface toward
// skyColor(rayDir) -- the REAL sky colour in that exact direction -- so distant terrain melts
// into the horizon instead of fading to a flat, direction-independent haze tint.
#define FOG_DENSITY        0.000022  // optical density at and below the camera's altitude
#define FOG_HEIGHT_FALLOFF 0.00028   // 1/world-unit: how fast density tapers ABOVE the camera
                                      // (large scale height ~3570 units -> a very gradual taper)

float fogOpticalDepth(vec3 camPos, vec3 rayDir, float dist) {
    // The ray starts at the camera's own altitude, so for as long as it stays at or below that
    // altitude (rayDir.y <= 0) density is the flat FOG_DENSITY the whole way -- no falloff
    // downward, ever. Only once/where it climbs above does the exponential taper apply, so a ray
    // that starts level or descending never sees it at all.
    if (rayDir.y <= 0.0) return FOG_DENSITY * dist;

    float kv = FOG_HEIGHT_FALLOFF * rayDir.y;
    float t  = (1.0 - exp(-kv * dist)) / kv;   // integral_0^dist exp(-k*rayDir.y*t) dt
    return FOG_DENSITY * t;
}

vec3 applyFog(vec3 color, vec3 camPos, vec3 rayDir, float dist) {
    float fogFactor = clamp(1.0 - exp(-fogOpticalDepth(camPos, rayDir, dist)), 0.0, 1.0);
    // skyGradient, NOT skyColor: the haze in front of a surface carries the sky's in-scattered
    // light (including the Mie glow near the sun, which is a real effect and comes along with the
    // gradient), but it must not carry the sun's DISC. skyColor would paint a bright solar disc
    // onto whatever mountain happened to be occluding the sun.
    return mix(color, skyGradient(rayDir), fogFactor);
}
