$input v_color0
$input v_texcoord0

// pack.frag -- the GI probes (probes.frag's four targets, one texel a probe), filtered and laid side by
// side into one texture, four texels a probe, so shade.frag and the next update read every probe through
// one sampler (a pass has nine).
//
// The filter: each probe with its six neighbours, those that hold their own cell and are usable, the
// probe itself counting double. A probe's few rays an update leave it noisy; neighbours a spacing apart
// see nearly the same light, and averaging them takes most of the noise out at no cost in rays. (The
// probes' own history stays unfiltered -- probes.frag reads its targets, not this.)
//
// Input: FBO 12 (slots 0..3). Output: FBO 13 (giPacked).

#include <bgfx_shader.sh>

SAMPLER2D(probeR,    0);
SAMPLER2D(probeG,    1);
SAMPLER2D(probeB,    2);
SAMPLER2D(probeMeta, 3);

#define GI_X 32
#define GI_Y 16
#define GI_Z 32

ivec3 slotOf(ivec3 cell) {
    return ivec3(mod(vec3(cell), vec3(GI_X, GI_Y, GI_Z)) + 0.5);
}
ivec2 texelOf(ivec3 slot) {
    return ivec2(slot.x + slot.z * GI_X, slot.y);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 probe = ivec2(pixel.x / 4, pixel.y);
    int k = pixel.x % 4;
    vec4 meta = texelFetch(probeMeta, probe, 0);
    if (k == 3) { gl_FragColor = meta; return; }
    ivec3 cell = ivec3(meta.xyz);
    vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);
    float weights = 0.0;
    for (int n = 0; n < 7; n++) {
        ivec3 offset = n == 0 ? ivec3(0, 0, 0) : n == 1 ? ivec3(1, 0, 0) : n == 2 ? ivec3(-1, 0, 0) : n == 3 ? ivec3(0, 1, 0)
                     : n == 4 ? ivec3(0, -1, 0) : n == 5 ? ivec3(0, 0, 1) : ivec3(0, 0, -1);
        ivec3 neighbour = cell + offset;
        ivec2 at = texelOf(slotOf(neighbour));
        vec4 theirs = texelFetch(probeMeta, at, 0);
        if (any(notEqual(ivec3(theirs.xyz), neighbour)) || theirs.w <= 0.05) continue;
        float w = (n == 0 ? 2.0 : 1.0) * theirs.w;
        vec4 sh = k == 0 ? texelFetch(probeR, at, 0) : k == 1 ? texelFetch(probeG, at, 0) : texelFetch(probeB, at, 0);
        sum += sh * w;
        weights += w;
    }
    gl_FragColor = weights > 0.0 ? sum / weights
                 : (k == 0 ? texelFetch(probeR, probe, 0) : k == 1 ? texelFetch(probeG, probe, 0) : texelFetch(probeB, probe, 0));
}
