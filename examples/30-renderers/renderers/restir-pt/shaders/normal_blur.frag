$input v_color0
$input v_texcoord0

// normal_blur.frag -- render resolution, after the G-buffer: the normals blurred, for softer voxel edges.
// Each texel's normal is averaged with those of the surface points round it in the world -- a Gaussian in
// world distance, restirNormals.x of a voxel's edge wide (gFace.w: the voxel's own, so a coarse voxel rounds
// as much as a fine one) -- so where two faces meet, the shading turns from one to the other over that width
// instead of at the edge. Only outside edges round: a point counts only if it lies on or below this face's
// plane, as the face round a convex edge does; round an inside corner the other face rises above it, and is
// left out, so a corner stays crisp. Points farther than the width in the world count for nothing, however
// near on screen: what stands in front of or behind this one is another surface.
//
// Water (RESTIR_WATER_BIT) is not blurred this way: its upward faces take the normal of a moving sea instead --
// restirWaves, flow noise over the world's x and z, low-passed over the texel's footprint.
//
// Only the shading reads it: the sun (shadow_soft) and the paths' first vertex (surface). The tests for "the
// same surface" (the denoise, the soft shadow's plane, the reconstruction) keep the G-buffer's own normal.
//
// Inputs: 0-6 the G-buffer (gPos 0, gNormal 1, gDirect 3, gFace 4). Output: gSoftNormal (xyz, unit; w the slope variance the waves' faded octaves took with them, water only).

#include <bgfx_shader.sh>
#include "restir_common.sc"

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gDirect, 3);
SAMPLER2D(gFace, 4);

uniform vec4 restirNormals;   // x the blur's width, in voxel edges (0: off); y seconds, for the waves

// The water: flow noise (Perlin and Neyret, "Flow Noise", 2001). Gradient noise whose lattice gradients
// each turn on the spot -- at its own speed, in its own sense -- so the surface churns: nothing travels
// across it (no current), and nothing swings between two shapes, as a sum of standing waves did, because
// no two cells keep time together. Six octaves, 60 down to 2 of the finest voxels; each turns at deep
// water's rate for its size (omega ~ sqrt(g k), g in finest voxels: 9.8 m/s^2 at the Teardown scale's ten
// a metre), so the ripples churn faster than the swell. Each finer octave is also displaced by the coarser
// ones' slope (the paper's pseudo-advection), so the ripples ride the swell instead of a fixed grid.

// A lattice point's gradient at time `time`: a random direction, turned at `rate` radians a second, either
// way round, at between half and one and a half times that rate.
vec2 restirFlowGradient(ivec2 cell, float rate, float time) {
    uint h = uint(cell.x) * 0x8da6b343u ^ uint(cell.y) * 0xd8163841u;
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    h ^= h >> 16u;
    float angle = float(h & 0xffffu) * (6.2831853 / 65536.0);
    float spin = (float((h >> 16u) & 0x7fffu) / 32768.0 + 0.5) * ((h & 0x80000000u) != 0u ? -1.0 : 1.0);
    angle += spin * rate * time;
    return vec2(cos(angle), sin(angle));
}

// Gradient noise at p and its derivative (x: the value, yz: d/dp), quintic interpolation (Quilez's
// "gradient noise derivatives").
vec3 restirFlowNoise(vec2 p, float rate, float time) {
    ivec2 i = ivec2(floor(p));
    vec2 f = p - floor(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    vec2 ga = restirFlowGradient(i, rate, time);
    vec2 gb = restirFlowGradient(i + ivec2(1, 0), rate, time);
    vec2 gc = restirFlowGradient(i + ivec2(0, 1), rate, time);
    vec2 gd = restirFlowGradient(i + ivec2(1, 1), rate, time);
    float va = dot(ga, f);
    float vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0));
    float vd = dot(gd, f - vec2(1.0, 1.0));
    float value = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
    vec2 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd)
           + du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va);
    return vec3(value, d);
}

// The slope of the water at world xz point `xz`, with octaves shorter than `footprint` (world units: how much
// water one render texel covers along its longest side) faded out -- gone under 8 footprints, whole over 20.
// `lost` gets the slope variance of what faded, per axis.
vec2 restirWaveSlope(vec2 xz, float footprint, out float lost) {
    float finest = restirSettings.y * 20.0;
    float g = 98.0 * finest;
    float time = restirNormals.y;
    vec2 slope = vec2(0.0, 0.0);
    lost = 0.0;
    for (int o = 0; o < 6; o++) {
        float wavelength = finest * 60.0 * pow(0.5, float(o));
        float k = 6.2831853 / wavelength;
        float rate = 0.3 * sqrt(g * k);
        // The octave's own offset, so octaves do not share a lattice; then the pseudo-advection.
        vec2 p = xz / wavelength + vec2(float(o) * 13.7, float(o) * 7.3) - slope * 1.5;
        vec3 n = restirFlowNoise(p, rate, time);
        float fade = smoothstep(8.0, 20.0, wavelength / footprint);
        slope += n.yz * (0.16 * fade);
        lost += (1.0 - fade) * 0.16 * 0.16 * 0.25;   // the octave's slope variance per axis (|dn/dp| about 0.7)
    }
    return slope;
}

