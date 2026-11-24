// =============================================================================
// santa_fe_clouds.sc  --  volumetric cumulus over the Sangres (plan R4).
//
// One layer between mtnClouds.y (base) and mtnClouds.z (top), metres above sea level. Density from a
// tileable 3D noise baked on the CPU (main.cpp bakeCloudNoise) into a 2D atlas: 128 slices of 128^2,
// 16 x 8 tiles of 130^2 (a texel of wrapped gutter round each, so bilinear filtering never bleeds
// between slices). Channels: r Perlin-Worley (the billows), g b a Worley at 2x, 4x, 8x (erosion).
//
//   coverage   a weather map (the atlas's r at a 60 km period, one slice), plus mtnClouds.x, plus a boost
//              over high ground (orographic: the far field's height)
//   shape      the low-frequency billows under a cumulus height profile (flat base, rounded towers)
//   erosion    the Worley octaves cut wisps out of the edges, more toward the top
//
// Lighting: a short march toward the sun for its transmittance, Beer-powder, a two-lobe phase function
// and three multiple-scattering octaves (Wrenninge 2015), ambient from the sky above and the ground below.
//
// Used for: camera rays (sfCloudMarch, in front of what they hit), cloud shadows on the ground
// (sfCloudShadow), and the cloud cover the diffuse rays see (sfCloudSky).
// =============================================================================

#ifndef SANTA_FE_CLOUDS_SC
#define SANTA_FE_CLOUDS_SC

uniform vec4 mtnClouds;   // x: coverage bias (-1..1), y: base m, z: top m, w: extinction per m at full density
uniform vec4 sfCloudMap;  // the cloud-shadow map: x, y its corner (world x, z), z its size (m), w held (1)
uniform vec4 mtnWind;     // xy: the weather's offset (m), z: shape scale (m per noise period), w: brightness

#define SF_NOISE_RES 128.0
#define SF_NOISE_TILE 130.0
#define SF_NOISE_W 2080.0
#define SF_NOISE_H 1040.0

vec4 sfNoiseSlice(float s, vec2 xy) {
    float tx = mod(s, 16.0), ty = floor(s / 16.0);
    vec2 uv = (vec2(tx, ty) * SF_NOISE_TILE + 1.0 + xy) / vec2(SF_NOISE_W, SF_NOISE_H);
    return texture2DLod(cloudNoise, uv, 0.0);
}

// The baked noise at p (one period = 1 in each axis), trilinear.
vec4 sfNoise(vec3 p) {
    vec3 q = fract(p) * SF_NOISE_RES;
    float z = q.z - 0.5;
    float z0 = floor(z);
    float f = z - z0;
    float s0 = mod(z0 + SF_NOISE_RES, SF_NOISE_RES), s1 = mod(z0 + 1.0, SF_NOISE_RES);
    return mix(sfNoiseSlice(s0, q.xy), sfNoiseSlice(s1, q.xy), f);
}

float sfRemap(float v, float a, float b, float c, float d) { return c + (clamp(v, a, b) - a) / max(b - a, 1e-5) * (d - c); }

// Coverage 0..1 at xz: the weather map, the bias, and more cloud over the mountains.
float sfCoverage(vec2 xz) {
    vec2 w = (xz + mtnWind.xy) / 60000.0;
    float weather = sfNoiseSlice(17.0, fract(w) * SF_NOISE_RES).r;
    float weather2 = sfNoiseSlice(71.0, fract(w * 3.1 + 0.37) * SF_NOISE_RES).r;
    float c = weather * 0.7 + weather2 * 0.3 + mtnClouds.x;
    // Orographic: afternoon cumulus builds over the high ground.
    // From a coarse far ring (one lookup, not a search from the finest): the boost is kilometres wide.
    float h;
    if (mtnFarHeightIn(MTN_FAR_RINGS - 2, xz, h) || mtnFarHeightIn(MTN_FAR_RINGS - 1, xz, h))
        c += 0.35 * smoothstep(2500.0, 3600.0, h);
    return clamp(c, 0.0, 1.0);
}

