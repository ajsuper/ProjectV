// atrous.sc -- passes 6-9, GI resolution: one level of the a-trous wavelet filter (SVGF's second half), on
// the diffuse and the specular indirect light side by side. Included by atrous1/2/4/8.frag, which set
// RESTIR_STEP and where each comes from: step 1 reads the accumulation (with accMom's variances), the
// others the previous level. Four levels reach 61 GI pixels across.
//
// A 5x5 B3-spline kernel spread RESTIR_STEP pixels apart, each tap weighted by
//   normal:    pow(max(n . nq, 0), 32), while a GI pixel is narrower than a voxel
//   plane:     exp(-|(xq - x) . n| / (a sliver that grows with distance and step))
//   luminance: exp(-|l - lq| / (6 sqrt(variance) + epsilon)), each lobe its own -- loose: the GI is soft.
//              l is the centre's 3x3 mean, not the centre: tested against a noisy centre, a dark texel
//              (most of them, where a few paths find a small bright light) rejects its rare bright
//              neighbours, and the filter darkened the light it averaged -- by up to a seventh in the
//              material room, against pt-reference
// The specular has no luminance test -- its few paths are too sparse for one not to darken it -- but is
// held to the lobe's own reach instead: a tap further out than the roughness spreads a
// reflection (RESTIR_SPECULAR_REACH GI pixels at alpha 0.3 and up, one for a mirror -- already the size of
// a GI pixel's footprint, and it takes the sparkle out) does not count, so a sharp reflection is not
// blurred by the filter its rough neighbours need.
// Each variance is filtered with the squared weights, for the next level. restirDenoise.x off: pass-through.
// Geometry: surf0 / surf1, the GI pixels' visible points this frame. Outputs: 0 diffuse, 1 specular (rgb,
// variance).

#define RESTIR_SPECULAR_REACH 12.0

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 centreD = RESTIR_DIFFUSE(p);
    vec4 centreS = RESTIR_SPECULAR(p);
    RestirSurface s = restirSurface(texelFetch(surf0, p, 0), texelFetch(surf1, p, 0));
    if (s.t < 0.0 || restirDenoise.x < 0.5) {
        gl_FragData[0] = centreD;
        gl_FragData[1] = centreS;
        return;
    }
    float ld = 0.0;
    float around = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0, 0), ivec2(passTargetRes.xy) - ivec2(1, 1));
            if (texelFetch(surf0, q, 0).w < 0.0) continue;
            ld += restirLum(RESTIR_DIFFUSE(q).rgb);
            around += 1.0;
        }
    ld /= max(around, 1.0);
    float sigmaD = 6.0 * sqrt(max(centreD.a, 0.0)) + 1e-3;
    // As restirSameSurface: once a GI pixel spans more than a voxel, neighbours on other faces of the
    // same blur of voxels are the same surface as far as this resolution can tell.
    float giPixel = s.t * restirSettings.w * restirJitter.z;
    bool fine = giPixel < 20.0 * restirSettings.y;
    float sigmaZ = 0.01 * s.t * float(RESTIR_STEP) + 0.05 + 2.0 * giPixel * float(RESTIR_STEP);
    float reach = max(RESTIR_SPECULAR_REACH * smoothstep(0.0, 0.3, pjvBsdfAlpha(s.glossiness)), 1.5);
    ivec2 size = ivec2(passTargetRes.xy);

    vec3 sumD = vec3(0.0, 0.0, 0.0);
    vec3 sumS = vec3(0.0, 0.0, 0.0);
    float varianceD = 0.0;
    float varianceS = 0.0;
    float totalD = 0.0;
    float totalS = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            ivec2 q = p + ivec2(x, y) * RESTIR_STEP;
            if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) continue;
            vec4 qs = texelFetch(surf0, q, 0);
            if (qs.w < 0.0) continue;
            vec4 qm = texelFetch(surf1, q, 0);
            vec4 qd = RESTIR_DIFFUSE(q);
            vec4 qsp = RESTIR_SPECULAR(q);
            float kx = x == 0 ? 0.375 : (abs(x) == 1 ? 0.25 : 0.0625);
            float ky = y == 0 ? 0.375 : (abs(y) == 1 ? 0.25 : 0.0625);
            float wn = fine ? pow(max(dot(restirOctDecode(qm.xy), s.n), 0.0), 32.0) : 1.0;
            float wz = exp(-abs(dot(qs.xyz - s.x, s.n)) / sigmaZ);
            float wd = kx * ky * wn * wz * exp(-abs(restirLum(qd.rgb) - ld) / sigmaD);
            float offset = length(vec2(float(x), float(y))) * float(RESTIR_STEP);
            float ws = kx * ky * wn * wz * (offset <= reach ? 1.0 : 0.0);
            sumD += qd.rgb * wd;
            varianceD += qd.a * wd * wd;
            totalD += wd;
            sumS += qsp.rgb * ws;
            varianceS += qsp.a * ws * ws;
            totalS += ws;
        }
    gl_FragData[0] = totalD > 1e-6 ? vec4(sumD / totalD, varianceD / (totalD * totalD)) : centreD;
    gl_FragData[1] = totalS > 1e-6 ? vec4(sumS / totalS, varianceS / (totalS * totalS)) : centreS;
}
