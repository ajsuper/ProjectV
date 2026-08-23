// =============================================================================
// pjv_face_key.sc  --  The identity of a voxel face, and how to compare two of them.
//
// This renderer computes and stores its indirect light PER VOXEL FACE. That only works if every
// pass agrees, exactly, on what "the same face" means -- a key that disagrees by one bit between
// the pass that writes it and the pass that reads it silently throws away a converged value and
// re-exposes the raw sample, which looks like noise rather than like a bug. So the definition
// lives here, once, and gbuffer/accumulate/compose all include it.
//
// A face key is a vec4:
//   xyz = the voxel's integer coordinate in its chunk's own voxel space (SceneIntersectData::voxelCoord)
//   w   = headerIndex * 6 + faceId, where faceId is 0..5 for -X +X -Y +Y -Z +Z
//
// WHY THE CHUNK INDEX IS IN THERE: voxelCoord is chunk-LOCAL, so two chunks placed side by side
// both contain a voxel (0,0,0). Without headerIndex the two faces are the same key and each would
// inherit the other's lighting. Folding it into w rather than spending a fifth channel keeps the
// key in one RGBA32F target.
//
// WHY IT IS EXACT: every component is a small integer held in a float32, which represents integers
// exactly up to 2^24. voxelCoord is bounded by the chunk resolution (256 today) and
// headerIndex * 6 by six times the chunk count, so both are orders of magnitude inside that. The
// key therefore survives a render-target round trip bit-for-bit and can be compared with ==,
// which a position-derived key never could.
// =============================================================================

// ---- PER-VOXEL LIGHTING (U) -------------------------------------------------------------------
// A mode switch, read by the two passes that decide how finely the indirect term is resolved:
// pjv_probe.sc, which chooses what a probe integrates, and resolve.frag, which chooses which probe
// samples belong to a pixel. Everything else -- the key format, gFace, the temporal gates, the
// upscaler -- is left exactly as it is, which is why the mode is three small edits rather than a
// second pipeline.
//
// OFF (default): the indirect term is per voxel FACE. A probe integrates the hemisphere about one
// face's normal, from that face's centre, and only samples from the same face count. This is the
// physically finer answer and it is what makes a voxel's lit top read differently from its shaded
// side without any direct light being involved.
//
// ON: the indirect term is per VOXEL. A probe integrates the whole SPHERE around the voxel -- see
// pjvProbeGather -- and any sample from the same voxel counts, whichever face it stood on. Every
// face of a voxel then converges to one number, so a voxel is lit as a unit.
//
// Nothing about this is a fallback for the other. It is the coarser quantity, and coarser is
// sometimes what is wanted: it is flat-shaded rather than form-shaded, it is a sixth of the distinct
// values to converge so it settles faster and holds still through more camera motion, and it is the
// granularity a real surface cache indexed by materialListIndex would store.
//
// It is DECLARED as a macro rather than a function so that only the shaders that actually use it
// need `uniform vec4 debugParams` -- the passes that include this header for pjvSameFace alone are
// unaffected and do not have to grow a uniform they never read.
#define PJV_PER_VOXEL_LIGHTING (debugParams.w > 0.5)

// The key a pixel with no surface carries. Deliberately NOT zero: zero is voxel (0,0,0) face -X,
// which is a real face in any scene with a chunk at the origin, and the sky would then share that
// face's converged lighting. 1e20 is outside any scene's coordinate range and is exactly
// representable, so the == below is safe.
#define FACE_KEY_NONE 1e20

// Which of a voxel's six faces the ray entered through, from that face's outward normal. Voxel
// faces are axis-aligned, so the largest component of n IS the axis -- no tolerance needed, and
// the max() picks a single winner even on the degenerate diagonal.
float pjvFaceId(vec3 n) {
    vec3 an = abs(n);
    int axis = (an.x >= an.y && an.x >= an.z) ? 0 : (an.y >= an.z ? 1 : 2);
    float sgn = (axis == 0 ? n.x : (axis == 1 ? n.y : n.z));
    return float(axis * 2 + (sgn > 0.0 ? 1 : 0));
}

// The full key for a hit. voxelCoord must come from the march (SceneIntersectData::voxelCoord),
// never from foundBox.position -- see the note in gbuffer.frag about the float32 round trip.
vec4 pjvFaceKey(ivec3 voxelCoord, uint headerIndex, vec3 n) {
    return vec4(vec3(voxelCoord), float(headerIndex) * 6.0 + pjvFaceId(n));
}

// True when two keys name the same face. Exact equality is correct here (see the header note), and
// a background key never matches -- including against another background key, which is what stops
// two unrelated sky pixels from pooling history.
bool pjvSameFace(vec4 a, vec4 b) {
    if (a.w >= FACE_KEY_NONE || b.w >= FACE_KEY_NONE) return false;
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// True when two keys name the same VOXEL, whatever face of it. The face id is the low part of w
// (w = headerIndex * 6 + faceId), so dividing it out leaves chunk + voxel identity.
//
// This is the gate a SWAYING BLADE needs, and it is exact rather than a loosened guess. The envelope
// march deliberately reports the blade's SOURCE voxel as its identity -- "identity is the SOURCE, so
// it holds still", see envelopeResolve -- so voxelCoord is rock steady from frame to frame even as
// the blade moves. What is not steady is the NORMAL, because that comes from the drawn box, and the
// ray meets a differently-oriented piece of the blade each frame. So the key churns in exactly one
// of its four components, and pjvSameFace, which needs all four, rejects every frame.
//
// Matching on the voxel instead pools a blade's faces into one value. For foliage that is the right
// quantity anyway -- a blade's indirect light is ambient, not a property of which side of it the ray
// happened to strike -- and being an identity rather than a distance test, it cannot ghost.
//
// It earns its place on static geometry too, for the pixel sitting exactly on a voxel's CORNER: the
// sub-pixel jitter flips such a pixel between two faces of one voxel, which changes the face id and
// nothing else.
bool pjvSameVoxel(vec4 a, vec4 b) {
    if (a.w >= FACE_KEY_NONE || b.w >= FACE_KEY_NONE) return false;
    return a.x == b.x && a.y == b.y && a.z == b.z && floor(a.w / 6.0) == floor(b.w / 6.0);
}

// The face's outward normal, recovered from the key alone. The temporal pass needs this to decide
// whether a history texel it did NOT match exactly is at least on a same-facing surface, and
// storing the normal again would be three more channels for something six values already imply.
vec3 pjvFaceNormal(float faceIdAndChunk) {
    int faceId = int(mod(faceIdAndChunk, 6.0));
    int axis = faceId / 2;
    float sgn = (faceId - axis * 2) == 1 ? 1.0 : -1.0;
    return axis == 0 ? vec3(sgn, 0.0, 0.0)
         : axis == 1 ? vec3(0.0, sgn, 0.0)
                     : vec3(0.0, 0.0, sgn);
}
