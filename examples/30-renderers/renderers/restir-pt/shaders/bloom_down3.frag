$input v_color0
$input v_texcoord0

// bloom_down3.frag -- the bloom's level 3 down (bloom.sc).
// Inputs: 0 bloomDown2.

#include <bgfx_shader.sh>

SAMPLER2D(bloomDown2, 0);
#define BLOOM_SOURCE bloomDown2
#include "bloom.sc"
