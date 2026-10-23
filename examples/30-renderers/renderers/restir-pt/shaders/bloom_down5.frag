$input v_color0
$input v_texcoord0

// bloom_down5.frag -- the bloom's level 5 down (bloom.sc).
// Inputs: 0 bloomDown4.

#include <bgfx_shader.sh>

SAMPLER2D(bloomDown4, 0);
#define BLOOM_SOURCE bloomDown4
#include "bloom.sc"