// The water's surface normal at world point P, `texel` world units a render texel wide there, seen along V.
// A low-pass over the texel's footprint on the water: at a grazing angle a texel covers 1/V.y times more
// water along the view than across it, and one sample of a wave shorter than that shimmers (the sun's glint,
// a near-mirror lobe, worst). So the slope is averaged over that ellipse (a fixed spiral, the same every
// frame), and the octaves it cannot hold fade against its long side. `lost` is the slope variance of the
// faded octaves, which shadow_soft widens the sun's glint lobe by.
#define WATER_TAPS 8
#define WATER_BLUR_SCALE 0.2   // the footprint's share used: 0.2 starts the blur 5x further out
vec3 restirWaves(vec3 P, float texel, vec3 V, out float lost) {
    float stretch = 1.0 / max(V.y, 0.06);
    vec2 along = normalize(V.xz + vec2(1e-5, 0.0));
    vec2 across = vec2(-along.y, along.x);
    texel *= WATER_BLUR_SCALE;
    float footprint = texel * stretch;
    vec2 sum = vec2(0.0, 0.0);
    lost = 0.0;
    for (int i = 0; i < WATER_TAPS; i++) {
        float r = sqrt((float(i) + 0.5) / float(WATER_TAPS));
        float a = float(i) * 2.39996323;
        vec2 d = (across * (cos(a) * texel) + along * (sin(a) * footprint)) * r;
        float l;
        sum += restirWaveSlope(P.xz + d, footprint, l);
        lost += l;
    }
    sum *= 1.0 / float(WATER_TAPS);
    lost *= 1.0 / float(WATER_TAPS);
    return normalize(vec3(-sum.x, 1.0, -sum.y));
}

#define NORMAL_BLUR_STRENGTH 0.5    // how far towards the blurred normal: half, so an edge's two faces meet at
                                    // most about 22 degrees from their own -- a bevel catching light, not a round
#define NORMAL_BLUR_TAPS 48
#define NORMAL_BLUR_MAX_RADIUS 32.0   // render texels: the widest the disc may be on screen

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(gPos, p, 0);
    vec3 N = normalize(texelFetch(gNormal, p, 0).xyz + vec3(0.0, 1e-6, 0.0));
    float edge = texelFetch(gFace, p, 0).w;
    if (surface.w < 0.0 || edge <= 0.0) { gl_FragColor = vec4(N, 0.0); return; }
    float texel = max(restirSettings.w * surface.w, 1.0e-6);
    if (restirIsWater(texelFetch(gDirect, p, 0).a)) {
        float lost = 0.0;
        vec3 waves = N.y > 0.5 ? restirWaves(surface.xyz, texel, normalize(cameraPos.xyz - surface.xyz), lost) : N;
        gl_FragColor = vec4(waves, lost);
        return;
    }
    if (restirNormals.x <= 0.0) { gl_FragColor = vec4(N, 0.0); return; }

    // The disc on screen: the blur's width in the world over a render texel's width at this distance.
    float width = restirNormals.x * edge;
    float radius = min(width / texel, NORMAL_BLUR_MAX_RADIUS);
    float sigma = 0.5 * width;
    ivec2 last = ivec2(passTargetRes.xy) - ivec2(1, 1);
    vec3 sum = N;
    // A fixed golden-angle spiral, the same every frame: the normal is an input to the GI's history tests,
    // and a pattern that turned would make it flicker.
    for (int i = 0; i < NORMAL_BLUR_TAPS; i++) {
        float r = sqrt((float(i) + 0.5) / float(NORMAL_BLUR_TAPS)) * radius;
        float a = float(i) * 2.39996323;
        ivec2 q = clamp(p + ivec2(floor(vec2(cos(a), sin(a)) * r + 0.5)), ivec2(0, 0), last);
        vec4 qs = texelFetch(gPos, q, 0);
        if (qs.w < 0.0) continue;
        vec3 d = qs.xyz - surface.xyz;
        float d2 = dot(d, d);
        if (d2 > 2.25 * width * width || dot(d, N) > 0.05 * edge) continue;   // too far, or an inside corner
        vec3 qn = texelFetch(gNormal, q, 0).xyz;
        sum += qn * exp(-d2 / (2.0 * sigma * sigma));
    }
    vec3 soft = normalize(mix(N, normalize(sum + vec3(0.0, 1e-6, 0.0)), NORMAL_BLUR_STRENGTH));
    // Never past the face's own plane: a normal facing into the surface would send its paths into it.
    if (dot(soft, N) < 0.1) soft = normalize(soft + N * (0.1 - dot(soft, N)));
    gl_FragColor = vec4(soft, 0.0);
}
