$input v_color0
$input v_texcoord0

// The trace pass without what only the reference renderer asks for (santa-fe-reference S26): the diffuse ray and
// its bounce's sun ray, and the haze's traced shafts. Compiled out rather than switched off: the full shader's
// registers are sized for its every path, and the leaner one keeps more rays in flight (1.25 ms of 13.8 a frame,
// optimized preset, 1600 x 900, RTX 3070). main.cpp draws with it whenever those features are off.
#define SF_LEAN 1
#include "trace.sc"
