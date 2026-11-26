// =============================================================================
// santa_fe_atmosphere.sc -- the sky as a planet's atmosphere (santa-fe-reference S29b): Hillaire 2020, "A Scalable
// and Production Ready Sky and Atmosphere Rendering Technique" (the model behind Unreal's SkyAtmosphere).
//
// A sphere of radius SF_RG with the air to SF_RT; world y is metres above the sphere (sea level), and a point's
// place on the planet is taken from the camera's: P = (x - cam.x, y + SF_RG, z - cam.z). Three media:
//
//   Rayleigh  scattering (5.802, 13.558, 33.1) e-6 /m x the air density (mtnAtmos.x), scale height 8 km
//   Mie       scattering 3e-6 /m x the haze (mtnAtmos.y: 40, ~0.03 optical depth over Santa Fe), extinction
//             1.11 x that, scale height 1.2 km down to SF_HAZE_FLOOR and constant below, Cornette-Shanks phase with g mtnAtmos.z
//   ozone     absorption (0.650, 1.881, 0.085) e-6 /m, a tent from 10 to 40 km peaking at 25 km
//
// Everything is for a sun of 1 at the top of the atmosphere; sfSunTop.rgb is the sun there. atmosphere.frag
// keeps one RGBA32F atlas, SF_ATMO_W x SF_ATMO_H, of four tables (each read the last frame's others):
//
//   transmittance   256 x 64 at (0, 0)     to the top of the atmosphere, by (r, mu) as Bruneton maps them
//   multiple scat.  32 x 32 at (256, 0)    psi (Hillaire's eq. 10): the light scattered more than once, by
//                                          (mu_s, height)
//   sky view        192 x 108 at (0, 64)   the sky's light from the camera by direction (Hillaire's mapping:
//                                          latitude dense at the horizon, longitude from the sun)
//   aerial persp.   32 slices of 32 x 32 at (0, 192): the camera's frustum in 32 x 32 columns and 32 depths to
//                                          SF_AP_RANGE (quadratic); rgb the light the air adds, a its mean
//                                          transmittance
//
// A shader that reads it defines SF_ATMO_SLOT (its sampler stage) before including this.
// =============================================================================

#ifndef SANTA_FE_ATMOSPHERE_SC
#define SANTA_FE_ATMOSPHERE_SC

uniform vec4 windowRes;  // xy: the window (the froxels' aspect)
uniform vec4 sfMoon;     // xyz: toward the moon; w: 1 when it is the key light (sunDir is then the moon)
uniform vec4 sfSunTrue;  // xyz: toward the sun, always; w: the sun above the air (it lights the moon's face)
uniform vec4 sfNight;    // x: a magnitude-0 star's light; y: the sky's turn about the pole; z: the latitude; w: a texel's angle
uniform vec4 sfSunTop;   // rgb: the sun's irradiance at the top of the atmosphere; w: the atlas has been written (1)

#define SF_RG 6360000.0
#define SF_RT 6460000.0
#define SF_ATMO_W 1024.0
#define SF_ATMO_H 224.0
#define SF_TR_W 256.0
#define SF_TR_H 64.0
#define SF_MS_X 256.0
#define SF_MS_N 32.0
#define SF_SV_Y 64.0
#define SF_SV_W 192.0
#define SF_SV_H 108.0
#define SF_AP_Y 192.0
#define SF_AP_N 32.0
#define SF_AP_RANGE 128000.0
#define SF_SUN_ANGULAR_RADIUS 0.004675   // the sun's disc, radians
#define SF_HAZE_FLOOR 1700.0             // m: the haze no denser below it (sfMedium)

// ---- The media ----
struct SfMedium { vec3 scatR; float scatM; vec3 ext; };

