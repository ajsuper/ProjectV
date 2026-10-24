$input v_color0
$input v_texcoord0

// dof.frag -- window, after the exposure: a very slight depth of field round the autofocus (exposure.y).
//
// A thin lens's blur, normalised: a point at distance d, focused at f, blurs to a circle of radius
// restirDof.x * |d - f| / d window pixels -- none at the focus, restirDof.x for the far distance, held
// there for the near. Kept to a couple of pixels, it softens what is off the subject without reading
// as an effect.
//
// A gather over a golden-angle disc of this pixel's radius, where a sample counts only as far as its
// own circle reaches this pixel (scatter-as-gather): a sharp subject does not smear into the blurred
// background round it, and the background does not bleed onto it.
//
// Inputs: 0 resolved, 1-7 the G-buffer (gPos 1, render resolution), 8 exposure (y: the focus).
// Output: dofColor. The views (G) other than the image pass through.

#include <bgfx_shader.sh>

SAMPLER2D(resolved, 0);
SAMPLER2D(gPos, 1);
SAMPLER2D(exposure, 8);   // after the G-buffer's seven (gMotion the last)

uniform vec4 restirDof;       // x the far distance's blur radius, window pixels (0: off)
uniform vec4 restirDenoise;   // z the view (0 the image)

#define DOF_TAPS 16

float dofDistance(vec2 uv) {
    ivec2 g = textureSize(gPos, 0);
    float t = texelFetch(gPos, clamp(ivec2(uv * vec2(g)), ivec2(0, 0), g - ivec2(1, 1)), 0).w;
    return t < 0.0 ? 1.0e4 : t;
}

float dofRadius(float d, float focus) {
    return restirDof.x * min(abs(d - focus) / max(d, 1.0e-3), 1.0);
}

void main() {
    vec2 size = vec2(textureSize(resolved, 0));
    ivec2 p = ivec2(v_texcoord0 * size);
    vec3 centre = texelFetch(resolved, p, 0).rgb;
    if (restirDof.x <= 0.0 || restirDenoise.z > 0.5) { gl_FragColor = vec4(centre, 1.0); return; }

    float focus = texelFetch(exposure, ivec2(0, 0), 0).y;
    float radius = dofRadius(dofDistance(v_texcoord0), focus);
    if (radius < 0.25) { gl_FragColor = vec4(centre, 1.0); return; }

    vec3 sum = centre;
    float weight = 1.0;
    ivec2 last = ivec2(size) - ivec2(1, 1);
    for (int i = 0; i < DOF_TAPS; i++) {
        float r = sqrt((float(i) + 0.5) / float(DOF_TAPS)) * radius;
        float a = float(i) * 2.39996323;
        vec2 offset = vec2(cos(a), sin(a)) * r;
        vec2 uv = v_texcoord0 + offset / size;
        // The sample's own circle must reach back here; the 1-pixel ramp keeps the edge soft.
        float w = clamp(dofRadius(dofDistance(uv), focus) - r + 1.0, 0.0, 1.0);
        sum += texelFetch(resolved, clamp(p + ivec2(round(offset)), ivec2(0, 0), last), 0).rgb * w;
        weight += w;
    }
    gl_FragColor = vec4(sum / weight, 1.0);
}
