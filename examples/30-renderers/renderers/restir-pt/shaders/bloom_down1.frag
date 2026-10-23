$input v_color0
$input v_texcoord0

// bloom_down1.frag -- the bloom's first level down, half the window, from the image after the depth of field; each tap weighted by 1 / (1 + luminance) (bloom.sc).
// Inputs: 0 dofColor.

#include <bgfx_shader.sh>

SAMPLER2D(dofColor, 0);
#define BLOOM_SOURCE dofColor
#define BLOOM_KARIS
#include "bloom.sc"
