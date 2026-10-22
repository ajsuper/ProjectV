$input v_color0
$input v_texcoord0

// =============================================================================
// smoke.frag -- the mushroom cloud's smoke, at render resolution, after the shade. Two ways of drawing
// it (smokeParams.z): 1 BLOCKS, the grid as solid voxel cubes, and 2 GAS, the grid marched as soft
// volumetric gas. 0 draws nothing (the cloud is its voxel chunks alone).
//
// The cloud is carried by particles on the CPU (physics bodies advected through the mushroom field);
// each frame they are splatted into a density grid of N^3 cells, uploaded as a 2D atlas of its z slices
// (smokeGrid: r = smoke density, g = density x temperature, b = density x dust, a = density x vapour). This pass marches the
// grid along the camera ray, stopping at the solid scene's depth:
//
//   extinction   the density, eroded by a rising fbm so the gas billows rather than blobs
//   emission     the hot part glows by its temperature: white-yellow, orange, deep red, then nothing
//   scattering   the moon, through a five-tap shadow march toward it; the fire's light from below;
//                the night sky
//
// Output (FBO 11): rgb the light the gas adds along the ray, a = how much of what is behind still shows.
// display.frag composites it over the frame: hdr * a + rgb.
// =============================================================================

#include <bgfx_shader.sh>
#include <grr_common.sc>

SAMPLER2D(gPos,      0);
SAMPLER2D(smokeGrid, 6);

uniform vec4 passTargetRes;
uniform vec4 smokeBox;        // xyz the grid's min corner (world), w the cell's edge (m)
uniform vec4 smokeGridInfo;   // x cells per axis, y tiles per atlas row, z atlas width, w atlas height
uniform vec4 smokeParams;     // x seconds, y extinction per unit density, z on, w fire emission strength
uniform vec4 smokeShape;      // x noise erosion, y noise frequency (1/m), z noise rise speed, w steps
uniform vec4 smokeBlocks;     // x the density a block is solid at, y lattice cells to a grid cell

#define MAX_STEPS 96
#define MOON_TAPS 5

