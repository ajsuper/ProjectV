// denoise.frag at a-trous level stride 4. One of three: the stride is a compile-time constant (see the
// note at ATROUS_STRIDE there), and these were once built by a per-example script that no longer exists,
// leaving untracked binaries that never rebuilt. A wrapper per level keeps them in the build.
// No $input lines here: shaderc reads them out of the included file.
#define ATROUS_STRIDE 4
#include "denoise.frag"
