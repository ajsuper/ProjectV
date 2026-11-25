$input v_color0
$input v_texcoord0

// =============================================================================
// sky_ambient.frag -- the sky's light on a surface, by its facing, and the overcast: sky_dirs.frag's rows
// averaged (santa-fe-reference S22, S23).
//
// Optimized mode has no diffuse ray: a surface's sky light comes from these six means -- the cosine-weighted
// radiance round +x, -x, +y, -y, +z, -z of the sky, the volumetric clouds and the land (an ambient cube) --
// weighed by its normal. Texel 6: the sun's mean transmittance through the clouds over the land round the
// camera, which dims the land's bounce, the clouds' fill from below and the air's light everywhere.
//
// main.cpp reads this 8 x 1 target back (a few frames late) and sets it as uniforms (sfSky*, sfOvercast), so
// every pass has it without a texture slot. Input: sky_dirs 0. Output: rgb (texel 6: r).
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_engine.sc>

SAMPLER2D(skyDirs, 0);

void main() {
    int row = int(gl_FragCoord.x);
    if (row > 6) { gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0); return; }
    int n = textureSize(skyDirs, 0).x;
    vec3 sum = vec3(0.0, 0.0, 0.0);
    for (int i = 0; i < 64; i++) {
        if (i >= n) break;
        sum += texelFetch(skyDirs, ivec2(i, row), 0).rgb;
    }
    gl_FragColor = vec4(sum / float(max(n, 1)), 1.0);
}
