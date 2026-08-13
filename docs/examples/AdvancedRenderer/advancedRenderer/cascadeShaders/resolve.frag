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
// Inputs (FBO 1 [0..5] then cascade 0): 0 gPos  1 gNormal  ... 6 cascade0. FBO 1 carries six
// attachments now that the G-buffer produces the per-face anchor and key, so cascade 0 -- the
// second input framebuffer -- binds at register 6 rather than 4.
// Output (FBO 6): rgb = indirect irradiance R, a = 1 if R came from a probe ON THIS PIXEL'S OWN
// FACE and 0 if it had to be interpolated from probes on other faces. accumulate treats that flag
// as authority -- see below, and see the note there.
//
// -----------------------------------------------------------------------------
// WHY A PROBE ON THIS FACE IS WORTH SO MUCH MORE THAN A GOOD INTERPOLATION
// -----------------------------------------------------------------------------
// A probe's gathered radiance is CAMERA-INDEPENDENT: its origin is the voxel face centre, its
// normal is the face normal, and its direction set is fixed. Two frames that both give a face a
// probe compute the same number for it, wherever the camera was standing.
//
// The interpolation is not. Which faces own a probe is decided by a lattice in SCREEN space --
// probe p samples whatever face sits at (p + 0.5) * s0 pixels -- so as the camera moves, the set
// of sampled faces changes, and with it the four values any unsampled face is blended from. That
// is a viewpoint-dependent answer to a viewpoint-independent question, and it is what made the
// lighting visibly change as you flew around: at distance nearly every face is interpolated from
// its neighbours, up close most faces own a probe, and walking towards a wall re-lights it.
//
// So this pass no longer treats the two as interchangeable. If any of the four candidate probes is
// on this pixel's own face, its radiance is used DIRECTLY and the sample is flagged exact. The
// bilateral blend remains only as a bootstrap for faces that have never owned a probe.
//
// (Several probes can land on one face; they gather identically and differ only in their merge
// term, so the highest-weighted one is picked and the choice does not matter much.)
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,     0);
SAMPLER2D(gNormal,  1);
SAMPLER2D(gKey,     5);
SAMPLER2D(cascade0, 6);

// Must match the cascades' face-anchored probe spacing so this pass indexes the same probe grid.
#define PROBE_SPACING0 16
// This pass writes at screen resolution and READS the quarter-size cascade atlas, so the two
// grids are the other way round from the cascade passes'.
#define CASCADE_SCREEN_RES passTargetRes.xy
#define CASCADE_ATLAS_RES  pjvResOr(passInputRes[6].xy, passTargetRes.xy)
#include <pjv_cascade_common.sc>
#include <pjv_face_key.sc>

#define GI_STRENGTH 1.0
// Luminance ceiling on R. A gather ray that catches a brightly sunlit bounce, averaged over only
// ~16 directions, can spike a probe far above its neighbours; unclamped that spike enters the
// temporal buffer as a bright pop that decays over seconds and drives the TAA neighbourhood bounds
// with it. Clamping the luminance preserves hue and tames the outlier before anything downstream
// can smear it in time. It matters more with face probes than it did without: one bad probe is now
// one whole face, not one pixel.
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
        vec2  uv   = (vec2(px) + 0.5) / CASCADE_ATLAS_RES;
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
    vec4 curKey = texture2D(gKey, pjvSnapToTexel(uv, CASCADE_SCREEN_RES));

    // Bilinear over the four nearest cascade-0 probes (probe centres at (i+0.5)*s0 in screen px),
    // but BILATERAL: each probe is weighted by how well its own surface matches this pixel's, so
    // radiance doesn't leak from a probe on a different face / at a different depth (the main
    // light-leak source at silhouettes and inner corners). Plain bilinear is the fallback when
    // every neighbour is rejected so isolated surfaces don't go black.
    int   s0 = probeSpacingOf(0);
    ivec2 probesN = ivec2(CASCADE_SCREEN_RES) / s0;
    vec2 pxCoord = floor(uv * CASCADE_SCREEN_RES);
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
    vec3  accPlain = vec3(0.0);
    vec3  exactRadiance = vec3(0.0);
    float exactWeight = -1.0;          // < 0 until a probe on this pixel's own face is found
    for (int i = 0; i < 4; i++) {
        ivec2 probe = clamp(b + off[i], ivec2(0, 0), probesN - 1);
        vec2  puv = pjvSnapToTexel((vec2(probe) + 0.5) * float(s0) / CASCADE_SCREEN_RES,
                                   CASCADE_SCREEN_RES);
        vec4  pgp = texture2D(gPos, puv);
        vec3  pN  = normalize(texture2D(gNormal, puv).xyz);
        vec3  r   = probeMeanRadiance(probe, N, jitter);

        // Is this probe standing on the very face being shaded? Then its radiance IS this face's
        // radiance and there is nothing to interpolate.
        if (pjvSameFace(texture2D(gKey, puv), curKey) && bw[i] > exactWeight) {
            exactWeight = bw[i];
            exactRadiance = r;
        }

        float w   = bw[i] * geomWeight(P, N, pgp.xyz, pN, pgp.a);
        acc      += r * w;
        accW     += w;
        accPlain += r * bw[i];
    }

    bool exact = exactWeight >= 0.0;
    vec3 indirect = exact ? exactRadiance
                          : (accW > 1e-4 ? acc / accW : accPlain);

    float lum = dot(indirect, vec3(0.299, 0.587, 0.114));
    if (lum > FIREFLY_MAX) indirect *= FIREFLY_MAX / lum;

    gl_FragData[0] = vec4(indirect * GI_STRENGTH, exact ? 1.0 : 0.0);
}