SfMedium sfMedium(float h) {
    SfMedium m;
    float dR = exp(-max(h, -500.0) / 8000.0);
    // The haze a layer over the land, not denser all the way to the sea: constant below SF_HAZE_FLOOR (Santa Fe's
    // valley floor, the fog's base). Extrapolated down, a grazing path at sea level 400 km off had an optical depth
    // of ~26 and a -2 degree sun lit no cloud.
    float dM = exp(-max(h, SF_HAZE_FLOOR) / 1200.0);
    float dO = max(0.0, 1.0 - abs(h - 25000.0) / 15000.0);
    m.scatR = vec3(5.802e-6, 13.558e-6, 33.1e-6) * mtnAtmos.x * dR;
    m.scatM = 3.0e-6 * mtnAtmos.y * dM;
    vec3 absO = vec3(0.650e-6, 1.881e-6, 0.085e-6) * dO;
    m.ext = m.scatR + vec3(m.scatM * 1.11, m.scatM * 1.11, m.scatM * 1.11) + absO;
    return m;
}

float sfPhaseR(float mu) { return 0.0596831 * (1.0 + mu * mu); }
// Cornette-Shanks (a Henyey-Greenstein closer to Mie's for haze).
float sfPhaseM(float mu, float g) {
    float g2 = g * g;
    return 0.1193662 * (1.0 - g2) * (1.0 + mu * mu) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
}

// ---- Geometry ----
// The point's place on the planet, from its world position (the camera over the planet's pole of this frame).
vec3 sfPlanetPos(vec3 world) { vec3 c = pjvViewPosition(); return vec3(world.x - c.x, world.y + SF_RG, world.z - c.z); }

// The distance along d from p (|p| = r) to the sphere of radius R; -1 when it misses (or is behind).
float sfRaySphere(vec3 p, vec3 d, float R) {
    float b = dot(p, d);
    float c = dot(p, p) - R * R;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    if (t0 > 0.0) return t0;
    if (t1 > 0.0) return t1;
    return -1.0;
}

// ---- The transmittance table's mapping (Bruneton 2017) ----
vec2 sfTransmittanceUV(float r, float mu) {
    float H = sqrt(SF_RT * SF_RT - SF_RG * SF_RG);
    float rho = sqrt(max(r * r - SF_RG * SF_RG, 0.0));
    float disc = r * r * (mu * mu - 1.0) + SF_RT * SF_RT;
    float d = max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
    float dMin = SF_RT - r, dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1.0);
    float xR = rho / H;
    // Texel centres at the ends.
    return vec2(0.5 / SF_TR_W + xMu * (1.0 - 1.0 / SF_TR_W), 0.5 / SF_TR_H + xR * (1.0 - 1.0 / SF_TR_H));
}

