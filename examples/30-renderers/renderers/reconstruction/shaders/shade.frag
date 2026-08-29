$input v_color0
$input v_texcoord0

// =============================================================================
// shade.frag  --  Pass 2. Softens the shadow, composes the render-resolution HDR frame.
//
// The G-buffer traced a HARD shadow: one ray, visibility 0 or 1. Left that way, a binary term
// magnified from half resolution gives stair-stepped shadow edges that no amount of reconstruction
// can fix, because the aliasing is in the signal rather than in the sampling.
//
// So softness is applied here, spatially, exactly as AdvancedRenderer's compose.frag does it. An
// 8-tap ring averages the VISIBILITY of neighbouring pixels -- a signal with no noise in it, so a
// small fixed kernel is enough and there is nothing to converge. The alternative, many shadow rays
// per pixel, buys the same penumbra for many times the cost.
//
// The ring is depth-gated: a tap on a surface at a different distance belongs to different geometry
// and must not lend its visibility to this one, or shadows bleed across silhouettes.
//
// Input  (FBO 1): 0 gPos (a = camDist), 1 gNormal, 2 gAmbient, 3 gDirect (w = hard visibility)
// Output (FBO 2): composedHDR, linear -- display.frag tone maps after the reconstruction.
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,     0);
SAMPLER2D(gNormal,  1);
SAMPLER2D(gAmbient, 2);
SAMPLER2D(gDirect,  3);

// Engine-set: (w, h, 1/w, 1/h) of this pass's target -- the RENDER resolution here.
uniform vec4 passTargetRes;

// Penumbra width, in render pixels. Wider is softer; too wide and contact shadows lose their
// footing. 2.5 is about one output pixel at half render scale.
#define SHADOW_RING_TAPS   8
#define SHADOW_RING_PIXELS 2.5

// A tap belongs to this surface only if it is at a comparable distance. Proportional to depth
// because a fixed world tolerance is far too tight up close and far too loose at range.
#define SHADOW_DEPTH_TOLERANCE 0.06

void main() {
    vec4 posSample = texture2D(gPos, v_texcoord0);
    float camDist = posSample.a;

    vec3 sunRadiance = texture2D(gDirect,  v_texcoord0).rgb;
    vec3 ambient     = texture2D(gAmbient, v_texcoord0).rgb;

    // Sky: the G-buffer already wrote the sky colour into both, and there is no shadow to soften.
    if (camDist < 0.0) {
        gl_FragColor = vec4(sunRadiance, 1.0);
        return;
    }

    // Average visibility over the ring. Divided by the WHOLE ring rather than by the taps that
    // survived the depth gate: a rejected tap means "no comparable surface there", which should
    // read as an unlit direction rather than being excluded from the average -- excluding it would
    // make an isolated voxel brighter than a flat wall in the same light.
    float visibility = texture2D(gDirect, v_texcoord0).a;
    for (int i = 0; i < SHADOW_RING_TAPS; i++) {
        float a = (float(i) + 0.5) * (6.2831853 / float(SHADOW_RING_TAPS));
        vec2 tapUV = v_texcoord0 + vec2(cos(a), sin(a)) * SHADOW_RING_PIXELS * passTargetRes.zw;

        float tapDist = texture2D(gPos, tapUV).a;
        if (tapDist < 0.0) continue;                                        // sky
        if (abs(tapDist - camDist) > camDist * SHADOW_DEPTH_TOLERANCE + 1e-3) continue;

        visibility += texture2D(gDirect, tapUV).a;
    }
    visibility /= float(SHADOW_RING_TAPS + 1);

    gl_FragColor = vec4(sunRadiance * visibility + ambient, 1.0);
}
