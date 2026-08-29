$input v_color0
$input v_texcoord0

// =============================================================================
// combine_blur.frag  --  Pass 4 of the fast renderer.
//
// Edge-aware (bilateral) blur of the gathered sky-light, then composite. By now the
// sky term has already been accumulated TEMPORALLY (sky_reproject.frag, pass 3), so
// a parked view is essentially noise-free here; this pass takes out the residual
// spatial noise that survives on freshly disoccluded pixels (age ~1) and while
// moving, with a small à-trous/bilateral kernel whose weights are stopped on the
// G-buffer's PLANE distance and NORMAL so light is averaged only across genuinely
// coplanar neighbours -- creases and silhouettes stay crisp while flat areas go
// smooth. (The final TAA pass then resolves the sub-pixel jitter into AA edges.)
//
// The gathered value is the cosine-weighted MEAN incoming radiance Li (occluded
// sky + one bounce). The diffuse response of a surface is albedo * mean(Li), so we
// multiply the blurred sky-light by the surface albedo (carried in gAmbient) and
// add the untouched direct sun. This replaces the old "flat ambient * AO scalar":
// the ambient is now real, occluded, coloured and directional.
//
// Inputs (flattened FBO-input order):
//   0 gDirect  1 gPos  2 gNormal  3 gAmbient(albedo)   (FBO 1)
//   4 skyAccum  5 skyPos  6 skyNormal                  (FBO 5, only skyAccum used)
// Outputs (FBO 3):
//   0 curColor  rgb = direct + albedo*skyLight, a = hit mask
//   1 curPos    carried forward for TAA
//   2 curNormal carried forward for TAA
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gDirect,  0);
SAMPLER2D(gPos,     1);
SAMPLER2D(gNormal,  2);
SAMPLER2D(gAmbient, 3);
// Register 4 = FBO 5's first texture: the temporally-accumulated sky term
// (sky_reproject.frag output). Same slot the raw sky occupied before the temporal
// pass was inserted; the bilateral blur below is unchanged, it just now smooths an
// already-denoised signal instead of raw few-ray noise.
SAMPLER2D(aoRaw,    4);

uniform vec4 windowRes;

#define BLUR_RADIUS 2      // 5x5 taps.
#define SIGMA_N     16.0   // Normal edge-stop sharpness.
#define SIGMA_P     0.35   // Plane-distance tolerance (voxels). Well under 1 so a
                           // voxel one step in front/behind is rejected, not blurred.

// This used to skip the blur once a pixel's TEMPORAL history had converged: the removed
// sky_reproject pass accumulated AO over time and stored its age in the alpha channel, and a parked
// frame could skip the 25-tap kernel almost everywhere.
//
// The screen-space AO feeding this pass has no history and therefore no age, so there is nothing to
// test and the blur now runs every frame. That is the honest cost of dropping the temporal pass, and
// it is affordable here because SSAO is cheap enough that the blur is no longer the cheap part of an
// expensive renderer. Left as a named constant, unreachable, to say where the shortcut went.
#define CONVERGED_AGE 1e30

void main() {
    vec2 uv = v_texcoord0;

    vec4  direct4 = texture2D(gDirect,  uv);
    float hitMask = direct4.a;
    vec4  pos4    = texture2D(gPos,     uv);
    vec3  N0      = texture2D(gNormal,  uv).xyz;
    vec3  albedo  = texture2D(gAmbient, uv).rgb;

    // Sky / background: no AO, pass the sky colour straight through.
    if (hitMask < 0.5) {
        gl_FragData[0] = vec4(direct4.rgb, 0.0);
        return;
    }

    vec3  P0     = pos4.xyz;
    vec2  texel  = 1.0 / windowRes.xy;

    vec4  sky0   = texture2D(aoRaw, uv);   // rgb = accumulated AO, a = temporal age

    vec3 skyLight;
    if (sky0.a >= CONVERGED_AGE) {
        // Converged: the temporal accumulator has already driven this pixel noise-free.
        // Use it directly and skip the whole bilateral kernel.
        skyLight = sky0.rgb;
    } else {
        // Fresh / moving: still noisy, so spatially denoise with the bilateral kernel.
        vec3  skySum = sky0.rgb * 1.0;   // seed with centre (its weight is 1)
        float wSum   = 1.0;
        for (int y = -BLUR_RADIUS; y <= BLUR_RADIUS; y++) {
            for (int x = -BLUR_RADIUS; x <= BLUR_RADIUS; x++) {
                if (x == 0 && y == 0) continue;               // centre already seeded
                vec2 o  = vec2(float(x), float(y)) * texel;
                vec2 su = uv + o;

                vec3 Ni = texture2D(gNormal, su).xyz;
                if (dot(Ni, Ni) < 0.01) continue;             // sky neighbour

                vec3 Pi = texture2D(gPos, su).xyz;

                // Edge stops: same facing (normal) and same surface PLANE. The plane
                // distance |dot(Pi-P0, N0)| is ~0 for coplanar neighbours (so a flat
                // face blurs freely, even at grazing angles), but ~1 voxel for the
                // surface one depth-step in front/behind -- which is then rejected
                // instead of bleeding light across the voxel edge.
                float wn = pow(max(0.0, dot(N0, Ni)), SIGMA_N);
                float planeDist = abs(dot(Pi - P0, N0));
                float wp = exp(-(planeDist * planeDist) / (2.0 * SIGMA_P * SIGMA_P));
                float w  = wn * wp;

                skySum += texture2D(aoRaw, su).rgb * w;
                wSum   += w;
            }
        }
        skyLight = skySum / wSum;
    }

    // Diffuse response = albedo * mean(Li). Direct sun is added untouched.
    vec3 finalColor = direct4.rgb + albedo * skyLight;

    gl_FragData[0] = vec4(finalColor, 1.0);
}
