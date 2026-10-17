$input v_color0
$input v_texcoord0

// giblur2.frag -- the ray GI's blur, second pass (stride 3). See grr_giblur.sc.
// Inputs: FBO 9 (giblur1, slot 0), FBO 1 (G-buffer, slots 1..6).

#include <bgfx_shader.sh>
#include <grr_common.sc>

SAMPLER2D(giIn,    0);
SAMPLER2D(gPos,    1);
SAMPLER2D(gNormal, 2);
#define GIBLUR_STRIDE 3.0
#include <grr_giblur.sc>
