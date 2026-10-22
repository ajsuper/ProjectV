$input v_color0
$input v_texcoord0

// exposure.frag -- pass 16, a fixed 1x1 target: the auto-exposure, carried from frame to frame.
//
// The meter's 64x64 log luminances are averaged, the centre weighted over the edges (what the eye is
// pointed at counts most), and the exposure that would bring that average to EXPOSURE_KEY, taken
// EXPOSURE_STRENGTH of the way, is the target: a dark room is lifted most of the way, but still reads as
// darker than the day outside. It is held to EXPOSURE_MIN_EV..EXPOSURE_MAX_EV round 1
// and approached over time, in stops: faster towards darker (walking into the sun), slower towards
// brighter (into a room), as an eye adapts.
//
// Output: x the exposure in use (linear multiplier), y the target, z the metered average (log2),
// w 1 once it holds a value. Inputs: 0 meter, 1 exposure (last frame, this pass's own output).

#include <bgfx_shader.sh>

SAMPLER2D(meter, 0);
SAMPLER2D(exposure, 1);

uniform vec4 restirPost;   // x vignette strength (0 off), y grade on, z auto-exposure on, w seconds since last frame

#define EXPOSURE_KEY      0.08   // the metered average that needs no correction: a sunlit outdoor view
                                 // (Lee Chemicals from its default framing meters 0.064)
#define EXPOSURE_STRENGTH 0.8
#define EXPOSURE_MIN_EV  -2.0
#define EXPOSURE_MAX_EV   3.0
#define ADAPT_DARKEN      3.0    // stops per second of the remaining gap, roughly
#define ADAPT_BRIGHTEN    1.2

void main() {
    float sum = 0.0;
    float weights = 0.0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            vec2 q = (vec2(float(x), float(y)) + 0.5) / 64.0 - 0.5;
            float w = exp(-dot(q, q) * 6.0);   // the corners count about a fifth of the centre
            sum += texelFetch(meter, ivec2(x, y), 0).r * w;
            weights += w;
        }
    float average = sum / weights;
    float target = clamp(EXPOSURE_STRENGTH * (log2(EXPOSURE_KEY) - average), EXPOSURE_MIN_EV, EXPOSURE_MAX_EV);

    vec4 last = texelFetch(exposure, ivec2(0, 0), 0);
    float ev = target;
    if (last.w > 0.5) {
        float was = log2(max(last.x, 1.0e-6));
        float rate = target < was ? ADAPT_DARKEN : ADAPT_BRIGHTEN;
        ev = mix(was, target, 1.0 - exp(-rate * clamp(restirPost.w, 0.0, 0.25)));
    }
    gl_FragColor = vec4(exp2(ev), exp2(target), average, 1.0);
}