// Density 0..1 (times mtnClouds.w for extinction). `detail`: erode with the Worley octaves.
float sfCloudDensitySmooth(vec3 p, float coverage, bool detail) {
    float base = mtnClouds.y, top = mtnClouds.z;
    // The Earth curves away under distant clouds (6371 km): a layer 60 km off is 280 m lower, 120 km off
    // 1.1 km, so the deck sinks to the horizon instead of ending in a wall above it.
    vec2 rel = p.xz - pjvViewPosition().xz;
    float y = p.y + dot(rel, rel) / (2.0 * 6.371e6);
    float h = (y - base) / (top - base);
    if (h <= 0.0 || h >= 1.0 || coverage <= 0.01) return 0.0;
    // Cumulus (santa-fe-reference S11): a flat, sharp base; how tall a cloud grows from a second weather map --
    // fair-weather puffs a third of the layer deep, towering congestus to its top -- and more where it is
    // cloudier; towers that narrow and round off.
    vec2 wq = (p.xz + mtnWind.xy) / 34000.0;
    float towering = sfNoiseSlice(41.0, fract(wq + 0.53) * SF_NOISE_RES).r;
    float tall = clamp(mix(0.3, 1.05, smoothstep(0.25, 0.6, towering)) * mix(0.8, 1.15, coverage), 0.25, 1.0);
    float profile = smoothstep(0.0, 0.05, h) * (1.0 - smoothstep(tall * 0.5, tall, h));
    // By height within the layer, not altitude: lowering the layer lowers the same clouds (their shadows come
    // with them), rather than reshuffling them -- with altitude, a lower base read another slice of the noise.
    vec3 q = vec3(p.x + mtnWind.x, (y - base + 4300.0) * 1.25, p.z + mtnWind.y) / mtnWind.z;
    // Domain-warped by the billows' own low octave, so heads bulge and lean rather than sit on a lattice.
    vec4 warp = sfNoise(q * 0.37 + vec3(0.71, 0.13, 0.29));
    q += (warp.gba - 0.5) * 0.18;
    vec4 n = sfNoise(q);
    vec4 n2 = sfNoise(q * 2.3 + vec3(0.41, 0.77, 0.19));
    float fbmW = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float billow = n.r * 0.72 + n2.r * 0.28;   // two scales of heads: cauliflower
    float shape = sfRemap(billow, -(1.0 - fbmW), 1.0, 0.0, 1.0);
    float d = sfRemap(shape * profile, 1.0 - coverage, 1.0, 0.0, 1.0);
    if (d <= 0.0) return 0.0;
    d *= mix(0.7, 1.25, smoothstep(0.1, 0.6, h));   // denser cores up the towers: bright, hard-edged heads
    if (detail) {
        vec4 m = sfNoise(q * 6.3 + vec3(0.31, 0.17, 0.53));
        float wisps = m.g * 0.625 + m.b * 0.25 + m.a * 0.125;
        wisps = mix(wisps, 1.0 - wisps, clamp(h * 4.0, 0.0, 1.0));   // billowy above, wispy at the base
        d = sfRemap(d, wisps * 0.45, 1.0, 0.0, 1.0);
    }
    // A steeper edge: cumulus are hard-edged, the fuzz of a thin margin reads as cotton.
    return clamp(d * 1.6, 0.0, 1.0);
}

// The blocky style (santa-fe-reference S12; with the blocky far field): clouds of boxes -- big ones for a
// cloud's body (the billows alone, read at each box's centre) and smaller ones for its detail (the eroded edge,
// at theirs), each box solid through. Lit as the smooth clouds are, so the boxes keep their shading.
#define SF_CLOUD_BODY_BOX 520.0
#define SF_CLOUD_DETAIL_BOX 170.0
float sfCloudDensity(vec3 p, float coverage, bool detail) {
    if (!sfOn2(SF2_CLOUD_BLOCKY)) return sfCloudDensitySmooth(p, coverage, detail);
    vec3 c1 = (floor(p / SF_CLOUD_BODY_BOX) + 0.5) * SF_CLOUD_BODY_BOX;
    if (sfCloudDensitySmooth(c1, sfCoverage(c1.xz), false) > 0.42) return 0.9;
    vec3 c2 = (floor(p / SF_CLOUD_DETAIL_BOX) + 0.5) * SF_CLOUD_DETAIL_BOX;
    if (sfCloudDensitySmooth(c2, coverage, detail) > 0.3) return 0.75;
    return 0.0;
}

