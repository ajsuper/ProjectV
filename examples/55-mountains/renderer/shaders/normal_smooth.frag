$input v_color0
$input v_texcoord0

// =============================================================================
// normal_smooth.frag  --  the voxels' normals, smoothed where the voxels are small on screen.
//
// At G-buffer resolution, after it. Each texel's normal becomes the average of the face normals of the
// surface points round it in the world: a Gaussian two and a half voxel edges wide (gFace.w, the voxel's
// own edge, so every ring smooths alike). Averaged over a staircase, the treads' and risers' normals come
// to the slope's own: a hillside of 30 m voxels shades like a hillside, not a flight of steps. Points off
// this one's plane by more than a voxel or two (a tree in front, the ground behind) count for nothing.
//
// Where a voxel is big on screen -- the ground at your feet -- its own face's normal is kept, crisp: the
// smoothing fades in as a voxel narrows from 3.5 render texels (7 window pixels) to 1.5.
//
// The voxels' colours are averaged the same way, so a coarse ring's dithered mix of ground and canopy
// reads as the blend it stands for, not as specks.
//
// Inputs: 0 gPos (xyz, w distance; < 0 sky), 1 gNormal, 2 gFace (w the voxel's edge), 3 gAlbedo.
// Outputs: 0 gSoft, xyz the normal shading uses (unit); 1 gSoftAlbedo, rgb the albedo it uses.
// =============================================================================

#include <bgfx_shader.sh>
#include <pjv_camera.sc>

SAMPLER2D(gPos, 0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gFace, 2);
SAMPLER2D(gAlbedo, 3);

void main() {
    vec2 uv = v_texcoord0;
    vec4 p0 = texture2D(gPos, uv);
    vec4 n0w = texture2D(gNormal, uv);
    vec3 n0 = n0w.xyz;
    vec3 a0 = texture2D(gAlbedo, uv).rgb;
    if (p0.w < 0.0) { gl_FragData[0] = vec4(0.0, 1.0, 0.0, 0.0); gl_FragData[1] = vec4(a0, 0.0); return; }
    float faceEdge = texture2D(gFace, uv).w;
    // The far field, or voxels shaded as it near the voxels' edge (gNormal.w): taken as they are.
    if (faceEdge <= 0.0 || n0w.w > 0.5) { gl_FragData[0] = vec4(n0, 1.0); gl_FragData[1] = vec4(a0, 1.0); return; }   // the far field: smooth already
    float edge = faceEdge;
    // How many render texels the voxel spans.
    float texelWorld = p0.w * 2.0 * pjvViewTanHalfFov() / passTargetRes.y;
    float voxelTexels = edge / max(texelWorld, 1e-6);
    float amount = 1.0 - smoothstep(1.5, 3.5, voxelTexels);
    if (amount <= 0.0) { gl_FragData[0] = vec4(n0, 1.0); gl_FragData[1] = vec4(a0, 1.0); return; }

    // A 7 x 7 kernel spaced to reach ~2.5 voxels each way.
    float width = 2.5 * edge;
    float stride = max(1.0, width / texelWorld / 3.0);
    vec2 texel = passTargetRes.zw * stride;
    vec3 sum = vec3(0.0, 0.0, 0.0);
    vec3 albedoSum = vec3(0.0, 0.0, 0.0);
    float wSum = 0.0;
    for (int y = -3; y <= 3; y++) {
        for (int x = -3; x <= 3; x++) {
            vec2 su = uv + vec2(float(x), float(y)) * texel;
            vec4 p = texture2D(gPos, su);
            if (p.w < 0.0) continue;
            vec3 n = texture2D(gNormal, su).xyz;
            vec3 d = p.xyz - p0.xyz;
            float d2 = dot(d, d) / (width * width);
            // Off this surface: what stands in front or behind is not part of its slope.
            float off = abs(dot(d, n0)) / edge;
            float w = exp(-2.0 * d2) * (1.0 - smoothstep(1.5, 3.0, off));
            sum += n * w;
            albedoSum += texture2D(gAlbedo, su).rgb * w;
            wSum += w;
        }
    }
    vec3 soft = dot(sum, sum) > 1e-8 ? normalize(sum) : n0;
    gl_FragData[0] = vec4(normalize(mix(n0, soft, amount)), 1.0);
    gl_FragData[1] = vec4(mix(a0, wSum > 1e-6 ? albedoSum / wSum : a0, amount), 1.0);
}
