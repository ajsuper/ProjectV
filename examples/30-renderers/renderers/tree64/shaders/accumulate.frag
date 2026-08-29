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
// Slot 4 is pickBuffer (fbo1's new 5th texture) now that this pass reads frameBufferInputIDs [1, 2] —
// getDependenciesList concatenates fbo1's textures (2,3,4,5,7) then fbo2's (6) in order, so
// accumulatedRender (fbo2) shifted from slot 4 to slot 5. See tree64Renderer/resources.json.
SAMPLER2D(accumulatedRender, 5);

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