// The slab [base, top] along a ray: entry and exit distances (exit < entry: none).
// `below`: how far under the base to start (the curved layer is lower far off; a shadow ray needs none).
vec2 sfCloudSlab(vec3 o, vec3 d, float tMax, float below) {
    float base = mtnClouds.y - below, top = mtnClouds.z;
    if (abs(d.y) < 1e-5) {
        if (o.y < base || o.y > top) return vec2(1.0, 0.0);
        return vec2(0.0, tMax);
    }
    float t0 = (base - o.y) / d.y, t1 = (top - o.y) / d.y;
    float a = max(min(t0, t1), 0.0), b = min(max(t0, t1), tMax);
    return vec2(a, b);
}

float sfHG(float mu, float g) {
    float g2 = g * g;
    return 0.0795775 * (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5);
}

// How much of the sun gets through the clouds to p (cloud shadows). `jitter` 0..1 offsets the samples.
float sfCloudShadow(vec3 p, vec3 sun, float jitter) {
    if (!sfOn(SF_CLOUDS) || !sfOn(SF_CLOUD_SHADOW) || sun.y <= 0.01) return 1.0;
#ifdef SF_HAS_CLOUD_SHADOW_MAP
    // The map (cloud_shadow.frag), where it reaches: p carried along the sun to the layer's base. A point
    // inside or over the layer takes it too (near enough). Beyond the map, the march below.
    if (sfCloudMap.w > 0.5) {
        vec2 at = p.xz + sun.xz * ((mtnClouds.y - p.y) / sun.y);
        vec2 uv = (at - sfCloudMap.xy) / sfCloudMap.z;
        if (all(greaterThan(uv, vec2(0.002, 0.002))) && all(lessThan(uv, vec2(0.998, 0.998))))
            return texture2DLod(cloudShadowMap, uv, 0.0).r;
    }
#endif
    vec2 slab = sfCloudSlab(p, sun, 1.0e5, 0.0);
    if (slab.y <= slab.x) return 1.0;
    float tau = 0.0;
    float len = min(slab.y - slab.x, 12000.0);
    float dt = len / 6.0;
    for (int i = 0; i < 6; i++) {
        vec3 q = p + sun * (slab.x + (float(i) + jitter) * dt);
        float c = sfCoverage(q.xz);
        tau += sfCloudDensity(q, c, false) * mtnClouds.w * dt;
    }
    return exp(-tau);
}

// The light the clouds throw on a diffuse ray: where the ray meets the layer's middle, how cloudy it
// is, as a grey that is bright where lit (cheap: no march; the diffuse rays average it).
vec4 sfCloudSky(vec3 o, vec3 d, vec3 sun, vec3 sunColour, vec3 skyUp) {
    if (!sfOn(SF_CLOUDS) || d.y <= 0.02) return vec4(0.0, 0.0, 0.0, 0.0);
    float mid = 0.5 * (mtnClouds.y + mtnClouds.z);
    float t = (mid - o.y) / d.y;
    if (t <= 0.0 || t > 80000.0) return vec4(0.0, 0.0, 0.0, 0.0);
    vec3 q = o + d * t;
    float c = sfCoverage(q.xz);
    float a = smoothstep(0.25, 0.75, c) * 0.85;
    vec3 colour = sunColour * (0.12 + 0.1 * max(sun.y, 0.0)) + skyUp * 0.6;
    return vec4(colour, a);
}

// A camera ray through the clouds up to tMax: rgb the light they scatter toward the camera, a their
// transmittance; `tCloud` gets the transmittance-weighted distance (for the air in front of them).
SF_GLOBAL float sfCloudFront = 1e9;   // the last march's first cloud (the composite's depth test)
SF_GLOBAL float sfCloudFrontBox = 0.0; // blocky: the size of the box it met first (the composite rebuilds its edges)

