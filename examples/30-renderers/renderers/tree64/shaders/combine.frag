$input v_color0 
$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 windowRes;
uniform vec4 cameraPos;

SAMPLER2D(globalIlluminationdd, 0);
SAMPLER2D(ambientOcclusion, 1);
SAMPLER2D(directIllumination, 2);
SAMPLER2D(normals, 3);
SAMPLER2D(albedo, 4);
SAMPLER2D(motionVector, 5);
SAMPLER2D(finalImage, 6);

void main() {
    vec2 texCord = v_texcoord0.xy;
    vec4 giValue = texture2D(globalIlluminationdd, texCord);
    vec4 aoValue = texture2D(ambientOcclusion, texCord);
    vec4 diValue = texture2D(directIllumination, texCord);
    //vec4 normal = texture2D(normals, texCord);
    vec4 albedo = texture2D(albedo, texCord);
    //vec4 motionVector = texture2D(motionVector, texCord);
    vec4 final = vec4(0);
    //final = (giValue + diValue) * albedo;
    final = giValue;

    float alpha = 0.5;
    gl_FragColor = mix(texture2D(finalImage, texCord), final, alpha);
}
