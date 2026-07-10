$input v_color0
$input v_texcoord0

// =============================================================================
// compose.frag  --  Recombine pass (was the back half of display).
//
// Builds the final HDR frame from the separately-denoised parts, and hands it to the TAA pass:
//   1. lightly bilateral-blur the temporally-accumulated indirect irradiance R (FBO7[0]),
//      depth/normal guided so it never bleeds across a surface/depth edge,
//   2. recombine  hdr = gDirect + gAlbedo * R   (direct sun + albedo stay crisp -- they were kept
//      out of the temporal accumulation on purpose),
//   3. add a ROUGH SPECULAR reflection gathered from cascade 0's directional radiance (see below),
//   4. output HDR (NOT tonemapped) so taa.frag can anti-alias it and display.frag tonemaps last.
//
// Rough reflection: cascade 0 stores per-direction radiance per probe; the diffuse resolve pass
// integrates it against a COSINE lobe. A glossy reflection is the same integral against a lobe
// centred on the mirror direction reflect(V,N) instead. With only ~16 octahedral directions per
// probe this can only represent WIDE/rough lobes (a sharp mirror would need far more directions),
// which is exactly "basic rough reflection". It is computed HERE, not folded into the temporally
// accumulated R, because specular is VIEW-DEPENDENT: the accumulate pass reprojects by world
// position (correct for diffuse, which is view-independent), so pushing a view-dependent highlight
// through it would smear/lag the highlight under motion. Gathering it fresh in compose keeps it
// glued to the view; the rough lobe + TAA keep the (undenoised) result acceptably stable.
//
// Inputs (FBO 7 [0..3], FBO 1 [0..3], FBO 5 [0]):
//   0 accumIndirect   4 gPos  5 gNormal  6 gAlbedo  7 gDirect   8 cascade0
// Output (FBO 8): 0 hdr composite.
// =============================================================================

#include <bgfx_shader.sh>

// Renderer 6 inputs [FBO7, FBO1, FBO5]. FBO7 has 2 targets (accumR, histKey) and FBO1 has 6,
// so gPos..gDirect land at 2..5 and cascade0 (FBO5) at 8.
SAMPLER2D(accumIndirect, 0);   // FBO7[0]: temporally-accumulated indirect irradiance R (a = age)
SAMPLER2D(gPos,          2);   // FBO1[0]: xyz world pos, a = camDist (a < 0 => sky)
SAMPLER2D(gNormal,       3);   // FBO1[1]
SAMPLER2D(gAlbedo,       4);   // FBO1[2]: rgb albedo (a = voxel size, unused here)
SAMPLER2D(gDirect,       5);   // FBO1[3]: direct sun (or sky colour for background)
SAMPLER2D(gKey,          7);   // FBO1[5]: rgb voxel index, a = face id (exact per-face key)
SAMPLER2D(cascade0,      8);   // FBO5[0]: cascade-0 directional radiance atlas (for reflections)

// Must match the cascades' coarser probe spacing so the reflection gather indexes the same grid.
#define PROBE_SPACING0 16
#include <pjv_cascade_common.sc>

// ---- Rough reflection knobs -------------------------------------------------
#define SPEC_SHININESS 0.0   // glossy-lobe exponent. LOW = rough/wide (what the ~16 dirs support);
                              // raising it narrows the lobe but starts to alias into the coarse
                              // direction grid (only ~8 dirs land in the upper hemisphere).
#define SPEC_STRENGTH  0.0    // overall reflection intensity multiplier (taste).
#define SPEC_F0        0.04   // dielectric normal-incidence reflectance for the Fresnel term.

