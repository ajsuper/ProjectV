$input v_color0
$input v_texcoord0

// =============================================================================
// resolve.frag  --  Pass 6 of the radiance-cascade renderer.
//
// Turns cascade 0 (the finest-spatial, fully-merged radiance atlas) into a per-pixel
// indirect irradiance value R. It no longer composites the frame -- direct sun and the albedo
// modulation are applied in display, AFTER R has been temporally accumulated (pass 7) and lightly
// filtered, so the smooth GI is denoised on its own while direct/edges/albedo detail stay crisp.
//
// Indirect at a pixel = the cosine-weighted mean of cascade 0's directional radiance,
// bilinearly interpolated over the four nearest cascade-0 probes. Because cascade 0's
// directions are merged all the way up (its transparent rays already carry the sky and
// the coarser cascades' bounced light), this single integration yields full multi-scale
// GI. Cosine-weighted mean radiance R satisfies E = pi*R, and diffuse Lo = albedo/pi * E
// = albedo * R, so the diffuse response is simply albedo * R (GI_STRENGTH lets it be
// dialed for taste).
//
// Inputs (FBO 1 [0..3] then cascade 0):
//   0 gPos  1 gNormal  2 gAlbedo  3 gDirect   4 cascade0
// Output (FBO 6): 0 composite (HDR linear).
// =============================================================================

#include <bgfx_shader.sh>

// FBO1 carries 6 targets in renderer 6 (adds gFace, gKey), so cascade0 (FBO5) binds at
// register 6, after FBO1's 0..5. gAlbedo/gDirect are not used by resolve and are left undeclared.
SAMPLER2D(gPos,     0);
SAMPLER2D(gNormal,  1);
SAMPLER2D(cascade0, 6);

// Must match the cascades' coarser probe spacing so this pass indexes the same probe grid.
#define PROBE_SPACING0 16
#include <pjv_cascade_common.sc>

#define GI_STRENGTH 1.0
// Max luminance for the indirect irradiance R (firefly cap). Generous -- only clips true outliers.
#define FIREFLY_MAX 3.0

// Cosine-weighted mean radiance over one cascade-0 probe's directions (D0 x D0 tile). The probe's
// directions live at pixels probe*D0 + tile inside cascade 0's sub-rectangle.
vec3 probeMeanRadiance(ivec2 probe, vec3 N, vec2 jitter) {
    int D0 = dirTileOf(0);
    vec3  accRad = vec3(0.0);
    float accW   = 0.0;
    for (int dy = 0; dy < D0; dy++)
    for (int dx = 0; dx < D0; dx++) {
        ivec2 tile = ivec2(dx, dy);
        vec3  dir  = dirForTile(tile, D0, jitter);
        float w    = max(dot(dir, N), 0.0);
        ivec2 px   = probe * D0 + tile;
        vec2  uv   = (vec2(px) + 0.5) / windowRes.xy;
        accRad += texture2D(cascade0, uv).rgb * w;
        accW   += w;
    }
    return accRad / max(accW, 1e-3);
}

void main() {
    vec2 uv = v_texcoord0;
    vec4 gp = texture2D(gPos, uv);

    // This pass now outputs ONLY the indirect irradiance (mean radiance R). Direct sun and the
    // albedo modulation are recombined later in display, so the smooth GI can be temporally
    // accumulated + lightly filtered on its own while direct/edges/albedo detail stay crisp.
    // Sky / background: no indirect; display shows gDirect (the sky colour) for these pixels.
    if (gp.a < 0.0) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    vec3 P = gp.xyz;
    vec3 N = normalize(texture2D(gNormal, uv).xyz);

    // Bilinear over the four nearest cascade-0 probes (probe centres at (i+0.5)*s0 in screen px),
    // but BILATERAL: each probe is weighted by how well its own surface matches this pixel's, so
    // radiance doesn't leak from a probe on a different face / at a different depth (the main
    // light-leak source at silhouettes and inner corners). Plain bilinear is the fallback when
    // every neighbour is rejected so isolated surfaces don't go black.
    int   s0 = probeSpacingOf(0);
    ivec2 probesN = ivec2(windowRes.xy) / s0;
    vec2 pxCoord = floor(uv * windowRes.xy);
    vec2 gpc = pxCoord / float(s0) - 0.5;
    vec2 f   = fract(gpc);
    ivec2 b  = ivec2(floor(gpc));
    vec2 jitter = cascadeJitter();

    ivec2 off[4];
    off[0] = ivec2(0, 0); off[1] = ivec2(1, 0);
    off[2] = ivec2(0, 1); off[3] = ivec2(1, 1);
    float bw[4];
    bw[0] = (1.0 - f.x) * (1.0 - f.y); bw[1] = f.x * (1.0 - f.y);
    bw[2] = (1.0 - f.x) * f.y;         bw[3] = f.x * f.y;

    vec3  acc = vec3(0.0);
    float accW = 0.0;
    vec3  accValid  = vec3(0.0);   // plain-bilinear fallback over VALID (non-sky) probes only
    float accValidW = 0.0;
    for (int i = 0; i < 4; i++) {
        ivec2 probe = clamp(b + off[i], ivec2(0, 0), probesN - 1);
        vec2  puv = (vec2(probe) + 0.5) * float(s0) / windowRes.xy;
        vec4  pgp = texture2D(gPos, puv);
        vec3  pN  = normalize(texture2D(gNormal, puv).xyz);
        vec3  r   = probeMeanRadiance(probe, N, jitter);
        float w   = bw[i] * geomWeight(P, N, pgp.xyz, pN, pgp.a);
        acc      += r * w;
        accW     += w;
        // A probe anchored on the sky/background (gPos.a < 0) gathered nothing -> its radiance is 0.
        // It must NEVER enter the fallback average, or its blackness leaks onto foreground surfaces at
        // silhouettes (the pulsing black patches). Only valid probes contribute to the fallback.
        float valid = pgp.a >= 0.0 ? 1.0 : 0.0;
        accValid  += r * bw[i] * valid;
        accValidW += bw[i] * valid;
    }
    // Prefer the geometry-weighted blend; if every neighbour was rejected fall back to the plain
    // average of VALID probes (never the sky zeros); if even those are all sky, contribute nothing.
    vec3 indirect = accW > 1e-4 ? acc / accW
                  : (accValidW > 1e-4 ? accValid / accValidW : vec3(0.0));

    // Firefly clamp: a gather ray that catches a brightly sunlit bounce, averaged over only ~16
    // directions per probe, can spike R far above its neighbours. Left unclamped that spike enters
    // the temporal buffers as a bright pop that recovers slowly (and drives the TAA neighbourhood
    // bounds). Soft-clamp the LUMINANCE (preserving hue) so genuine bright GI is kept but outliers
    // are tamed at the source, before any temporal accumulation sees them.
    float lum = dot(indirect, vec3(0.299, 0.587, 0.114));
    if (lum > FIREFLY_MAX) indirect *= FIREFLY_MAX / lum;

    gl_FragData[0] = vec4(indirect * GI_STRENGTH, 1.0);
}
