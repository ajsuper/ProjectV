$input v_color0, v_texcoord0

// rmlui_gradient.frag -- linear, radial and conic gradients, repeating or not (RmlUi's GL3
// gradient). u_rmluiGradient: x the function (below), y the number of stops. u_rmluiGradientPV: p
// (linear: the start; radial, conic: the centre) and v (linear: start to end; radial: the inverse
// radius; conic: the angle's unit vector). Stops premultiplied, at most sixteen.

#include <bgfx_shader.sh>

#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define PI 3.14159265

uniform vec4 u_rmluiGradient;
uniform vec4 u_rmluiGradientPV;
uniform vec4 u_rmluiStopColours[16];
uniform vec4 u_rmluiStopPositions[4];

float stopPosition(int i) {
    vec4 four = u_rmluiStopPositions[i / 4];
    int k = i - (i / 4) * 4;
    return k == 0 ? four.x : (k == 1 ? four.y : (k == 2 ? four.z : four.w));
}

void main() {
    int func = int(u_rmluiGradient.x + 0.5);
    int stops = int(u_rmluiGradient.y + 0.5);
    vec2 p = u_rmluiGradientPV.xy;
    vec2 v = u_rmluiGradientPV.zw;
    float t = 0.0;
    if (func == LINEAR || func == REPEATING_LINEAR) {
        t = dot(v, v_texcoord0 - p) / dot(v, v);
    } else if (func == RADIAL || func == REPEATING_RADIAL) {
        t = length(v * (v_texcoord0 - p));
    } else {
        vec2 d = v_texcoord0 - p;
        vec2 r = vec2(v.x * d.x + v.y * d.y, -v.y * d.x + v.x * d.y);
        t = 0.5 + atan2(-r.x, r.y) / (2.0 * PI);
    }
    if (func >= REPEATING_LINEAR) {
        float t0 = stopPosition(0);
        float t1 = stopPosition(stops - 1);
        t = t0 + mod(t - t0, t1 - t0);
    }
    vec4 colour = u_rmluiStopColours[0];
    for (int i = 1; i < 16; i++) {
        if (i >= stops) break;
        colour = mix(colour, u_rmluiStopColours[i], smoothstep(stopPosition(i - 1), stopPosition(i), t));
    }
    gl_FragColor = v_color0 * colour;
}
