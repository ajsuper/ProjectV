$input v_color0
$input v_texcoord0

// giblur1.frag -- the ray GI's blur, first pass (stride 1). See grr_giblur.sc.
// Inputs: FBO 8 (giaccum: the light slot 0, its points slot 1), FBO 1 (G-buffer, slots 2..7).

#include <bgfx_shader.sh>
#include <grr_common.sc>

SAMPLER2D(giIn,    0);
SAMPLER2D(gPos,    2);
SAMPLER2D(gNormal, 3);
#define GIBLUR_STRIDE 1.0
#include <grr_giblur.sc>
