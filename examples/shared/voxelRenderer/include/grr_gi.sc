// =============================================================================
// grr_gi.sc -- the GI's probe grid: where the probes are, how they are stored, and the light they give a
// surface. Read by probes.frag (the light a probe ray finds where it lands: further bounces) and by
// shade.frag. (docs/plans/voxel-renderer.md, R5)
//
// A grid of GI_X x GI_Y x GI_Z probes, giParams.y apart, round the camera: probe cell c (integer, world)
// lies at (c + 0.5) * spacing, for c from giOrigin to giOrigin + dims. As the camera moves the grid
// scrolls in whole cells, and a cell is stored at c mod dims -- so a scroll moves nothing; the cells it
// brings in find another cell's light in their slot, see its cell in the slot's last texel, and start
// over.
//
// Each probe is four texels of giPacked, side by side: the light arriving at it as L1 spherical
// harmonics, one texel a colour channel (L00, L1y, L1z, L1x), then (its cell, how usable it is: the
// share of its rays that did not start inside solid voxels).
//
// The including shader declares giPacked.
// =============================================================================
#ifndef GRR_GI_SC
#define GRR_GI_SC

#define GI_X 32
#define GI_Y 16
#define GI_Z 32

uniform vec4 giParams;   // x = GI on, y = probe spacing (world units), z = frame
uniform vec4 giOrigin;   // xyz = the grid's lowest cell

// Floor modulo, through floats: GLSL's integer % is undefined for a negative operand, and cells below
// zero are as common as cells above it.
ivec3 giSlot(ivec3 cell) {
    vec3 dims = vec3(GI_X, GI_Y, GI_Z);
    return ivec3(mod(vec3(cell), dims) + 0.5);
}

vec4 giTexel(ivec3 slot, int k) {
    return texelFetch(giPacked, ivec2((slot.x + slot.z * GI_X) * 4 + k, slot.y), 0);
}

// Irradiance from L1 coefficients (Ramamoorthi & Hanrahan), divided by pi: the light a surface facing n
// reflects, per unit albedo -- the same units as shade.frag's other terms.
float giEvaluate(vec4 sh, vec3 n) {
    return max(0.282095 * sh.x + 0.325735 * dot(sh.yzw, vec3(n.y, n.z, n.x)), 0.0);
}

// The probes' light at P on a surface facing N: the eight round it, trilinear, each weighted toward the
// probes on N's side (a probe behind the surface sees the other side of it) and by how usable it is.
// `weight` is how much of that is real (0 outside the grid, or among probes that are all inside solid).
vec3 giIrradiance(vec3 P, vec3 N, out float weight) {
    weight = 0.0;
    float spacing = giParams.y;
    // Half a cell off the surface: the probes round a point on a wall, not in it.
    vec3 g = (P + N * (0.5 * spacing)) / spacing - 0.5 - giOrigin.xyz;
    ivec3 base = ivec3(floor(g));
    vec3 f = g - vec3(base);
    vec3 sum = vec3(0.0, 0.0, 0.0);
    float weights = 0.0;
    for (int k = 0; k < 8; k++) {
        ivec3 offset = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        ivec3 local = base + offset;
        if (any(lessThan(local, ivec3(0, 0, 0))) || any(greaterThanEqual(local, ivec3(GI_X, GI_Y, GI_Z)))) continue;
        ivec3 cell = local + ivec3(giOrigin.xyz);
        ivec3 slot = giSlot(cell);
        vec4 meta = giTexel(slot, 3);
        if (any(notEqual(ivec3(meta.xyz), cell)) || meta.w <= 0.05) continue;
        vec3 tri = mix(vec3(1.0, 1.0, 1.0) - f, f, vec3(offset));
        vec3 toProbe = normalize((vec3(cell) + 0.5) * spacing - P);
        float facing = 0.5 * (dot(toProbe, N) + 1.0);
        float w = tri.x * tri.y * tri.z * (facing * facing + 0.05) * meta.w;
        sum += vec3(giEvaluate(giTexel(slot, 0), N), giEvaluate(giTexel(slot, 1), N), giEvaluate(giTexel(slot, 2), N)) * w;
        weights += w;
    }
    if (weights <= 1e-4) return vec3(0.0, 0.0, 0.0);
    // Fade out over the last cells toward the grid's edges, into the sky ambient beyond.
    vec3 edge = min(g, vec3(GI_X, GI_Y, GI_Z) - 1.0 - g);
    weight = clamp(min(edge.x, min(edge.y, edge.z)) / 3.0, 0.0, 1.0);
    return sum / weights;
}

// The nearest probe's light, cheaply: for the probes' own rays, where it is one bounce among many.
vec3 giNearest(vec3 P, vec3 N, out float weight) {
    weight = 0.0;
    float spacing = giParams.y;
    ivec3 local = ivec3(floor((P + N * (0.5 * spacing)) / spacing - giOrigin.xyz));
    if (any(lessThan(local, ivec3(0, 0, 0))) || any(greaterThanEqual(local, ivec3(GI_X, GI_Y, GI_Z)))) return vec3(0.0, 0.0, 0.0);
    ivec3 cell = local + ivec3(giOrigin.xyz);
    ivec3 slot = giSlot(cell);
    vec4 meta = giTexel(slot, 3);
    if (any(notEqual(ivec3(meta.xyz), cell)) || meta.w <= 0.5) return vec3(0.0, 0.0, 0.0);
    weight = 1.0;
    return vec3(giEvaluate(giTexel(slot, 0), N), giEvaluate(giTexel(slot, 1), N), giEvaluate(giTexel(slot, 2), N));
}

#endif // GRR_GI_SC