// The light a cloud scatters at p, per unit of its extinction: the sun through the cloud toward it (a short
// march, multiple scattering in octaves, Beer-powder) and the sky and ground round it. `c` the coverage there.
vec3 sfCloudLight(vec3 p, float c, float sigma, float mu, vec3 sun, vec3 sunColour, vec3 skyUp, vec3 groundUp) {
    float tauSun = 0.0;
    float ls = 60.0;
    float lt = ls * 0.5;
    int lightSteps = int(sfQuality.y + 0.5);
    for (int j = 0; j < 6; j++) {
        if (j >= lightSteps) break;
        vec3 q = p + sun * lt;
        tauSun += sfCloudDensity(q, c, j < 2) * mtnClouds.w * ls;   // the step's coverage: it changes over km
        lt += ls * 1.5;
        ls *= 1.9;
    }
    float h = clamp((p.y - mtnClouds.y) / (mtnClouds.z - mtnClouds.y), 0.0, 1.0);
    vec3 sunL = vec3(0.0, 0.0, 0.0);
    float a = 1.0, b = 1.0, cc = 1.0;
    // Multiple scattering, in octaves. 0.5 and 0.5 (were 0.6 and 0.3: the fourth octave reached as deep as
    // 3% of the optical depth, so a cloud's heart and shaded side were as bright as its sunlit head -- flat white).
    for (int k = 0; k < 4; k++) {
        float ph = mix(sfHG(mu, 0.8 * cc), sfHG(mu, -0.25 * cc), 0.3);
        sunL += sunColour * (a * ph * exp(-tauSun * b));
        a *= 0.5; b *= 0.5; cc *= 0.5;
    }
    float powder = 1.0 - exp(-sigma * 2.0 * 120.0);   // Beer-powder: dark edges facing the sun
    sunL *= mix(1.0, powder * 2.0, 0.5);
    // Less than the full sky: deep inside a cloud most of it is hidden; grey bases, bright sunlit heads.
    vec3 amb = mix(groundUp * 1.2 + skyUp * 0.2, skyUp * 0.85, 0.25 + 0.75 * h) * (0.35 + 0.45 * h);
    return (sunL + amb) * mtnWind.w;
}

// A box's face, lit cartoon-fashion: tops bright, sides by how they face the sun, bottoms dark.
float sfBoxFace(int axis, vec3 stepDir, vec3 sun) {
    vec3 n = axis == 0 ? vec3(-stepDir.x, 0.0, 0.0) : (axis == 1 ? vec3(0.0, -stepDir.y, 0.0) : vec3(0.0, 0.0, -stepDir.z));
    float f = 0.78 + 0.3 * max(dot(n, sun), 0.0);
    if (n.y > 0.5) f += 0.12;
    if (n.y < -0.5) f -= 0.3;
    return f;
}