void sfTransmittanceRMu(vec2 uv, out float r, out float mu) {
    float xMu = (uv.x - 0.5 / SF_TR_W) / (1.0 - 1.0 / SF_TR_W);
    float xR = (uv.y - 0.5 / SF_TR_H) / (1.0 - 1.0 / SF_TR_H);
    float H = sqrt(SF_RT * SF_RT - SF_RG * SF_RG);
    float rho = H * clamp(xR, 0.0, 1.0);
    r = sqrt(rho * rho + SF_RG * SF_RG);
    float dMin = SF_RT - r, dMax = rho + H;
    float d = dMin + clamp(xMu, 0.0, 1.0) * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

// ---- The multiple-scattering table's mapping ----
vec2 sfMultiUV(float r, float muS) {
    float u = clamp(muS * 0.5 + 0.5, 0.0, 1.0);
    float v = clamp((r - SF_RG) / (SF_RT - SF_RG), 0.0, 1.0);
    return vec2(0.5 / SF_MS_N + u * (1.0 - 1.0 / SF_MS_N), 0.5 / SF_MS_N + v * (1.0 - 1.0 / SF_MS_N));
}

// ---- The sky-view table's mapping (Hillaire's, longitude from the sun's azimuth) ----
// uv in [0, 1]^2 -> the view's zenith cosine and its cosine to the sun's azimuth, for an eye at r.
void sfSkyViewDir(vec2 uv, float r, out float cosZenith, out float cosLight) {
    float vHorizon = sqrt(max(r * r - SF_RG * SF_RG, 0.0));
    float beta = acos(clamp(vHorizon / r, -1.0, 1.0));   // the horizon's dip below level
    float zenithHorizon = 3.14159265 - beta;
    float zen;
    if (uv.y < 0.5) {
        float c = 1.0 - 2.0 * uv.y;
        c = 1.0 - c * c;
        zen = zenithHorizon * c;
    } else {
        float c = uv.y * 2.0 - 1.0;
        zen = zenithHorizon + beta * c * c;
    }
    cosZenith = cos(zen);
    float cl = uv.x * uv.x;
    cosLight = -(cl * 2.0 - 1.0);
}

vec2 sfSkyViewUV(float r, float cosZenith, float cosLight) {
    float vHorizon = sqrt(max(r * r - SF_RG * SF_RG, 0.0));
    float beta = acos(clamp(vHorizon / r, -1.0, 1.0));
    float zenithHorizon = 3.14159265 - beta;
    float zen = acos(clamp(cosZenith, -1.0, 1.0));
    float v;
    if (zen < zenithHorizon) {
        float c = zen / zenithHorizon;
        c = sqrt(max(1.0 - c, 0.0));
        v = 0.5 * (1.0 - c);
    } else {
        float c = (zen - zenithHorizon) / max(beta, 1e-4);
        v = 0.5 + 0.5 * sqrt(clamp(c, 0.0, 1.0));
    }
    float u = sqrt(clamp(-cosLight * 0.5 + 0.5, 0.0, 1.0));
    return vec2(u, v);
}

#ifdef SF_ATMO_SLOT
SAMPLER2D(sfAtmo, SF_ATMO_SLOT);

bool sfAtmoReady() { return sfSunTop.w > 0.5; }

// A bilinear read of the atlas inside a region (uv in its own [0, 1], the region's origin and size in texels).
vec4 sfAtmoRead(vec2 uv, vec2 origin, vec2 size) {
    // Held to the region's texel centres: a bilinear read at its edge would blend in the atlas's next region.
    vec2 px = origin + clamp(uv * size, vec2(0.5, 0.5), size - vec2(0.5, 0.5));
    return texture2DLod(sfAtmo, px / vec2(SF_ATMO_W, SF_ATMO_H), 0.0);
}

// Transmittance from a point at r, along mu, to the top of the atmosphere: 0 below the planet's horizon, softened
// over the sun's disc (the terminator is the disc setting, not a line).
vec3 sfTransmittanceToTop(float r, float mu) {
    float muHorizon = -sqrt(max(1.0 - (SF_RG / r) * (SF_RG / r), 0.0));
    float vis = smoothstep(-SF_SUN_ANGULAR_RADIUS, SF_SUN_ANGULAR_RADIUS, mu - muHorizon);
    if (vis <= 0.0) return vec3(0.0, 0.0, 0.0);
    return sfAtmoRead(sfTransmittanceUV(r, max(mu, muHorizon)), vec2(0.0, 0.0), vec2(SF_TR_W, SF_TR_H)).rgb * vis;
}

vec3 sfMultiScat(float r, float muS) {
    return sfAtmoRead(sfMultiUV(r, muS), vec2(SF_MS_X, 0.0), vec2(SF_MS_N, SF_MS_N)).rgb;
}

// The sun's light reaching a point (world position): the top's, through the air on its way.
vec3 sfSunLightAt(vec3 world, vec3 sun) {
    vec3 P = sfPlanetPos(world);
    float r = length(P);
    return sfSunTop.rgb * sfTransmittanceToTop(r, dot(P / r, sun));
}

// The sky's light from the camera along dir (no sun disc).
vec3 sfSkyViewLight(vec3 dir, vec3 sun) {
    vec3 P = sfPlanetPos(pjvViewPosition());
    float r = length(P);
    vec3 up = P / r;
    float cosZ = dot(dir, up);
    vec2 dh = vec2(dir.x, dir.z), sh = vec2(sun.x, sun.z);
    float ld = length(dh), ls = length(sh);
    float cosL = ld > 1e-5 && ls > 1e-5 ? dot(dh / ld, sh / ls) : 1.0;
    vec2 uv = sfSkyViewUV(r, cosZ, cosL);
    return sfAtmoRead(uv, vec2(0.0, SF_SV_Y), vec2(SF_SV_W, SF_SV_H)).rgb * sfSunTop.rgb;
}

// The air between the camera and a point d metres along dir: rgb the light it adds, a its transmittance. From
// the froxels where dir is on the screen; off it (a reflection, a shadow's sample) from the nearest column.
vec4 sfAerialFromCamera(vec3 dir, float d) {
    bool inFront;
    vec3 c = pjvViewPosition();
    vec2 uv = pjvPerspectiveUV(c + dir * 100.0, max(windowRes.xy, vec2(1.0, 1.0)), c, pjvViewDirection(), pjvViewTanHalfFov(), inFront);
    if (!inFront) uv = vec2(0.5, 0.5);
    uv = clamp(uv, vec2(0.5 / SF_AP_N, 0.5 / SF_AP_N), vec2(1.0 - 0.5 / SF_AP_N, 1.0 - 0.5 / SF_AP_N));
    // Slices at depths range (k + 1)^2 / N^2 (quadratic: dense near), the first from 0.
    float s = sqrt(clamp(d / SF_AP_RANGE, 0.0, 1.0)) * SF_AP_N - 1.0;
    float s0 = floor(s);
    float f = s - s0;
    vec2 px = uv * SF_AP_N;
    vec4 a = s0 < 0.0 ? vec4(0.0, 0.0, 0.0, 1.0)
                      : texture2DLod(sfAtmo, (vec2(s0 * SF_AP_N, SF_AP_Y) + px) / vec2(SF_ATMO_W, SF_ATMO_H), 0.0);
    vec4 b = texture2DLod(sfAtmo, (vec2(min(s0 + 1.0, SF_AP_N - 1.0) * SF_AP_N, SF_AP_Y) + px) / vec2(SF_ATMO_W, SF_ATMO_H), 0.0);
    // Below the first slice, a share of it by the distance (the air is thin there; linear is near enough).
    if (s0 < 0.0) f = clamp(s + 1.0, 0.0, 1.0);
    vec4 v = mix(a, b, f);
    return vec4(v.rgb * sfSunTop.rgb, v.a);
}
#endif


// ---- The night sky (S29f) ----
float sfHash(vec3 p) { p = fract(p * 0.3183099 + vec3(0.1, 0.71, 0.113)); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float sfValueNoise2(vec2 x) {
    vec2 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = sfHash(vec3(i, 0.0)), b = sfHash(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = sfHash(vec3(i + vec2(0.0, 1.0), 0.0)), d = sfHash(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// A world direction in the sky's own frame: the celestial pole up (north is +z, at the latitude's height), turned
// by the sky's rotation about it.
vec3 sfCelestial(vec3 d) {
    float lat = sfNight.z;
    vec3 pole = vec3(0.0, sin(lat), cos(lat));
    vec3 east = vec3(1.0, 0.0, 0.0);
    vec3 north = cross(pole, east);
    vec3 c = vec3(dot(d, east), dot(d, north), dot(d, pole));
    float a = sfNight.y, ca = cos(a), sa = sin(a);
    return vec3(ca * c.x - sa * c.y, sa * c.x + ca * c.y, c.z);
}

// The stars: one in each cell of a 160-a-face cube over the celestial sphere (most cells), its magnitude by a power
// law (many faint, a few bright: 1 to 7.5), its colour by temperature; each spread as a Gaussian over a texel's
// angle (sfNight.w) so its light is conserved whatever the resolution. The Milky Way: a band along the galactic
// plane (62.9 degrees to the equator) with dust lanes; airglow: the upper air's own faint light, brighter toward
// the horizon (the van Rhijn factor). Radiances in the renderer's units (a lux is kLuxToUnits).
vec3 sfNightSky(vec3 dir) {
    if (sfNight.x <= 0.0) return vec3(0.0, 0.0, 0.0);
    vec3 c = sfCelestial(dir);
    vec3 a = abs(c);
    float face; vec2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = c.x > 0.0 ? 0.0 : 1.0; uv = c.yz / a.x; }
    else if (a.y >= a.z) { face = c.y > 0.0 ? 2.0 : 3.0; uv = c.xz / a.y; }
    else { face = c.z > 0.0 ? 4.0 : 5.0; uv = c.xy / a.z; }
    float N = 160.0;
    vec2 g = (uv * 0.5 + 0.5) * N;
    vec2 cell0 = floor(g - 0.5);
    float sigma = max(sfNight.w * 0.7, 0.0004);
    vec3 L = vec3(0.0, 0.0, 0.0);
    for (int k = 0; k < 4; k++) {
        vec2 cell = cell0 + vec2(float(k & 1), float(k >> 1));
        float h0 = sfHash(vec3(cell, face * 7.0 + 1.0));
        if (h0 > 0.8) continue;
        vec2 sp = (cell + vec2(sfHash(vec3(cell, face * 7.0 + 2.0)), sfHash(vec3(cell, face * 7.0 + 3.0)))) / N * 2.0 - 1.0;
        vec3 sd = face < 0.5 ? vec3(1.0, sp.x, sp.y) : face < 1.5 ? vec3(-1.0, sp.x, sp.y) : face < 2.5 ? vec3(sp.x, 1.0, sp.y)
                : face < 3.5 ? vec3(sp.x, -1.0, sp.y) : face < 4.5 ? vec3(sp.x, sp.y, 1.0) : vec3(sp.x, sp.y, -1.0);
        sd = normalize(sd);
        float ang = acos(clamp(dot(sd, c), -1.0, 1.0));
        if (ang > 4.0 * sigma) continue;
        float mag = 1.0 + 6.5 * pow(sfHash(vec3(cell, face * 7.0 + 4.0)), 0.25);
        float E = sfNight.x * pow(10.0, -0.4 * mag);
        float t = sfHash(vec3(cell, face * 7.0 + 5.0));
        vec3 tint = t < 0.15 ? vec3(0.75, 0.85, 1.0) : t < 0.7 ? vec3(1.0, 0.97, 0.92) : t < 0.92 ? vec3(1.0, 0.85, 0.65) : vec3(1.0, 0.7, 0.5);
        L += tint * E * exp(-0.5 * ang * ang / (sigma * sigma)) / (6.2831853 * sigma * sigma);
    }
    // The Milky Way: the galactic pole at RA 12h51m, Dec 27.1 deg, in the frame above (z the celestial pole).
    vec3 gp = vec3(cos(0.473) * cos(3.366), cos(0.473) * sin(3.366), sin(0.473));
    float b = asin(clamp(dot(c, gp), -1.0, 1.0));
    float band = exp(-b * b / (2.0 * 0.12 * 0.12));
    vec2 q = vec2(atan2(c.y, c.x) * 6.0, b * 20.0);
    float dust = 0.55 + 0.45 * sfValueNoise2(q * 1.7) * sfValueNoise2(q * 4.1 + 3.0);
    float lanes = smoothstep(0.05, 0.0, abs(b + 0.02)) * 0.5;
    vec3 milky = vec3(0.85, 0.88, 1.0) * band * dust * (1.0 - lanes) * 4.0e-4;   // cd/m^2 at its brightest
    // Airglow: ~1.5e-4 cd/m^2 at the zenith, by the van Rhijn factor (a 90 km layer seen slant).
    float cz = max(dir.y, 0.0);
    float sinZ2 = 1.0 - cz * cz, k2 = (SF_RG / (SF_RG + 90000.0)) * (SF_RG / (SF_RG + 90000.0));
    float rhijn = 1.0 / sqrt(max(1.0 - k2 * sinZ2, 0.02));
    vec3 glow = vec3(0.75, 1.0, 0.8) * 1.5e-4 * rhijn;
    // cd/m^2 to the renderer's radiance: sfNight.x is 2.08e-6 lux in units, so units per lux = x / 2.08e-6.
    return L + (milky + glow) * (sfNight.x / 2.08e-6);
}

// The moon's disc (0.26 deg): a grey sphere lit by the true sun (its phase is where that light falls on it), its
// maria darker; Lambertian, albedo 0.07-0.16.
vec3 sfMoonDisc(vec3 dir) {
    vec3 m = sfMoon.xyz;
    float cosA = dot(dir, m);
    float R = 0.00454;
    if (cosA < cos(R)) return vec3(0.0, 0.0, 0.0);
    vec3 right = normalize(cross(m, vec3(0.0, 1.0, 0.0)));
    vec3 upv = cross(right, m);
    vec2 xy = vec2(dot(dir - m * cosA, right), dot(dir - m * cosA, upv)) / sin(R);
    float r2 = dot(xy, xy);
    if (r2 >= 1.0) return vec3(0.0, 0.0, 0.0);
    vec3 n = right * xy.x + upv * xy.y - m * sqrt(1.0 - r2);   // its face toward us
    float lit = max(dot(n, sfSunTrue.xyz), 0.0);
    float maria = sfValueNoise2(xy * 2.3 + 1.7) * 0.6 + sfValueNoise2(xy * 5.1) * 0.4;
    float albedo = mix(0.07, 0.16, smoothstep(0.35, 0.65, maria));
    return vec3(1.0, 0.96, 0.88) * sfSunTrue.w * albedo * lit * 0.3183099;
}
// ---- The integration (atmosphere.frag's tables, and any pass that marches the air itself) ----
#ifdef SF_ATMO_SLOT
// Light scattered toward -dir along the ray from p (planet space) for tMax metres, in `steps` samples; T its
// transmittance. Per unit sun. `withMulti`: the multiple-scattering table's term.
vec3 sfIntegrate(vec3 p, vec3 dir, float tMax, vec3 sun, int steps, bool withMulti, out vec3 T) {
    float mu = dot(dir, sun);
    float pR = sfPhaseR(mu), pM = sfPhaseM(mu, mtnAtmos.z);
    vec3 L = vec3(0.0, 0.0, 0.0);
    T = vec3(1.0, 1.0, 1.0);
    float t = 0.0;
    for (int i = 0; i < 64; i++) {
        if (i >= steps) break;
        // Samples dense near the start (the air is denser low, and near is seen larger).
        float t1 = tMax * pow((float(i) + 1.0) / float(steps), 2.0);
        float dt = t1 - t;
        float tm = t + 0.5 * dt;
        t = t1;
        vec3 q = p + dir * tm;
        float r = length(q);
        float h = r - SF_RG;
        SfMedium m = sfMedium(h);
        vec3 up = q / r;
        float muS = dot(up, sun);
        vec3 sunT = sfTransmittanceToTop(r, muS);
        vec3 scatS = m.scatR + vec3(m.scatM, m.scatM, m.scatM);
        vec3 S = sunT * (m.scatR * pR + vec3(m.scatM * pM, m.scatM * pM, m.scatM * pM));
        if (withMulti) S += sfMultiScat(r, muS) * scatS;
        vec3 Tstep = exp(-m.ext * dt);
        // Hillaire's energy-conserving step: the light scattered over the step, as the step absorbs it.
        L += T * (S - S * Tstep) / max(m.ext, vec3(1e-12, 1e-12, 1e-12));
        T *= Tstep;
    }
    return L;
}
#endif

#endif