// Lobe-weighted mean radiance of one cascade-0 probe around the reflection direction (its own
// D0 x D0 direction tile). Same structure as resolve's cosine-weighted probeMeanRadiance, but the
// per-direction weight is a Phong glossy lobe centred on refl instead of the cosine term.
vec3 probeSpecRadiance(ivec2 probe, vec3 refl, vec2 jitter) {
    int D0 = dirTileOf(0);
    vec3  accRad = vec3(0.0);
    float accW   = 0.0;
    for (int dy = 0; dy < D0; dy++)
    for (int dx = 0; dx < D0; dx++) {
        ivec2 tile = ivec2(dx, dy);
        vec3  dir  = dirForTile(tile, D0, jitter);
        float w    = pow(max(dot(dir, refl), 0.0), SPEC_SHININESS);
        ivec2 px   = probe * D0 + tile;
        vec2  uv   = (vec2(px) + 0.5) / windowRes.xy;
        accRad += texture2D(cascade0, uv).rgb * w;
        accW   += w;
    }
    return accRad / max(accW, 1e-3);
}

// Rough reflection radiance at surface (P,N) seen from the camera: bilateral-bilinear over the four
// nearest cascade-0 probes (same probe layout / geometry weighting as the diffuse resolve), each
// contributing its reflection-lobe-weighted radiance. Fresnel is applied by the caller.
vec3 specularReflection(vec2 uv, vec3 P, vec3 N, vec3 refl) {
    int   s0 = probeSpacingOf(0);
    ivec2 probesN = ivec2(windowRes.xy) / s0;
    vec2  pxCoord = floor(uv * windowRes.xy);
    vec2  gpc = pxCoord / float(s0) - 0.5;
    vec2  f   = fract(gpc);
    ivec2 b   = ivec2(floor(gpc));
    vec2  jitter = cascadeJitter();

    ivec2 off[4];
    off[0] = ivec2(0, 0); off[1] = ivec2(1, 0);
    off[2] = ivec2(0, 1); off[3] = ivec2(1, 1);
    float bw[4];
    bw[0] = (1.0 - f.x) * (1.0 - f.y); bw[1] = f.x * (1.0 - f.y);
    bw[2] = (1.0 - f.x) * f.y;         bw[3] = f.x * f.y;

    vec3  acc = vec3(0.0);
    float accW = 0.0;
    vec3  accPlain = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        ivec2 probe = clamp(b + off[i], ivec2(0, 0), probesN - 1);
        vec2  puv = (vec2(probe) + 0.5) * float(s0) / windowRes.xy;
        vec4  pgp = texture2D(gPos, puv);
        vec3  pN  = normalize(texture2D(gNormal, puv).xyz);
        vec3  r   = probeSpecRadiance(probe, refl, jitter);
        float w   = bw[i] * geomWeight(P, N, pgp.xyz, pN, pgp.a);
        acc      += r * w;
        accW     += w;
        accPlain += r * bw[i];
    }
    return accW > 1e-4 ? acc / accW : accPlain;
}

// ---- Per-face FLATTEN of the indirect irradiance (the key motion-stability fix) ----------------
// The GI is gathered on a sparse SCREEN-anchored probe grid, so a pixel's raw R depends on where the
// camera happens to put that grid over the geometry: as the camera moves the grid slides, the set of
// probes a pixel blends changes, and grazing-ray + direction-jitter noise ride on top -> R shimmers
// frame to frame even on a flat wall (the "flicker under movement"). But a voxel FACE's true
// irradiance is one value (it is view-independent, and low-frequency across the face). So we AVERAGE
// R over same-face pixels: an a-trous box keyed on the EXACT face id (voxel index + face). Every tap
// that shares this pixel's face contributes; taps on any other face are rejected (no leak across
// edges/corners). The average over many face pixels cancels the per-pixel grid-slide + gather noise,
// yielding a near-constant per-face value that barely moves with the camera -> the temporal
// accumulate/TAA then have a stable target and the flicker goes away. This is renderer 4's flat-face
// property, reconstructed from the cascade GI. Direct sun + albedo are NOT flattened (stay crisp).
#define FLATTEN_LEVELS 3        // a-trous levels; strides 1,2,4 px (was 5 -> +8,16; the two widest
                                // taps cost ~1.8ms for little static gain, dropped after profiling)
