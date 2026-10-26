$input v_color0
$input v_texcoord0

// exposure.frag -- a fixed 1x1 target: the auto-exposure, carried from frame to frame.
//
// The meter's 64x64 log luminances are averaged, the centre weighted over the edges (what the eye is
// pointed at counts most), and the exposure that would bring that average to EXPOSURE_KEY, taken
// EXPOSURE_STRENGTH of the way, is the target: a dark room is lifted most of the way, but still reads as
// darker than the day outside. It is held to EXPOSURE_MIN_EV..EXPOSURE_MAX_EV round 1
// and approached over time, in stops: faster towards darker (walking into the sun), slower towards
// brighter (into a room), as an eye adapts.
//
// It also keeps the depth of field's focus, for the same reason (a value carried and eased from frame to
// frame): the distance at the centre of the view -- where the crosshair is -- averaged over a 5x5 patch
// in log distance and eased towards, as a camera's autofocus racks. Sky counts as far.
//
// Output: x the exposure in use (linear multiplier), y the focus distance (world units), z the metered
// average (log2), w 1 once it holds a value.
// Inputs: 0 meter, 1 exposure (last frame, this pass's own output), 2.. the G-buffer (gPos 2).

#include <bgfx_shader.sh>

SAMPLER2D(meter, 0);
SAMPLER2D(exposure, 1);
SAMPLER2D(gPos, 2);

uniform vec4 restirPost;   // x vignette strength (0 off), y grade on, z auto-exposure on, w seconds since last frame

#define EXPOSURE_KEY      0.08   // the metered average that needs no correction: a sunlit outdoor view
                                 // (a sunlit converted level, framed by default, meters 0.064)
#define EXPOSURE_STRENGTH 0.8
#define EXPOSURE_MIN_EV  -2.0
#define EXPOSURE_MAX_EV   3.0
#define ADAPT_DARKEN      3.0    // stops per second of the remaining gap, roughly
#define ADAPT_BRIGHTEN    1.2
#define FOCUS_RATE        6.0    // per second, in log distance
#define FOCUS_FAR         1.0e4  // what the sky counts as

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

    ivec2 g = textureSize(gPos, 0);
    float focusLog = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            ivec2 at = clamp(g / 2 + ivec2(x, y) * max(g.y / 100, 1), ivec2(0, 0), g - ivec2(1, 1));
            float t = texelFetch(gPos, at, 0).w;
            focusLog += log2(t < 0.0 ? FOCUS_FAR : max(t, 0.1));
        }
    focusLog /= 25.0;

    vec4 last = texelFetch(exposure, ivec2(0, 0), 0);
    float ev = target;
    float focus = focusLog;
    if (last.w > 0.5) {
        float dt = clamp(restirPost.w, 0.0, 0.25);
        float was = log2(max(last.x, 1.0e-6));
        float rate = target < was ? ADAPT_DARKEN : ADAPT_BRIGHTEN;
        ev = mix(was, target, 1.0 - exp(-rate * dt));
        focus = mix(log2(max(last.y, 0.1)), focusLog, 1.0 - exp(-FOCUS_RATE * dt));
    }
    gl_FragColor = vec4(exp2(ev), exp2(focus), average, 1.0);
}
