#ifndef PROJECTV_ANIMATION_H
#define PROJECTV_ANIMATION_H

#include <cstdint>
#include <string>
#include <vector>

#include "core/math.h"
#include "data_structures/scene.h"

// Per-voxel animation: the motion table, and the bake that produces an envelope from it.
//
// The design in one paragraph. Geometry stores the REST POSE and nothing else. A material's flags
// byte says "this moves, under motion set N" (see MATERIAL_FLAG_ANIMATED in scene.h). The bake walks
// the geometry and writes a second, quarter-resolution tree64 -- the envelope -- marking every place
// an animated voxel could be drawn, and a byte per marked cell naming the motion set. At render time
// the traversal enters a marked cell and asks which voxel, if any, is drawn there right now.
//
// That split is the whole idea: WHERE SOMETHING MIGHT BE is baked, static and sparse; WHERE IT IS is
// evaluated, dynamic and free. Nothing is stored per frame and no geometry is rebuilt.
namespace projv {

    // What kind of field a motion set describes. The traversal switches on this, so adding a fourth
    // is a case and a table row rather than a new file -- which is the test this had to pass to be an
    // engine feature at all.
    enum class MotionKind : uint32_t {
        None    = 0,
        // Displacement. A voxel is drawn at rest + d(position, time), with d a smooth low-frequency
        // field. Grass sway, leaf flutter, flags, cloth.
        //
        // Smooth is load-bearing, not an aesthetic choice. Neighbouring voxels of one blade must move
        // together or the blade separates into floating segments -- which does not read as motion
        // blur, it reads as a bug. A per-voxel position hash has no scale and therefore no
        // neighbourhood coherence, so it is exactly the wrong generator here.
        Sway    = 1,
        // Backward advection. A cell asks where its contents came from N steps ago and walks a
        // material chain by that count. Fire, smoke, steam.
        //
        // The count IS the age, which is why the material chain costs nothing extra: blue at the base
        // to orange at the tip falls out of "how far has this parcel travelled".
        Advect  = 2,
    };

    // One row of the motion table. Sixteen of these exist; a material names one by index.
    struct MotionSet {
        MotionKind kind = MotionKind::None;
        // Peak displacement in VOXELS. The envelope is baked for this value, so raising it at runtime
        // does not crash -- it runs out of envelope, and the extra motion is simply not drawn.
        //
        // One to two voxels is the useful range. Below it there is no visible animation; above it the
        // motion stops being coherent and approaches noise, which no mechanism handles well.
        float amplitude = 0.0f;
        // Spatial frequency of the field, in cycles per voxel. Low: the field must vary over tens of
        // voxels, not per voxel. See the note on MotionKind::Sway.
        float frequency = 0.0f;
        float speed = 0.0f;             // how fast the field travels, voxels per second
        core::vec3 direction{1.0f, 0.0f, 0.0f};   // wind / rise direction, normalised on upload
        float turbulence = 0.0f;        // 0..1, how much the field departs from a plane wave

        // Whether this field can move geometry vertically -- DERIVED from the direction rather than
        // stored beside it, so the two cannot disagree.
        //
        // Worth having at all because it HALVES both the bake and the resolve: a horizontal field can
        // only ever draw a voxel in its own row, so neither has to look up or down. Wind is
        // horizontal, which is the common case; a rising plume is not.
        bool isVertical() const { return direction.y > 1e-4f || direction.y < -1e-4f; }

        // ---- ADVECTION ONLY (MotionKind::Advect); ignored by Sway ----------------------------
        //
        // How readily a risen parcel burns out, 0..1. The GAPS this leaves are what make a flame read
        // as fire rather than as a lit volume, and they are sampled at the parcel's SOURCE rather
        // than where it is drawn -- so every cell tracing back to one source dies together and whole
        // tongues detach, instead of individual voxels speckling out.
        float dissolve = 0.0f;
        // How much the swirl grows with age. Real fire is laminar where it leaves the fuel and ragged
        // by the time it is a few voxels up; uniform turbulence gives a body that wanders as a whole
        // rather than one that breaks into tongues.
        float turbulenceGrowth = 0.0f;
        // How far a parcel may travel from its source, in VOXELS. For Sway this is `amplitude`; for
        // Advect the two are different quantities -- amplitude is the swirl's reach, this is the
        // rise -- and the envelope is baked from THIS one.
        float travel = 0.0f;
    };

    // The engine-owned animation state. Time is a member rather than a global on purpose: a renderer
    // writing an image sequence and a voxelizer asked for frame 37 both need to sample
    // deterministically, and `inline Camera cam` is the pattern not to repeat.
    struct AnimationState {
        float time = 0.0f;                        // seconds
        MotionSet sets[MAX_MOTION_SETS];
    };
}

namespace projv::utils {

    // What a bake did, and -- more usefully -- what it could not do.
    //
    // Reported rather than logged-and-forgotten because the failure this system actually has is
    // silent: geometry flagged as animated with nowhere to be drawn is drawn NOWHERE. Not stale, not
    // misplaced: absent. A caller that can see `skippedNoEnvelope > 0` can say so; a caller reading
    // pixels cannot tell it from a traversal bug, and historically did not.
    struct EnvelopeBakeReport {
        uint32_t blobsBaked = 0;          // blobs given an envelope
        uint32_t blobsSkipped = 0;        // blobs with no animated material in them
        uint64_t envelopeCells = 0;       // total cells marked
        uint64_t sourceVoxels = 0;        // animated voxels the cells were grown from
        uint32_t componentsAnimated = 0;
        // A blob referenced by two components whose palettes disagree about which slots animate. The
        // envelope can only be right for one of them, so the second is reported by name rather than
        // silently overwriting the first. Rare: instances of one asset almost always share a palette.
        uint32_t sharedBlobConflicts = 0;
        bool ok() const { return sharedBlobConflicts == 0; }
    };

    /**
     * Builds (or rebuilds) the animation envelope for every component in the scene.
     *
     * Reads: each component's palette flags, and each of its blobs' geometry + materialIDs.
     * Writes: `GeometryBlob::envelope` and `GeometryBlob::envelopeMotion`, and marks the blob dirty
     * so the next GPU flush uploads them.
     *
     * A blob with no animated material gets EMPTY arrays, which is the same thing as never having
     * been baked -- so running this on a static scene is a no-op that costs one pass over the
     * palettes, and running it twice is idempotent.
     *
     * Call it after loading and after any edit that changes animated geometry. The envelope is
     * derived from the geometry, so an edit invalidates it; it is not itself editable.
     */
    EnvelopeBakeReport bakeAnimationEnvelope(Scene& scene, const AnimationState& anim);

    /**
     * The envelope resolution for a chunk resolution: a quarter, floored at 4 (the smallest tree64).
     * The traversal derives the same value from the chunk header, so the two must agree -- which is
     * why it is a function in a header rather than a shift written out at each site.
     */
    inline uint32_t envelopeResolutionFor(uint32_t chunkResolution) {
        uint32_t r = chunkResolution / 4u;
        return r < 4u ? 4u : r;
    }

} // namespace projv::utils

#endif // PROJECTV_ANIMATION_H
