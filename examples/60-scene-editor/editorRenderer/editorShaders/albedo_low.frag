// =============================================================================
// albedo_low.frag  --  The viewport's primary march at reduced resolution, for reconstruct.frag.
//
// While the camera moves, the viewport traces at a fraction of the panel's pixels and reconstruct.frag
// rebuilds the full-resolution G-buffer from what these rays found. That only works because of what is
// recorded here: not a colour to be magnified, but WHICH VOXEL each ray hit -- its box (centre, edge
// length, and the chunk whose rotation orients it) and its stored albedo, which is constant across the
// voxel. A full-resolution pixel then asks a geometric question of its neighbours' voxels ("does MY ray
// hit this box, and where?") instead of averaging their colours, and gets the answer the full march
// would have given. See reconstruct.frag for when it cannot, and what it does then.
//
// The lean march: the reduced-resolution path is only ever used with the Advanced preview off, whose
// colour is per voxel. A peeled or reflective colour is not, and could not be rebuilt like this.
//
// Targets (lowMaterial, lowSurface), both RGBA32F:
//   lowMaterial = (albedo.rgb, (headerIndex + 1) * 8 + face)
//                 w == 0 is a miss. face (0..5) is the face of the voxel the ray entered through, in
//                 the chunk's own axes: 2 * axis + (1 if the outward normal points along +axis). A
//                 float holds the packed value exactly up to 2^24, so 2M chunks; the +1 frees zero.
//   lowSurface  = (voxel centre in world space, edge length in world units)
// =============================================================================
#define EDITOR_ALBEDO_LEAN 1
#define EDITOR_ALBEDO_NO_MAIN 1
#include "albedo.frag"

void main() {
    vec2 uvJit = v_texcoord0 + viewportJitter() / passTargetRes.xy;
    Ray ray = primaryRay(uvJit);
    SceneIntersectData hit = raySceneIntersect(ray, viewportPrimaryQuery()).hit;

    if (hit.foundBox.size < 0.0 || hit.rayT <= 0.0 || dot(hit.normal, hit.normal) < 0.5) {
        gl_FragData[0] = vec4(0.0, 0.0, 0.0, 0.0);
        gl_FragData[1] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // foundBox.position is the voxel's MINIMUM corner in the chunk's own frame, carried into world
    // space by the chunk's rotation and translation -- so the centre is half an edge along each of the
    // chunk's rotated axes from it, not along the world's.
    mat3 rotation = rotationFromQuat(headers(int(hit.headerIndex)).rotation);
    float size = hit.foundBox.size;
    vec3 centre = hit.foundBox.position + rotation * vec3(0.5 * size, 0.5 * size, 0.5 * size);

    // The entry face, in the chunk's axes. reconstruct.frag only accepts a voxel through a face some
    // coarse ray actually saw -- see the note there on internal faces.
    vec3 localNormal = hit.normal * rotation;   // transpose(R) * normal
    vec3 axisWeight = abs(localNormal);
    int axis = axisWeight.x >= axisWeight.y && axisWeight.x >= axisWeight.z ? 0 : (axisWeight.y >= axisWeight.z ? 1 : 2);
    float component = axis == 0 ? localNormal.x : (axis == 1 ? localNormal.y : localNormal.z);
    int face = 2 * axis + (component > 0.0 ? 1 : 0);

    gl_FragData[0] = vec4(fetchVoxelColorFromHit(hit), float((int(hit.headerIndex) + 1) * 8 + face));
    gl_FragData[1] = vec4(centre, size);
}
