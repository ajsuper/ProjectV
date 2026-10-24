$input v_color0
$input v_texcoord0

// bloom_down1.frag -- the bloom's first level down, half the window, from the image after the depth of field and the motion blur; each tap weighted by 1 / (1 + luminance) (bloom.sc).
// Inputs: 0 mbColor.

#include <bgfx_shader.sh>

SAMPLER2D(mbColor, 0);
#define BLOOM_SOURCE mbColor
#define BLOOM_KARIS
#include "bloom.sc"
