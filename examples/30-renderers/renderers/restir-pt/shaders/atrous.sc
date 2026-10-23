// atrous.sc -- passes 6-9, GI resolution: one level of the a-trous wavelet filter (SVGF's second half).
// Included by atrous1/2/4/8.frag, which set RESTIR_STEP and where the colour comes from:
//   step 1 reads the accumulation (accIrr + accMom's variance), the others the previous level.
// Four levels reach 61 GI pixels across; the luminance test is loose (6 sigma) -- the GI is meant to be
// soft, and what this leaves the resolve averages.
// A 5x5 B3-spline kernel spread RESTIR_STEP pixels apart, each tap weighted by
//   normal:    pow(max(n . nq, 0), 32), while a GI pixel is narrower than a voxel
//   plane:     exp(-|(xq - x) . n| / (a sliver that grows with distance and step))
//   luminance: exp(-|l - lq| / (6 sqrt(variance) + epsilon))
// and the variance filtered with the squared weights, for the next level. restirDenoise.x off: pass-through.
// Geometry: accPos / accNrm, the GI pixels' visible points this frame. Output: rgb, variance -- and the
// last level (RESTIR_ATROUS_LAST) passes accPos / accNrm on beside it, for the composite's upsample,
// which has no input slots left for the accumulation's four.

#ifdef RESTIR_ATROUS_LAST
#define RESTIR_ATROUS_OUT(C) gl_FragData[0] = (C); gl_FragData[1] = texelFetch(accPos, p, 0); gl_FragData[2] = texelFetch(accNrm, p, 0)
#else
#define RESTIR_ATROUS_OUT(C) gl_FragColor = (C)
#endif

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 centre = RESTIR_COLOUR(p);
    vec4 surface = texelFetch(accPos, p, 0);
    if (surface.w < 0.0 || restirDenoise.x < 0.5) { RESTIR_ATROUS_OUT(centre); return; }
    vec3 P = surface.xyz;
    vec3 N = texelFetch(accNrm, p, 0).xyz;
    float t = surface.w;
    float l = restirLum(centre.rgb);
    float sigmaL = 6.0 * sqrt(max(centre.a, 0.0)) + 1e-3;
    // As restirSameSurface: once a GI pixel spans more than a voxel, neighbours on other faces of the
    // same blur of voxels are the same surface as far as this resolution can tell.
    float giPixel = t * restirSettings.w * restirJitter.z;
    bool fine = giPixel < 20.0 * restirSettings.y;
    float sigmaZ = 0.01 * t * float(RESTIR_STEP) + 0.05 + 2.0 * giPixel * float(RESTIR_STEP);
    ivec2 size = ivec2(passTargetRes.xy);

    vec3 sum = vec3(0.0, 0.0, 0.0);
    float variance = 0.0;
    float total = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            ivec2 q = p + ivec2(x, y) * RESTIR_STEP;
            if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) continue;
            vec4 qs = texelFetch(accPos, q, 0);
            if (qs.w < 0.0) continue;
            vec4 qc = RESTIR_COLOUR(q);
            float kx = x == 0 ? 0.375 : (abs(x) == 1 ? 0.25 : 0.0625);
            float ky = y == 0 ? 0.375 : (abs(y) == 1 ? 0.25 : 0.0625);
            float wn = fine ? pow(max(dot(texelFetch(accNrm, q, 0).xyz, N), 0.0), 32.0) : 1.0;
            float wz = exp(-abs(dot(qs.xyz - P, N)) / sigmaZ);
            float wl = exp(-abs(restirLum(qc.rgb) - l) / sigmaL);
            float w = kx * ky * wn * wz * wl;
            sum += qc.rgb * w;
            variance += qc.a * w * w;
            total += w;
        }
    vec4 filtered = total > 1e-6 ? vec4(sum / total, variance / (total * total)) : centre;
    RESTIR_ATROUS_OUT(filtered);
}
