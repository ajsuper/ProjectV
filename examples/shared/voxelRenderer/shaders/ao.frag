$input v_color0
$input v_texcoord0

// =============================================================================
// ao.frag -- pass 2, at render resolution. Ambient occlusion at two scales, from the G-buffer alone:
//
//   CONTACT  a few voxels: the darkening in a crease, under a ledge, where a crate meets the floor.
//   WIDE     a few metres: how enclosed this point is -- a courtyard's corner, the inside of a doorway.
//
// The samples are SURFACE POINTS, not probes: points in a spiral round the pixel on screen, read back as
// world positions from the G-buffer. A point occludes by how far it rises above this pixel's tangent
// plane -- the cosine of its elevation, falling off with distance (the Alchemy / HBAO family). So a point
// on the same plane occludes exactly nothing, however the face is turned to the camera, and a point past
// a convex edge lies below the plane and occludes nothing either: no self-occlusion on slanted faces and
// no dark rim on outer edges.
//
// Each pixel's spiral is turned by one of 16 rotations in a 4x4 ordered pattern, static in time --
// shade.frag's 5x5 [1 2 2 2 1] filter gives every rotation equal weight, so the pattern cancels on any
// surface the filter spans.
//
// Inputs: FBO 1 (G-buffer). Output (FBO 2): r = contact visibility, g = wide visibility (1 open).
// =============================================================================

#include <bgfx_shader.sh>
#include <grr_common.sc>

SAMPLER2D(gPos,    0);
SAMPLER2D(gNormal, 1);
SAMPLER2D(gFace,   4);   // w = the voxel's edge

#include <pjv_engine.sc>   // passTargetRes, passInputRes: engine-set
uniform vec4 renderParams;   // y = AO on

#define CONTACT_SAMPLES 8
#define WIDE_SAMPLES    12
// Radii in voxels of the finest size (lightParams.w), so they follow the scene's own scale.
#define CONTACT_RADIUS_VOXELS 5.0
#define WIDE_RADIUS_VOXELS    28.0
// Screen-space cap, in render pixels: up close a world radius would otherwise sprawl across the frame.
#define MAX_RADIUS_PIXELS     140.0
// How dark full occlusion reads: the estimator averages cosines, about 0.3 in a right-angled crease.
#define CONTACT_STRENGTH      1.6
#define WIDE_STRENGTH         1.3
// A point must rise this far above the tangent plane (in voxels) to count: the voxel faces' own steps.
#define BIAS_VOXELS           0.05
#define SPIRAL_TURNS          7.0

float bayer4(vec2 pixel) {
    ivec2 p = ivec2(mod(pixel, 4.0));
    int fine   = 2 * ((p.x ^ p.y) & 1) + (p.y & 1);
    int coarse = 2 * (((p.x ^ p.y) >> 1) & 1) + ((p.y >> 1) & 1);
    return float(4 * fine + coarse);
}

// Over `count` points in a spiral of `radius` world units (radiusPixels on screen) round the pixel: the
// occlusion they make, 0 open .. 1.
float gather(vec3 P, vec3 N, vec2 centre, float radius, float radiusPixels, float bias, int count, float turn,
             float jitter) {
    if (radiusPixels < 1.5) return 0.0;
    float occluded = 0.0;
    for (int i = 0; i < count; i++) {
        float t = (float(i) + jitter) / float(count);
        float angle = 6.2831853 * (t * SPIRAL_TURNS + turn);
        float r = max(radiusPixels * t, 1.0);
        vec2 uv = centre + vec2(cos(angle), sin(angle)) * r * passTargetRes.zw;
        if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) continue;
        uv = (floor(uv * passTargetRes.xy) + 0.5) * passTargetRes.zw;   // a texel, not a blend of two
        vec4 Q = texture2D(gPos, uv);
        if (Q.w < 0.0) continue;                                         // sky: open
        vec3 v = Q.xyz - P;
        float d = length(v);
        float rise = dot(v, N);
        if (d < 1e-4 || rise <= bias) continue;
        // Elevation above the tangent plane, faded out from half the radius to the radius.
        occluded += (rise / d) * (1.0 - smoothstep(radius * 0.5, radius, d));
    }
    return occluded / float(count);
}

void main() {
    vec4 pos = texture2D(gPos, v_texcoord0);
    if (pos.w < 0.0 || renderParams.y < 0.5) {
        gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }
    vec4 nv = texture2D(gNormal, v_texcoord0);
    vec3 N = normalize(nv.xyz);
    float voxel = max(lightParams.w, 1e-4);

    // World size of one render pixel at this distance: world radii to screen radii.
    float pixelWorld = pos.w * (2.0 * tan(radians(GRR_FOV * 0.5))) / passTargetRes.y;
    float bias = max(BIAS_VOXELS * texture2D(gFace, v_texcoord0).w, pixelWorld * 0.05);
    vec2 pixel = floor(v_texcoord0 * passTargetRes.xy);
    vec2 centre = (pixel + 0.5) * passTargetRes.zw;
    float turn = (bayer4(pixel) + 0.5) / 16.0;
    float jitter = fract(bayer4(pixel.yx) * 0.618034 + 0.25);

    float contactRadius = CONTACT_RADIUS_VOXELS * voxel, wideRadius = WIDE_RADIUS_VOXELS * voxel;
    float contactPixels = min(contactRadius / pixelWorld, MAX_RADIUS_PIXELS);
    float widePixels = min(wideRadius / pixelWorld, MAX_RADIUS_PIXELS);
    // Capped on screen, the world radius shrinks with it, so the falloff matches what was sampled.
    float contact = gather(pos.xyz, N, centre, contactPixels * pixelWorld, contactPixels, bias, CONTACT_SAMPLES,
                           turn, jitter);
    float wide = gather(pos.xyz, N, centre, widePixels * pixelWorld, widePixels, bias, WIDE_SAMPLES,
                        fract(turn + 0.5), fract(jitter + 0.5));
    gl_FragColor = vec4(clamp(1.0 - contact * CONTACT_STRENGTH, 0.0, 1.0), clamp(1.0 - wide * WIDE_STRENGTH, 0.0, 1.0), 0.0, 1.0);
}
