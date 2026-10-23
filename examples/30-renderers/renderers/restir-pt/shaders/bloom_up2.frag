$input v_color0
$input v_texcoord0

// bloom_up2.frag -- the bloom's level 2 up: bloomDown2 plus a tent of the level below (bloom.sc).
// Inputs: 0 bloomUp3, 1 bloomDown2.

#include <bgfx_shader.sh>

SAMPLER2D(bloomUp3, 0);
SAMPLER2D(bloomDown2, 1);
#define BLOOM_SMALL bloomUp3
#define BLOOM_LEVEL bloomDown2
#include "bloom.sc"
