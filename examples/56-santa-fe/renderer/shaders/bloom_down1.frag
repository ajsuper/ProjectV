$input v_color0
$input v_texcoord0

// bloom_down1.frag -- the bloom's first level down, half the window, from the resolved frame; each tap weighted by
// 1 / (1 + luminance) (bloom.sc). Inputs: 0 resolved (1: the resolve's history).

#include <bgfx_shader.sh>

SAMPLER2D(resolved, 0);
#define BLOOM_SOURCE resolved
#define BLOOM_KARIS
#include "bloom.sc"
