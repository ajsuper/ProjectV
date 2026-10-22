$input v_color0 
$input v_texcoord0

#include <bgfx_shader.sh>
#include <pjv_color.sc>

uniform vec2 windowRes;
uniform vec3 cameraPos;
//SAMPLER2D(u_texColor, 0);

SAMPLER2D(globalIllumination, 0);
SAMPLER2D(directIllumination, 1);
SAMPLER2D(normal, 2);
SAMPLER2D(albedo, 3);
// Slot 4 is pickBuffer (fbo1's 5th texture, unused by this pass) now that this pass reads
// frameBufferInputIDs [1, 2] — getDependenciesList concatenates fbo1's textures (2,3,4,5,7) then
// fbo2's (6) in order, so accumulatedRender (fbo2) shifted from slot 4 to slot 5. See
// tree64Renderer/resources.json.
SAMPLER2D(accumulatedRender, 5);


void main() {
    vec3 hdr = texture2D(accumulatedRender, v_texcoord0).rgb;

    gl_FragColor = vec4(pjvDisplay(hdr, 1.0, gl_FragCoord.xy), 1.0);   // the engine's neutral transform
}
