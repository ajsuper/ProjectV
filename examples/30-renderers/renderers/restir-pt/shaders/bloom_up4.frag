$input v_color0
$input v_texcoord0

// bloom_up4.frag -- the bloom's level 4 up: bloomDown4 plus a tent of the level below (bloom.sc).
// Inputs: 0 bloomDown5, 1 bloomDown4.

#include <bgfx_shader.sh>

SAMPLER2D(bloomDown5, 0);
SAMPLER2D(bloomDown4, 1);
#define BLOOM_SMALL bloomDown5
#define BLOOM_LEVEL bloomDown4
#include "bloom.sc"