// The blocky style's clouds, marched exactly: box by box (a 3D DDA), each box's constant density integrated
// over exactly the length of ray inside it -- crisp boxes, flat-lit faces. Body boxes first: a solid one is one
// segment, an empty one skipped whole; a partly cloudy one walked through its 3 x 3 x 3 detail boxes.
vec4 sfCloudMarchBlocky(vec3 o, vec3 d, vec2 slab, vec3 sun, vec3 sunColour, vec3 skyUp, vec3 groundUp, out float tCloud) {
    float mu = dot(d, sun);
    float T = 1.0;
    vec3 L = vec3(0.0, 0.0, 0.0);
    float sumW = 0.0, sumT = 0.0;
    float B = SF_CLOUD_BODY_BOX;
    vec3 stepDir = vec3(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    vec3 invD = vec3(abs(d.x) > 1e-7 ? 1.0 / abs(d.x) : 1e30, abs(d.y) > 1e-7 ? 1.0 / abs(d.y) : 1e30, abs(d.z) > 1e-7 ? 1.0 / abs(d.z) : 1e30);
    float t = slab.x;
    vec3 p0 = o + d * (t + 1e-3);
    vec3 cell = floor(p0 / B);
    vec3 tNext = vec3(t, t, t) + abs((cell + max(stepDir, vec3(0.0, 0.0, 0.0))) * B - p0) * invD;
    vec3 tDelta = B * invD;
    // The face a ray entered its box by: the slab's axis at the start, then the last step's.
    int axis = d.y < 0.0 ? 1 : 1;
    for (int i = 0; i < 220; i++) {
        if (t >= slab.y || T < 0.01) break;
        float exitT = min(min(tNext.x, tNext.y), tNext.z);
        float segEnd = min(exitT, slab.y);
        vec3 centre = (cell + 0.5) * B;
        float cov = sfCoverage(centre.xz);
        float db = cov > 0.02 ? sfCloudDensitySmooth(centre, cov, false) : 0.0;
        if (db > 0.42) {
            // A body box, solid through.
            float sigma = 0.9 * mtnClouds.w;
            float len = segEnd - t;
            vec3 pm = o + d * (0.5 * (t + segEnd));
            float Ts = exp(-sigma * len);
            if (t < sfCloudFront) { sfCloudFront = t; sfCloudFrontBox = B; }
            L += T * sfCloudLight(pm, cov, sigma, mu, sun, sunColour, skyUp, groundUp) * (1.0 - Ts) * sfBoxFace(axis, stepDir, sun);
            sumW += T * (1.0 - Ts); sumT += T * (1.0 - Ts) * t;
            T *= Ts;
        } else if (db > 0.1 && t < 80000.0) {
            // Partly cloudy: its detail boxes.
            float b = B / 3.0;
            float tt = t;
            vec3 q0 = o + d * (tt + 1e-3);
            vec3 sc = floor(q0 / b);
            vec3 sNext = vec3(tt, tt, tt) + abs((sc + max(stepDir, vec3(0.0, 0.0, 0.0))) * b - q0) * invD;
            vec3 sDelta = b * invD;
            int subAxis = axis;
            for (int k = 0; k < 9; k++) {
                if (tt >= segEnd || T < 0.01) break;
                float sExit = min(min(min(sNext.x, sNext.y), sNext.z), segEnd);
                vec3 c2 = (sc + 0.5) * b;
                if (sfCloudDensitySmooth(c2, cov, sfOn(SF_CLOUD_DETAIL)) > 0.3) {
                    float sigma = 0.75 * mtnClouds.w;
                    float len = sExit - tt;
                    vec3 pm = o + d * (0.5 * (tt + sExit));
                    float Ts = exp(-sigma * len);
                    if (tt < sfCloudFront) { sfCloudFront = tt; sfCloudFrontBox = b; }
                    L += T * sfCloudLight(pm, cov, sigma, mu, sun, sunColour, skyUp, groundUp) * (1.0 - Ts) * sfBoxFace(subAxis, stepDir, sun);
                    sumW += T * (1.0 - Ts); sumT += T * (1.0 - Ts) * tt;
                    T *= Ts;
                }
                tt = sExit;
                if (sNext.x <= sNext.y && sNext.x <= sNext.z) { sc.x += stepDir.x; sNext.x += sDelta.x; subAxis = 0; }
                else if (sNext.y <= sNext.z) { sc.y += stepDir.y; sNext.y += sDelta.y; subAxis = 1; }
                else { sc.z += stepDir.z; sNext.z += sDelta.z; subAxis = 2; }
            }
        }
        t = exitT;
        if (tNext.x <= tNext.y && tNext.x <= tNext.z) { cell.x += stepDir.x; tNext.x += tDelta.x; axis = 0; }
        else if (tNext.y <= tNext.z) { cell.y += stepDir.y; tNext.y += tDelta.y; axis = 1; }
        else { cell.z += stepDir.z; tNext.z += tDelta.z; axis = 2; }
    }
    tCloud = sumW > 1e-4 ? sumT / sumW : slab.y;
    return vec4(L, T);
}

vec4 sfCloudMarch(vec3 o, vec3 d, float tMax, vec3 sun, vec3 sunColour, vec3 skyUp, vec3 groundUp,
                  float jitter, int steps, out float tCloud) {
    tCloud = tMax;
    sfCloudFront = 1e9;
    sfCloudFrontBox = 0.0;
    if (!sfOn(SF_CLOUDS)) return vec4(0.0, 0.0, 0.0, 1.0);
    vec2 slab = sfCloudSlab(o, d, min(tMax, 300000.0), 7000.0);   // to the horizon (the curved deck sinks ~7 km by 300 km)
    if (slab.y <= slab.x) return vec4(0.0, 0.0, 0.0, 1.0);
    if (sfOn2(SF2_CLOUD_BLOCKY)) return sfCloudMarchBlocky(o, d, slab, sun, sunColour, skyUp, groundUp, tCloud);
    float mu = dot(d, sun);
    float T = 1.0;
    vec3 L = vec3(0.0, 0.0, 0.0);
    float len = slab.y - slab.x;
    float dt = len / float(steps);
    dt = clamp(dt, 40.0, 300.0);
    float t = slab.x + dt * jitter;
    float sumW = 0.0, sumT = 0.0;
    for (int i = 0; i < 160; i++) {
        if (i >= steps * 4 || t > slab.y || T < 0.01) break;
        vec3 p = o + d * t;
        float c = sfCoverage(p.xz);
        float den = c > 0.02 ? sfCloudDensity(p, c, sfOn(SF_CLOUD_DETAIL)) : 0.0;
        if (den > 0.0) {
            sfCloudFront = min(sfCloudFront, t);
            float sigma = den * mtnClouds.w;
            vec3 S = sigma * sfCloudLight(p, c, sigma, mu, sun, sunColour, skyUp, groundUp);
            float Ts = exp(-sigma * dt);
            L += T * S * (1.0 - Ts) / max(sigma, 1e-6);   // energy-conserving integration (Hillaire)
            sumW += T * (1.0 - Ts);
            sumT += T * (1.0 - Ts) * t;
            T *= Ts;
        }
        // Longer steps far off: a cloud 100 km away is a few pixels, and the horizon is 250 km out.
        t += dt * (1.0 + t / 12000.0);
    }
    if (sumW > 1e-4) tCloud = sumT / sumW;
    return vec4(L, T);
}

// High cirrus (santa-fe-reference S11): a thin sheet ~10 km up, streaks drawn out along the wind, lit by the
// sun with a strong forward lobe. 2D and cheap; behind the cumulus, in front of the sky. rgb its light, a its
// opacity (0 where the view does not reach it).
vec4 sfCirrus(vec3 o, vec3 d, vec3 sun, vec3 sunColour, vec3 skyUp) {
    if (!sfOn(SF_CLOUDS) || d.y <= 0.004) return vec4(0.0, 0.0, 0.0, 0.0);
    float alt = 10500.0;
    float t = (alt - o.y) / d.y;
    if (t <= 0.0 || t > 600000.0) return vec4(0.0, 0.0, 0.0, 0.0);
    vec2 xz = o.xz + d.xz * t + mtnWind.xy * 2.0;
    vec2 a = vec2(xz.x * 0.9 + xz.y * 0.43, xz.y * 0.9 - xz.x * 0.43);   // the jet's direction
    vec2 q = vec2(a.x / 90000.0, a.y / 14000.0);                        // drawn out along it
    float n = sfNoiseSlice(23.0, fract(q) * SF_NOISE_RES).r * 0.6 + sfNoiseSlice(57.0, fract(q * 3.7 + 0.21) * SF_NOISE_RES).r * 0.4;
    float patch = sfNoiseSlice(91.0, fract(xz / 160000.0 + 0.4) * SF_NOISE_RES).r;
    float alpha = smoothstep(0.52, 0.8, n) * smoothstep(0.35, 0.7, patch) * 0.55;
    alpha *= smoothstep(0.004, 0.06, d.y);   // thins to nothing at the horizon
    if (alpha <= 0.0) return vec4(0.0, 0.0, 0.0, 0.0);
    float mu = dot(d, sun);
    vec3 L = (sunColour * (0.25 + 1.6 * sfHG(mu, 0.75)) + skyUp * 0.9) * mtnWind.w;
    return vec4(L, alpha);
}

#endif