#define FLATTEN_RADIUS 1        // 3x3 taps per level

// A tap belongs to the same WALL as the shaded pixel iff it has the same face ORIENTATION (face id --
// voxel-agnostic, so it spans all voxels of the wall) AND lies on the same PLANE (rejects a parallel
// wall behind an edge / a perpendicular face at a corner). This is what lets the flatten average R
// across a whole multi-voxel wall (an exact voxel-key test would only ever average within one voxel's
// footprint -> no flattening on far / oblique walls, exactly where it is needed).
bool composeSameWall(vec2 sUV, float cFaceId, vec3 P, vec3 N, float planeTol) {
    vec4 sKey = texture2D(gKey, sUV);
    if (abs(sKey.a - cFaceId) > 0.5) return false;
    vec4 sgp = texture2D(gPos, sUV);
    if (sgp.a < 0.0) return false;                          // sky
    return abs(dot(sgp.xyz - P, N)) < planeTol;            // coplanar
}

void main() {
    vec2 uv = v_texcoord0;
    vec4 gp = texture2D(gPos, uv);
    vec3 direct = texture2D(gDirect, uv).rgb;

    vec3 hdr;
    if (gp.a < 0.0) {
        // Sky / background: no indirect term, gDirect holds the sky colour.
        hdr = direct;
    } else {
        vec3 P = gp.xyz;
        vec3 N = normalize(texture2D(gNormal, uv).xyz);
        vec3 albedo = texture2D(gAlbedo, uv).rgb;

        // Per-face flatten of the (already temporally-accumulated) indirect irradiance. Box-average R
        // over every same-face tap of a multi-stride a-trous kernel; taps on other faces are rejected
        // by the exact face-key test, so nothing leaks across an edge. The result is the face's mean
        // R -- near camera-independent -> the flicker source is removed. Own texel is always included.
        float cFaceId  = texture2D(gKey, uv).a;
        float planeTol = max(0.3, gp.a * 0.02);
        vec2 texel = 1.0 / windowRes.xy;
        vec3  acc  = texture2D(accumIndirect, uv).rgb;
        float accW = 1.0;
        for (int lvl = 0; lvl < FLATTEN_LEVELS; lvl++) {
            float stride = float(1 << lvl);
            for (int dy = -FLATTEN_RADIUS; dy <= FLATTEN_RADIUS; dy++)
            for (int dx = -FLATTEN_RADIUS; dx <= FLATTEN_RADIUS; dx++) {
                if (dx == 0 && dy == 0) continue;
                vec2 sUV = uv + vec2(float(dx), float(dy)) * texel * stride;
                if (composeSameWall(sUV, cFaceId, P, N, planeTol)) {
                    acc  += texture2D(accumIndirect, sUV).rgb;
                    accW += 1.0;
                }
            }
        }
        vec3 R = acc / accW;

        // ---- Rough specular reflection from the cascade's directional radiance ----
        // View ray (eye -> surface) and its mirror about the normal. The reflected direction is
        // always in the upper hemisphere for a front-facing surface, which is where cascade 0 holds
        // valid radiance (downward gather dirs are stored as 0), so the lobe samples meaningful data.
        vec3 toEye = normalize(cameraPos.xyz - P);
        vec3 V     = -toEye;                       // eye -> surface
        vec3 refl  = reflect(V, N);                // mirror direction
        vec3 specRad = specularReflection(uv, P, N, refl);
        // Schlick Fresnel: reflections strengthen at grazing angles.
        float fres = SPEC_F0 + (1.0 - SPEC_F0) * pow(1.0 - max(dot(N, toEye), 0.0), 5.0);
        vec3 specular = specRad * (fres * SPEC_STRENGTH);

        hdr = direct + albedo * R + specular;
    }

    gl_FragData[0] = vec4(hdr, 1.0);
}
