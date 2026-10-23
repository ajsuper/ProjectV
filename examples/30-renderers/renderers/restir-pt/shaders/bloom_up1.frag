$input v_color0
$input v_texcoord0

// bloom_up1.frag -- the bloom's level 1 up: bloomDown1 plus a tent of the level below (bloom.sc).
// Inputs: 0 bloomUp2, 1 bloomDown1.

#include <bgfx_shader.sh>

SAMPLER2D(bloomUp2, 0);
SAMPLER2D(bloomDown1, 1);
#define BLOOM_SMALL bloomUp2
#define BLOOM_LEVEL bloomDown1
#include "bloom.sc"
