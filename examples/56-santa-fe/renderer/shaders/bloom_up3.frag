$input v_color0
$input v_texcoord0

// bloom_up3.frag -- the bloom's level 3 up: bloomDown3 plus a tent of the level below (bloom.sc).
// Inputs: 0 bloomUp4, 1 bloomDown3.

#include <bgfx_shader.sh>

SAMPLER2D(bloomUp4, 0);
SAMPLER2D(bloomDown3, 1);
#define BLOOM_SMALL bloomUp4
#define BLOOM_LEVEL bloomDown3
#include "bloom.sc"