float smokeHash(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float valueNoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = smokeHash(i), n100 = smokeHash(i + vec3(1.0, 0.0, 0.0));
    float n010 = smokeHash(i + vec3(0.0, 1.0, 0.0)), n110 = smokeHash(i + vec3(1.0, 1.0, 0.0));
    float n001 = smokeHash(i + vec3(0.0, 0.0, 1.0)), n101 = smokeHash(i + vec3(1.0, 0.0, 1.0));
    float n011 = smokeHash(i + vec3(0.0, 1.0, 1.0)), n111 = smokeHash(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

float fbm(vec3 p) { return 0.65 * valueNoise(p) + 0.35 * valueNoise(p * 2.37 + vec3(1.7, -3.1, 4.3)); }

// The grid at a world point: two bilinear taps in neighbouring z slices of the atlas, blended.
vec4 gridAt(vec3 p) {
    float N = smokeGridInfo.x;
    vec3 g = (p - smokeBox.xyz) / smokeBox.w - 0.5;
    if (any(lessThan(g, vec3(-0.5, -0.5, -0.5))) || any(greaterThan(g, vec3(N - 0.5, N - 0.5, N - 0.5))))
        return vec4(0.0, 0.0, 0.0, 0.0);
    g = clamp(g, vec3(0.0, 0.0, 0.0), vec3(N - 1.0, N - 1.0, N - 1.0));
    float z0 = floor(g.z);
    float fz = g.z - z0;
    float z1 = min(z0 + 1.0, N - 1.0);
    float tiles = smokeGridInfo.y;
    vec2 inTile = g.xy + 0.5;
    vec2 tile0 = vec2(mod(z0, tiles), floor(z0 / tiles)) * N;
    vec2 tile1 = vec2(mod(z1, tiles), floor(z1 / tiles)) * N;
    vec4 a = texture2DLod(smokeGrid, (tile0 + inTile) / smokeGridInfo.zw, 0.0);
    vec4 b = texture2DLod(smokeGrid, (tile1 + inTile) / smokeGridInfo.zw, 0.0);
    return mix(a, b, fz);
}

// The colour of glowing gas by its temperature (0 cold .. 1 the fireball's core), times how bright.
vec3 fireColour(float heat) {
    vec3 c = mix(vec3(0.55, 0.04, 0.0), vec3(1.0, 0.32, 0.03), smoothstep(0.12, 0.45, heat));
    c = mix(c, vec3(1.0, 0.62, 0.18), smoothstep(0.45, 0.75, heat));
    c = mix(c, vec3(1.0, 0.9, 0.65), smoothstep(0.8, 1.0, heat));
    float glow = pow(smoothstep(0.06, 1.0, heat), 1.6);
    return c * glow;
}

float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * GRR_PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// ---- BLOCKS --------------------------------------------------------------------------------------
// The grid as voxels: a DDA through a lattice of blockSubdivision cells to each grid cell, stopping at the
// first one dense enough to be solid (the density at its centre, jittered per cell so the surface is
// ragged rather than an isosurface). The block is lit by its face: the moon through the smoke toward it,
// the night sky, the fire's light, and its own glow by temperature.
#define BLOCK_STEPS 240

float cellHash(vec3 c) { return smokeHash(c * 1.37 + vec3(11.3, 5.7, 3.1)); }

vec4 drawBlocks(vec3 dir, float tEnter, float tExit) {
    float N = smokeGridInfo.x;
    float sub = max(smokeBlocks.y, 1.0);
    float L = smokeBox.w / sub;
    float cells = N * sub;
    vec3 start = cameraPos.xyz + dir * (tEnter + 1e-3);
    vec3 u = clamp((start - smokeBox.xyz) / L, vec3(0.0, 0.0, 0.0), vec3(cells - 1e-3, cells - 1e-3, cells - 1e-3));
    vec3 c = floor(u);
    vec3 stepDir = vec3(dir.x >= 0.0 ? 1.0 : -1.0, dir.y >= 0.0 ? 1.0 : -1.0, dir.z >= 0.0 ? 1.0 : -1.0);
    vec3 safe = vec3(abs(dir.x) < 1e-6 ? 1e-6 : dir.x, abs(dir.y) < 1e-6 ? 1e-6 : dir.y, abs(dir.z) < 1e-6 ? 1e-6 : dir.z);
    vec3 tDelta = abs(vec3(L, L, L) / safe);
    vec3 tNext = tEnter + ((c + max(stepDir, vec3(0.0, 0.0, 0.0))) - u) * L / safe;
    vec3 normal = vec3(0.0, 0.0, 0.0);
    float t = tEnter;
    for (int i = 0; i < BLOCK_STEPS; i++) {
        if (t > tExit) break;
        if (any(lessThan(c, vec3(0.0, 0.0, 0.0))) || any(greaterThanEqual(c, vec3(cells, cells, cells)))) break;
        vec3 centre = smokeBox.xyz + (c + 0.5) * L;
        vec4 cell = gridAt(centre);
        float jitter = cellHash(c);
        if (cell.x * (0.6 + 0.8 * jitter) > smokeBlocks.x) {
            if (dot(normal, normal) < 0.5) normal = -dir;   // the camera is inside it
            float heat = clamp(cell.y / cell.x, 0.0, 1.0);
            float dust = clamp(cell.z / cell.x, 0.0, 1.0);
            float vapour = clamp(cell.w / cell.x, 0.0, 1.0);
            vec3 albedo = mix(mix(vec3(0.24, 0.23, 0.225), vec3(0.42, 0.35, 0.27), dust), vec3(0.85, 0.88, 0.92), vapour);
            albedo *= 0.85 + 0.3 * cellHash(c + 17.0);
            vec3 moon = normalize(sunDir.xyz);
            // The moon through the smoke toward it: four taps, as soft as the gas's.
            float depthToMoon = 0.0;
            float s = smokeBox.w * 0.8;
            vec3 q = centre;
            for (int k = 0; k < 4; k++) {
                q += moon * s;
                depthToMoon += gridAt(q).x * s;
                s *= 1.8;
            }
            float moonVisible = exp(-depthToMoon * smokeParams.y * 0.7);
            vec3 lit = grrSunColor() * (max(dot(normal, moon), 0.0) * moonVisible)
                     + grrAmbient(normal) * 1.5
                     + grrFlashLight(centre, normal) * 0.6;
            vec3 colour = albedo * lit / GRR_PI * 3.0 + fireColour(heat) * (smokeParams.w / max(smokeParams.y, 1e-4));
            return vec4(colour, 0.0);
        }
        // On to the next lattice cell.
        if (tNext.x < tNext.y && tNext.x < tNext.z) {
            t = tNext.x; tNext.x += tDelta.x; c.x += stepDir.x; normal = vec3(-stepDir.x, 0.0, 0.0);
        } else if (tNext.y < tNext.z) {
            t = tNext.y; tNext.y += tDelta.y; c.y += stepDir.y; normal = vec3(0.0, -stepDir.y, 0.0);
        } else {
            t = tNext.z; tNext.z += tDelta.z; c.z += stepDir.z; normal = vec3(0.0, 0.0, -stepDir.z);
        }
    }
    return vec4(0.0, 0.0, 0.0, 1.0);
}

void main() {
    if (smokeParams.z < 0.5) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 dir = grrCameraRay(v_texcoord0, passTargetRes.xy, cameraDir.xyz);
    vec4 pos = texture2D(gPos, v_texcoord0);
    float surfaceT = pos.w < 0.0 ? 1.0e9 : pos.w;

    float N = smokeGridInfo.x;
    vec3 boxMin = smokeBox.xyz, boxMax = smokeBox.xyz + vec3(N, N, N) * smokeBox.w;
    vec3 safeDir = vec3(abs(dir.x) < 1e-6 ? 1e-6 : dir.x, abs(dir.y) < 1e-6 ? 1e-6 : dir.y, abs(dir.z) < 1e-6 ? 1e-6 : dir.z);
    vec3 t0 = (boxMin - cameraPos.xyz) / safeDir, t1 = (boxMax - cameraPos.xyz) / safeDir;
    vec3 tNear = min(t0, t1), tFar = max(t0, t1);
    float tEnter = max(max(tNear.x, tNear.y), max(tNear.z, 0.0));
    float tExit = min(min(min(tFar.x, tFar.y), tFar.z), surfaceT);
    if (tExit <= tEnter) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Debug views (EXPLOSION_SMOKE_DEBUG): 8 the grid's box, 9 the raw density summed along the ray.
    if (smokeParams.z > 7.5 && smokeParams.z < 8.5) {
        gl_FragColor = vec4(0.4, 0.0, 0.0, 0.5);
        return;
    }
    if (smokeParams.z > 8.5) {
        float sum = 0.0;
        for (int i = 0; i < 64; i++) sum += gridAt(cameraPos.xyz + dir * mix(tEnter, tExit, (float(i) + 0.5) / 64.0)).x;
        sum *= (tExit - tEnter) / 64.0;
        gl_FragColor = vec4(sum * 0.05, sum * 0.01, 0.0, 0.0);
        return;
    }
    if (smokeParams.z < 1.5) {
        gl_FragColor = drawBlocks(dir, tEnter, tExit);
        return;
    }
    float steps = clamp(smokeShape.w, 8.0, float(MAX_STEPS));
    float stepLength = max((tExit - tEnter) / steps, smokeBox.w * 0.3);
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float t = tEnter + stepLength * jitter;

    vec3 moon = normalize(sunDir.xyz);
    vec3 moonLight = grrSunColor();
    float cosTheta = dot(dir, moon);
    float phase = mix(henyeyGreenstein(cosTheta, 0.45), henyeyGreenstein(cosTheta, -0.2), 0.35) * 4.0 * GRR_PI;
    vec3 skyLight = grrSky(vec3(0.0, 1.0, 0.0)) * 1.5;
    float extinction = smokeParams.y;
    vec3 noiseDrift = vec3(0.0, -smokeParams.x * smokeShape.z, 0.0);

    vec3 colour = vec3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    for (int i = 0; i < MAX_STEPS; i++) {
        if (t >= tExit || transmittance < 0.01) break;
        vec3 p = cameraPos.xyz + dir * t;
        vec4 cell = gridAt(p);
        float ds = stepLength;
        if (cell.x > 1e-4) {
            float heat = clamp(cell.y / cell.x, 0.0, 1.0);
            float dust = clamp(cell.z / cell.x, 0.0, 1.0);
            float vapour = clamp(cell.w / cell.x, 0.0, 1.0);
            // Erosion: the noise eats the thin edges and leaves the thick core, so the cloud's surface
            // is cauliflower rather than a smooth blob. Hot gas is eroded less: the fireball is solid.
            float n = fbm(p * smokeShape.y + noiseDrift);
            float erode = smokeShape.x * (1.0 - 0.6 * heat);
            float density = max(cell.x * (1.0 + erode * (n * 2.0 - 1.0)) - 0.015, 0.0);
            float sigma = density * extinction;
            if (sigma > 1e-5) {
                // The moon through the gas toward it.
                float depthToMoon = 0.0;
                float s = smokeBox.w * 0.7;
                vec3 q = p;
                for (int k = 0; k < MOON_TAPS; k++) {
                    q += moon * s;
                    depthToMoon += gridAt(q).x * s;
                    s *= 1.7;
                }
                float moonVisible = exp(-depthToMoon * extinction * 0.7);
                // The fire's light from below, dimmed by the gas it crosses (roughly: by the density here).
                vec3 toFire = flashParams.xyz - p;
                float fireFalloff = flashParams.w / (dot(toFire, toFire) + flashColor.w * flashColor.w);
                vec3 fireLight = flashColor.rgb * fireFalloff * exp(-density * extinction * 2.0);
                vec3 albedo = mix(mix(vec3(0.24, 0.23, 0.225), vec3(0.42, 0.35, 0.27), dust), vec3(0.85, 0.88, 0.92), vapour);
                vec3 scattered = albedo * (moonLight * (moonVisible * phase) + skyLight + fireLight * 0.6);
                // Emission per unit extinction: thick hot gas glows its full colour, thin hot gas less.
                vec3 emitted = fireColour(heat) * (smokeParams.w / max(extinction, 1e-4));
                float stepTransmittance = exp(-sigma * ds);
                colour += transmittance * (scattered + emitted) * (1.0 - stepTransmittance);
                transmittance *= stepTransmittance;
            }
        } else {
            ds *= 1.6;   // empty: stride on
        }
        t += ds;
    }
    gl_FragColor = vec4(colour, transmittance);
}
