$input v_color0 
$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec2 windowRes;
uniform vec3 cameraPos;
//SAMPLER2D(u_texColor, 0);

SAMPLER2D(globalIllumination, 0);
SAMPLER2D(directIllumination, 1);
SAMPLER2D(normal, 2);
SAMPLER2D(albedo, 3);
SAMPLER2D(accumulatedRender, 4);

vec3 acesToneMap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture2D(accumulatedRender, v_texcoord0).rgb;

    vec3 mapped = acesToneMap(hdr);

    // Gamma correction
    mapped = pow(mapped, vec3(1.0 / 3.2));

    gl_FragColor = vec4(mapped, 1.0);
}
