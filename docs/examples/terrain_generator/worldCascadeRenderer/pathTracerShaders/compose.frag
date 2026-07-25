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

SAMPLER2D(accumIndirect, 0);   // FBO7[0]: temporally-accumulated indirect irradiance R
SAMPLER2D(gPos,          4);   // FBO1[0]: xyz world pos, a = camDist (a < 0 => sky)
SAMPLER2D(gNormal,       5);   // FBO1[1]
SAMPLER2D(gAlbedo,       6);   // FBO1[2]
SAMPLER2D(gDirect,       7);   // FBO1[3]: direct sun (or sky colour for background)
SAMPLER2D(cascade0,      8);   // FBO5[0]: cascade-0 directional radiance atlas (for reflections)

#include <pjv_cascade_common.sc>

// ---- Rough reflection knobs -------------------------------------------------
#define SPEC_SHININESS 1.0   // glossy-lobe exponent. LOW = rough/wide (what the ~16 dirs support);
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

// Light bilateral radius. 1 = 3x3. Deliberately small -- temporal accumulation is the primary
// denoiser; this just removes leftover speckle without softening the GI.
#define FILTER_RADIUS 1
// Age-guided a-trous spread. accumIndirect.a carries the temporal age (from accumulate). Where
// history is THIN (freshly disoccluded / newly seen: age near 1) temporal denoising hasn't kicked
// in yet, so those pixels would show the raw undersampled GI (~16 gather rays/probe) -- the ugly
// disocclusion noise. We stand in for the missing temporal filter by SPREADING the same 3x3 bilateral
// taps wider (a-trous stride) for thin-history pixels, then shrinking the stride back to 1 as the
// pixel converges. Tap COUNT is unchanged, so converged pixels pay nothing extra.
#define FILTER_MAX_STRIDE 4.0   // widest tap spacing (px) for a brand-new pixel
#define FILTER_AGE_FULL   16.0  // age at/above which the filter is back to a tight 3x3

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

        // Light depth/normal-guided bilateral blur of the indirect irradiance. The tap SPACING
        // widens for thin-history pixels (age -> the a-trous stride) so freshly disoccluded regions
        // get a real spatial denoise instead of showing raw noise, and tightens back to 1px once the
        // temporal mean has converged. age lives in accumIndirect.a at this pixel.
        float age = texture2D(accumIndirect, uv).a;
        float t   = clamp(1.0 - (age - 1.0) / (FILTER_AGE_FULL - 1.0), 0.0, 1.0);
        float stride = mix(1.0, FILTER_MAX_STRIDE, t);

        vec2 texel = 1.0 / windowRes.xy;
        vec3  acc = vec3(0.0);
        float accW = 0.0;
        for (int dy = -FILTER_RADIUS; dy <= FILTER_RADIUS; dy++)
        for (int dx = -FILTER_RADIUS; dx <= FILTER_RADIUS; dx++) {
            vec2 sUV = uv + vec2(float(dx), float(dy)) * texel * stride;
            vec4 sgp = texture2D(gPos, sUV);
            vec3 sN  = normalize(texture2D(gNormal, sUV).xyz);
            float w  = geomWeight(P, N, sgp.xyz, sN, sgp.a);
            acc  += texture2D(accumIndirect, sUV).rgb * w;
            accW += w;
        }
        vec3 R = accW > 1e-4 ? acc / accW : texture2D(accumIndirect, uv).rgb;

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

        // Height + distance fog: fades the shaded surface toward the real sky colour in the
        // exact direction the camera is looking at it (see applyFog in pjv_sun_sky.sc), so
        // distant terrain melts into the horizon instead of just getting darker.
        vec3  toP  = P - cameraPos.xyz;
        float dist = length(toP);
        hdr = applyFog(hdr, cameraPos.xyz, toP / max(dist, 1e-4), dist);
    }

    gl_FragData[0] = vec4(hdr, 1.0);
}
