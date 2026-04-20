$input v_color0 
$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 frameCount;

SAMPLER2D(globalIllumination, 0);
SAMPLER2D(directIllumination, 1);
SAMPLER2D(normal, 2);
SAMPLER2D(albedo, 3);
SAMPLER2D(accumulatedRender, 4);

const float maxAccumFrames = 50.0;  // controls convergence speed

void main() {
    vec3 currFrame = min(max(0.0, texture2D(globalIllumination, v_texcoord0).rgb), 5);
    vec3 prevAccum = texture2D(accumulatedRender, v_texcoord0).rgb;

    //float alpha = 0.1;


    // Exponential moving average accumulation
    //vec3 accumulated = mix(prevAccum, currFrame, alpha);
    float n = frameCount.x - frameCount.z;
    vec3 accumulated = (prevAccum * (n - 1.0) + currFrame) / n;

    
    if (frameCount.y != 0) {
        accumulated = currFrame;
    }

    gl_FragColor = vec4(accumulated, 1.0);
}
