$input v_color0
$input v_texcoord0

// bloom_down2.frag -- the bloom's level 2 down (bloom.sc).
// Inputs: 0 bloomDown1.

#include <bgfx_shader.sh>

SAMPLER2D(bloomDown1, 0);
#define BLOOM_SOURCE bloomDown1
#include "bloom.sc"
