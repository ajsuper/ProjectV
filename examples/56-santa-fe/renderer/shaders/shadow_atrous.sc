// =============================================================================
// shadow_atrous.sc -- the sun's shadow at the window's resolution, an A-Trous step (santa-fe-reference S22c).
// shadow_atrous1/2/4/8.frag define SF_ATROUS_STEP and include this.
//
// A 5 x 5 B3-spline kernel with its taps SF_ATROUS_STEP pixels apart, held to this pixel's surface (its plane
// and normal) and kept from the sky; four steps (1, 2, 4, 8) reach 30 pixels. The penumbra shadow_prep found
// (g, pixels) gates each step: one whose taps reach past it leaves the pixel as it is, so a contact shadow
// stays sharp and a far-cast one blurs wide. sfUpscale2.y off: the hard shadow, as the reconstruction made it.
//
// Inputs: the last step's output 0 (r the visibility, g the penumbra, b the depth); the reconstruction 1-6
// (its normal + depth 6). Output: the same as the input.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>
#include <pjv_camera.sc>

SAMPLER2D(shadowIn, 0);
SAMPLER2D(rND, 6);

uniform vec4 sfUpscale2;   // y: the shadow's A-Trous filter on

vec3 sfAtrousPosition(ivec2 q, float depth) {
    vec2 uv = (vec2(q) + 0.5) / passTargetRes.xy;
    return pjvViewPosition() + pjvViewRayDirection(uv, passTargetRes.xy) * depth;
}

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(shadowIn, at, 0);
    float h = float(SF_ATROUS_STEP);
    float gate = clamp((c.g - h) / h, 0.0, 1.0);   // the taps reach 2h; past the penumbra, none
    if (sfUpscale2.y < 0.5 || c.b < 0.0 || gate <= 0.0) { gl_FragColor = c; return; }
    ivec2 size = ivec2(passTargetRes.xy);
    vec4 nd = texelFetch(rND, at, 0);
    vec3 p0 = sfAtrousPosition(at, nd.w);
    float pixelWorld = max(nd.w, 1e-3) * 2.0 * pjvViewTanHalfFov() / passTargetRes.y;
    float sum = 0.0, wsum = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            float kx = x == 0 ? 0.375 : (abs(x) == 1 ? 0.25 : 0.0625);
            float ky = y == 0 ? 0.375 : (abs(y) == 1 ? 0.25 : 0.0625);
            float w = kx * ky;
            ivec2 q = clamp(at + ivec2(x, y) * SF_ATROUS_STEP, ivec2(0, 0), size - 1);
            vec4 s = texelFetch(shadowIn, q, 0);
            if (x != 0 || y != 0) {
                if (s.b < 0.0) continue;
                vec4 ndq = texelFetch(rND, q, 0);
                float plane = abs(dot(sfAtrousPosition(q, ndq.w) - p0, nd.xyz)) / max(pixelWorld * h * 2.0, 0.02);
                float nn = max(dot(ndq.xyz, nd.xyz), 0.0);
                w *= gate * nn * nn * exp(-plane * plane);
            }
            sum += s.r * w;
            wsum += w;
        }
    gl_FragColor = vec4(sum / max(wsum, 1e-6), c.g, c.b, 0.0);
}
