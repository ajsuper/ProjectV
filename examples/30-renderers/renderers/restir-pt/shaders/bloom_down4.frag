$input v_color0
$input v_texcoord0

// bloom_down4.frag -- the bloom's level 4 down (bloom.sc).
// Inputs: 0 bloomDown3.

#include <bgfx_shader.sh>

SAMPLER2D(bloomDown3, 0);
#define BLOOM_SOURCE bloomDown3
#include "bloom.sc"
