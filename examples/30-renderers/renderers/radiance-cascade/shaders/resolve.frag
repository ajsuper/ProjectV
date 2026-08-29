$input v_color0
$input v_texcoord0

// =============================================================================
// resolve.frag  --  Pass 6 of the radiance-cascade renderer.
//
// Turns cascade 0 (the finest-spatial, fully-merged radiance atlas) into a per-pixel
// indirect-lighting value and composites the frame:
//   final = direct sun (already in gDirect) + albedo * indirect
//
// Indirect at a pixel = the cosine-weighted mean of cascade 0's directional radiance,
// bilinearly interpolated over the four nearest cascade-0 probes. Because cascade 0's
// directions are merged all the way up (its transparent rays already carry the sky and
// the coarser cascades' bounced light), this single integration yields full multi-scale
// GI. Cosine-weighted mean radiance R satisfies E = pi*R, and diffuse Lo = albedo/pi * E
// = albedo * R, so the diffuse response is simply albedo * R (GI_STRENGTH lets it be
// dialed for taste).
//
// Inputs (FBO 1 [0..3] then cascade 0):
//   0 gPos  1 gNormal  2 gAlbedo  3 gDirect   4 cascade0
// Output (FBO 6): 0 composite (HDR linear).
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(gPos,     0);
SAMPLER2D(gNormal,  1);
SAMPLER2D(gAlbedo,  2);
SAMPLER2D(gDirect,  3);
SAMPLER2D(cascade0, 4);

#include <pjv_cascade_common.sc>

#define GI_STRENGTH 1.0

// Cosine-weighted mean radiance over one cascade-0 probe's directions (Tc x Tc tile).
vec3 probeMeanRadiance(ivec2 probe, vec3 N, vec2 jitter) {
    int Tc = tileSizeOf(0);
    vec3  accRad = vec3(0.0);
    float accW   = 0.0;
    for (int dy = 0; dy < Tc; dy++)
    for (int dx = 0; dx < Tc; dx++) {
        ivec2 tile = ivec2(dx, dy);
        vec3  dir  = dirForTile(tile, Tc, jitter);
        float w    = max(dot(dir, N), 0.0);
        ivec2 px   = probe * Tc + tile;
        vec2  uv   = (vec2(px) + 0.5) / windowRes.xy;
        accRad += texture2D(cascade0, uv).rgb * w;
        accW   += w;
    }
    return accRad / max(accW, 1e-3);
}

void main() {
    vec2 uv = v_texcoord0;
    vec4 gp = texture2D(gPos, uv);
    vec4 direct = texture2D(gDirect, uv);

    // Sky / background: gDirect already holds the sky colour.
    if (gp.a < 0.0) {
        gl_FragData[0] = vec4(direct.rgb, 0.0);
        return;
    }

    vec3 N      = normalize(texture2D(gNormal, uv).xyz);
    vec3 albedo = texture2D(gAlbedo, uv).rgb;

    // Bilinear over the four nearest cascade-0 probes (probe centres at (i+0.5)*T0).
    int  Tc = tileSizeOf(0);
    vec2 pxCoord = floor(uv * windowRes.xy);
    vec2 gpc = pxCoord / float(Tc) - 0.5;
    vec2 f   = fract(gpc);
    ivec2 b  = ivec2(floor(gpc));
    vec2 jitter = cascadeJitter();

    vec3 r00 = probeMeanRadiance(b + ivec2(0, 0), N, jitter);
    vec3 r10 = probeMeanRadiance(b + ivec2(1, 0), N, jitter);
    vec3 r01 = probeMeanRadiance(b + ivec2(0, 1), N, jitter);
    vec3 r11 = probeMeanRadiance(b + ivec2(1, 1), N, jitter);
    vec3 indirect = mix(mix(r00, r10, f.x), mix(r01, r11, f.x), f.y);

    vec3 color = direct.rgb + albedo * indirect * GI_STRENGTH;
    gl_FragData[0] = vec4(color, 1.0);
}
