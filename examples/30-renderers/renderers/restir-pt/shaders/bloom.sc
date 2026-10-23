// bloom.sc -- the bloom's mip chain (after the depth of field, window resolution and down): the glow a
// lens and an eye put round anything much brighter than its surroundings -- a sunlit patch, a window, the
// sun -- which the image otherwise ends in a hard edge at the tone curve.
//
// Five levels down, each a 13-tap filter of the one above (Jimenez, "Next Generation Post Processing in Call
// of Duty: Advanced Warfare", 2014); the first weights each tap by 1 / (1 + luminance), so a single very
// bright pixel (the sun's disc, a firefly) cannot flicker the whole glow. Then four levels up, each the
// level's own plus a 3x3 tent of the one below. display.frag mixes the top into the image, a few percent:
// no threshold, so it is energy-preserving and every bright thing glows in proportion.
//
// Included by bloom_down*.frag (BLOOM_SOURCE, optionally BLOOM_KARIS) and bloom_up*.frag (BLOOM_SMALL,
// BLOOM_LEVEL). The targets are filtered (bilinear), so each tap reads four texels.

float bloomLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

#ifdef BLOOM_SOURCE
vec3 bloomTap(vec2 uv, inout float total) {
    vec3 c = texture2D(BLOOM_SOURCE, uv).rgb;
#ifdef BLOOM_KARIS
    float w = 1.0 / (1.0 + bloomLum(c));
#else
    float w = 1.0;
#endif
    total += w;
    return c * w;
}

void main() {
    vec2 uv = v_texcoord0;
    vec2 t = 1.0 / vec2(textureSize(BLOOM_SOURCE, 0));
    // The 13 taps: a centre, four at one texel's diagonal (weight 0.5 between them), and a 3x3 at two
    // texels (corners 0.125 between them, edges 0.25, centre 0.125 again) -- as overlapping 2x2 boxes.
    float ta = 0.0;
    vec3 inner = bloomTap(uv + t * vec2(-1.0, 1.0), ta) + bloomTap(uv + t * vec2(1.0, 1.0), ta) +
                 bloomTap(uv + t * vec2(-1.0, -1.0), ta) + bloomTap(uv + t * vec2(1.0, -1.0), ta);
    float tb = 0.0;
    vec3 corners = bloomTap(uv + t * vec2(-2.0, 2.0), tb) + bloomTap(uv + t * vec2(2.0, 2.0), tb) +
                   bloomTap(uv + t * vec2(-2.0, -2.0), tb) + bloomTap(uv + t * vec2(2.0, -2.0), tb);
    float tc = 0.0;
    vec3 edges = bloomTap(uv + t * vec2(0.0, 2.0), tc) + bloomTap(uv + t * vec2(-2.0, 0.0), tc) +
                 bloomTap(uv + t * vec2(2.0, 0.0), tc) + bloomTap(uv + t * vec2(0.0, -2.0), tc);
    float td = 0.0;
    vec3 centre = bloomTap(uv, td);
    vec3 sum = inner * 0.5 + corners * 0.125 + edges * 0.25 + centre * 0.125;
    float total = ta * 0.5 + tb * 0.125 + tc * 0.25 + td * 0.125;
    gl_FragColor = vec4(sum / max(total, 1e-6), 1.0);
}
#endif

#ifdef BLOOM_SMALL
void main() {
    vec2 uv = v_texcoord0;
    vec2 t = 1.0 / vec2(textureSize(BLOOM_SMALL, 0));
    vec3 up = texture2D(BLOOM_SMALL, uv).rgb * 4.0;
    up += (texture2D(BLOOM_SMALL, uv + vec2(t.x, 0.0)).rgb + texture2D(BLOOM_SMALL, uv - vec2(t.x, 0.0)).rgb +
           texture2D(BLOOM_SMALL, uv + vec2(0.0, t.y)).rgb + texture2D(BLOOM_SMALL, uv - vec2(0.0, t.y)).rgb) * 2.0;
    up += texture2D(BLOOM_SMALL, uv + t).rgb + texture2D(BLOOM_SMALL, uv - t).rgb +
          texture2D(BLOOM_SMALL, uv + vec2(t.x, -t.y)).rgb + texture2D(BLOOM_SMALL, uv + vec2(-t.x, t.y)).rgb;
    gl_FragColor = vec4(texture2D(BLOOM_LEVEL, uv).rgb + up / 16.0, 1.0);
}
#endif
