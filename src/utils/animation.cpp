#include "utils/animation.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "core/log.h"
#include "utils/material.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

namespace projv::utils {
    namespace {

        // Which slots of a palette animate, and under which motion set. Index is the slot; -1 means
        // the slot does not move. Kept as a small dense vector rather than a map because a palette is
        // at most 256 entries and this is read once per voxel of the blob.
        struct AnimatedSlots {
            std::vector<int16_t> motionSet;   // -1 = static
            bool any = false;
            // Identity of the animated set, so two components sharing a blob can be compared without
            // comparing whole palettes. Order-independent is not needed: slot order is the palette's
            // order and two components sharing a blob share its slot numbering by construction.
            uint64_t signature = 0;
        };

        AnimatedSlots classifySlots(const std::vector<Material>& palette, const AnimationState& anim) {
            AnimatedSlots out;
            out.motionSet.assign(palette.size(), -1);
            for (size_t s = 0; s < palette.size(); s++) {
                if (!materialIsAnimated(palette[s])) continue;
                uint32_t set = materialMotionSet(palette[s]);
                // A material pointing at an empty table row is flagged but has no field to move it.
                // Treated as static rather than as an error: the flag is authored, the table is
                // runtime, and a scene loaded before its motion sets are configured should render
                // rather than refuse.
                if (anim.sets[set].kind == MotionKind::None) continue;
                out.motionSet[s] = static_cast<int16_t>(set);
                out.any = true;
                out.signature = out.signature * 1000003ull + (s + 1) * 17ull + set;
            }
            return out;
        }

        // How far, in ENVELOPE CELLS, a source voxel's influence reaches.
        //
        // A cell is 4 voxels across, so an amplitude of up to 4 voxels can only ever push a voxel
        // into a cell adjacent to its own -- radius 1. The ceil is what keeps that true if someone
        // dials the amplitude past the useful range: the envelope stays valid, it just costs more.
        // ---- EVERY LATTICE OFFSET A SOURCE CAN BE DRAWN AT, EXACTLY -------------------------
        //
        // The drawn position is `source + round(localDir * signal * amplitude)` with signal clamped
        // to [-1, 1] (see pjvValueNoise and pjvMotionDisplacement). So the reachable offsets are the
        // rounded points of a LINE SEGMENT of half-length `amplitude` voxels through the origin --
        // typically three of them for a one-to-two voxel amplitude, and never more than a handful.
        //
        // This replaced a blanket dilation of one ENVELOPE CELL in every direction, which marked a
        // 3x3 block of cells -- reaching four to seven voxels away to bound a displacement of one or
        // two, and marking nine cells where one or two are reachable. The cost of that is not the
        // memory; it is that a ray crosses several times as many marked cells, and a marked cell that
        // resolves to nothing still pays a full block walk. Measured as the dominant term: narrowing
        // the candidate window made things SLOWER because rays then travelled further before
        // resolving, which is the same relationship seen from the other side.
        //
        // ENUMERATED EXACTLY RATHER THAN SAMPLED, because the failure mode of missing one offset is
        // a voxel drawn nowhere -- skipped by the geometry march as animated, and unreachable here
        // because its cell was never marked. Dense sampling cannot be made safe by inspection: two
        // axes' rounding boundaries can fall arbitrarily close together, leaving an interval narrower
        // than any fixed step. So the boundaries themselves are computed. Along axis a the rounded
        // value changes exactly where localDir[a] * t crosses a half-integer; between consecutive
        // crossings the whole rounded vector is constant, so evaluating one point per interval sees
        // every offset there is.
        void reachableOffsets(const MotionSet& ms, const core::quat& worldToLocal,
                              std::vector<core::ivec3>& out) {
            out.clear();
            if (ms.kind != MotionKind::Sway || ms.amplitude <= 0.0f) return;

            // Normalised HERE because the GPU normalises on upload (see the animation upload in
            // perform_renderer.cpp). A non-unit direction left as authored would have the bake
            // enveloping one amplitude while the shader drew another.
            core::vec3 d = ms.direction;
            float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (len < 1e-6f) return;
            core::vec3 u = worldToLocal * (d / len);

            const float A = ms.amplitude;

            std::vector<float> ts{-A, A};
            for (int axis = 0; axis < 3; axis++) {
                float ua = (axis == 0) ? u.x : (axis == 1) ? u.y : u.z;
                if (std::fabs(ua) < 1e-6f) continue;
                int kLo = static_cast<int>(std::floor(-A * std::fabs(ua) - 0.5f)) - 1;
                int kHi = static_cast<int>(std::ceil(A * std::fabs(ua) + 0.5f)) + 1;
                for (int k = kLo; k <= kHi; k++) {
                    float t = (static_cast<float>(k) + 0.5f) / ua;
                    if (t >= -A && t <= A) ts.push_back(t);
                }
            }
            std::sort(ts.begin(), ts.end());

            auto add = [&out](const core::vec3& p) {
                core::ivec3 o{static_cast<int>(std::floor(p.x + 0.5f)),
                              static_cast<int>(std::floor(p.y + 0.5f)),
                              static_cast<int>(std::floor(p.z + 0.5f))};
                for (const core::ivec3& e : out) {
                    if (e.x == o.x && e.y == o.y && e.z == o.z) return;
                }
                out.push_back(o);
            };

            // Both endpoints, and one interior point of every interval between boundaries.
            add(u * -A);
            add(u * A);
            for (size_t i = 0; i + 1 < ts.size(); i++) {
                add(u * (0.5f * (ts[i] + ts[i + 1])));
            }
        }

        // ---- WHERE AN ADVECTING FIELD CAN DRAW SOMETHING, IN ENVELOPE CELLS ------------------
        //
        // Advection's envelope is a different shape from sway's and is built a different way, for a
        // reason that is worth stating rather than discovering: sway's reachable set is EXACT -- the
        // drawn position is source + round(dir * signal * amplitude), a line segment's worth of
        // lattice points -- while advection's is the result of iterating a noisy field up to `travel`
        // steps, and there is no closed form for where that lands.
        //
        // So this bounds it instead. Per step the flow is `direction + swirl`, with the swirl bounded
        // by turbulence * (1 + turbulenceGrowth); after k steps the rise is at most k along the
        // direction and the lateral drift at most k * that bound. That is a CONE, and marking the
        // cone generously is the safe direction: a marked cell that resolves to nothing costs a
        // trace, while an unmarked cell that should have drawn something is a hole in the flame.
        //
        // Emitted in CELL units rather than voxel units, unlike reachableOffsets. Sway reaches one or
        // two voxels and enumerating those is a handful of points; a plume reaching sixteen voxels
        // with lateral spread is thousands, nearly all of them landing in a cell some neighbour
        // already marked. Enumerating the cells directly is the same set for a sixty-fourth of the
        // work, and the one voxel of slop it introduces at the cone's rim is absorbed by the +1 cell
        // of padding below.
        void advectCellOffsets(const MotionSet& ms, const core::quat& worldToLocal,
                               std::vector<core::ivec3>& out) {
            out.clear();
            if (ms.kind != MotionKind::Advect || ms.travel <= 0.0f) return;

            core::vec3 d = ms.direction;
            float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (len < 1e-6f) return;
            // Rotated into THIS chunk's voxel frame, for the same reason reachableOffsets does it:
            // the shader traces along `Rinv * flow`, so a rotated chunk's plume rises along a
            // different lattice axis than the world direction names.
            core::vec3 u = worldToLocal * (d / len);

            // The trace takes one step per voxel of travel, so `travel` is also the step count.
            const float travel = std::min(ms.travel, 32.0f);
            // ---- HOW FAR ONE STEP CAN GO SIDEWAYS, AND HOW THAT ACCUMULATES ------------------
            //
            // Per step the shader adds `direction + swirl`, with the swirl built from two independent
            // noise values in the plane perpendicular to the direction and a quarter of their sum
            // along it, all scaled by turbulence * (1 + turbulenceGrowth * ageFrac). Both noise
            // values are bounded by 1, so the perpendicular part of a step is at most sqrt(2) times
            // that scale and the axial part is 1 +/- half of it.
            //
            // Accumulating those bounds over k steps is what gives the cone its radius. The sum has
            // to be taken PROPERLY rather than as k times the largest step: the growth term ramps
            // from 0 to turbulenceGrowth across the trace, so charging every step the tip's
            // turbulence overstates the reach by a factor of a few -- and the cost of that is not
            // memory but rays, since every marked cell a ray crosses buys a full backward trace.
            const float perStep = ms.turbulence * 1.4143f;
            // sum(i = 0 .. k-1) of perStep * (1 + growth * i / travel)
            auto driftAfter = [&](float kRaw) {
                // Clamped to the step count: past the last step the trace has stopped, so
                // extrapolating the sum would widen the cone's rim for steps that never happen.
                const float k = std::min(std::max(kRaw, 0.0f), travel);
                if (k <= 0.0f) return 0.0f;
                return perStep * (k + ms.turbulenceGrowth * k * (k - 1.0f) / (2.0f * travel));
            };
            // The rise itself is perturbed too, so the axis runs a little past `travel`.
            const float axialMax = travel + 0.5f * driftAfter(travel);
            const int nCells = static_cast<int>(std::ceil(axialMax / 4.0f));

            std::unordered_set<uint64_t> seen;
            auto add = [&out, &seen](int x, int y, int z) {
                // Packed rather than compared pairwise: the cone runs to a few hundred cells, where
                // sway's reachable set is three or four and a linear scan is cheaper than a hash.
                uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(x + 512)) << 40) |
                               (static_cast<uint64_t>(static_cast<uint32_t>(y + 512)) << 20) |
                               static_cast<uint64_t>(static_cast<uint32_t>(z + 512));
                if (!seen.insert(key).second) return;
                out.push_back(core::ivec3{x, y, z});
            };

            for (int k = 0; k <= nCells; k++) {
                const float rise = std::min(static_cast<float>(k) * 4.0f, axialMax);
                const core::vec3 c = u * (rise / 4.0f);            // cone axis, in cells
                // The lateral bound in cells, plus one. The padding covers two things at once: the
                // source voxel's position within its own cell (up to 3 voxels off the cell corner
                // this is measured from) and the rounding of the axis position.
                const int rad = static_cast<int>(std::ceil(driftAfter(rise) / 4.0f)) + 1;
                const int cx = static_cast<int>(std::lround(c.x));
                const int cy = static_cast<int>(std::lround(c.y));
                const int cz = static_cast<int>(std::lround(c.z));
                for (int dz = -rad; dz <= rad; dz++)
                for (int dy = -rad; dy <= rad; dy++)
                for (int dx = -rad; dx <= rad; dx++) {
                    add(cx + dx, cy + dy, cz + dz);
                }
            }
        }

        // Every chunk handle a component owns, whether it is one loose chunk or a whole grid.
        void collectChunkHandles(const Scene& scene, const ComponentRecord& comp,
                                 std::vector<ChunkHandle>& out) {
            if (comp.kind == ComponentKind::Chunk) {
                out.push_back(comp.chunkHandle);
            } else if (comp.kind == ComponentKind::Grid && comp.gridIndex >= 0 &&
                       static_cast<size_t>(comp.gridIndex) < scene.grids.size()) {
                for (int32_t idx : scene.grids[comp.gridIndex].cellToChunk) {
                    if (idx >= 0) out.push_back(static_cast<ChunkHandle>(idx));
                }
            }
        }
    } // namespace

    EnvelopeBakeReport bakeAnimationEnvelope(Scene& scene, const AnimationState& anim) {
        EnvelopeBakeReport report;

        // Which component's animated-slot set a blob was baked from, so a blob shared by two
        // components that disagree is reported rather than quietly baked twice.
        std::unordered_map<int32_t, uint64_t> blobSignature;
        std::unordered_set<int32_t> blobsDone;

        for (size_t ci = 0; ci < scene.components.size(); ci++) {
            ComponentRecord& component = scene.components[ci];
            if (component.materialPalette.empty()) continue;

            AnimatedSlots slots = classifySlots(component.materialPalette, anim);

            std::vector<ChunkHandle> chunkHandles;
            collectChunkHandles(scene, component, chunkHandles);
            if (chunkHandles.empty()) continue;
            if (slots.any) report.componentsAnimated++;

            for (ChunkHandle handle : chunkHandles) {
                if (handle >= scene.chunks.size()) continue;
                Chunk& chunk = scene.chunks[handle];
                if (!chunk.alive) continue;
                int32_t poolIndex = chunk.geometryPoolIndex;
                if (poolIndex < 0 || poolIndex >= static_cast<int32_t>(scene.geometryPool.size())) continue;

                // A blob is shared across instances; bake it once.
                auto seen = blobSignature.find(poolIndex);
                if (seen != blobSignature.end()) {
                    if (seen->second != slots.signature) {
                        // Named, not swallowed. The envelope is a property of the geometry plus the
                        // palette, and one blob cannot carry two answers -- so the second component
                        // renders its animated materials at rest. That is a degradation with a stated
                        // cause, which is the whole point of reporting it.
                        report.sharedBlobConflicts++;
                        core::warn("bakeAnimationEnvelope: component {} ('{}') shares geometry blob {} "
                                   "with a component whose palette animates a different set of slots. "
                                   "The envelope was baked for the first one; this component's animated "
                                   "materials will render statically. Give it its own copy of the .data "
                                   "to animate it independently.",
                                   ci, component.name, poolIndex);
                    }
                    continue;
                }
                blobSignature.emplace(poolIndex, slots.signature);

                GeometryBlob& blob = scene.geometryPool[poolIndex];

                // No animated material in this component: the blob gets NO envelope. Clearing rather
                // than leaving whatever was there is what makes a re-bake after un-flagging a
                // material actually remove the animation, and what makes this idempotent.
                if (!slots.any) {
                    if (!blob.envelope.empty() || !blob.envelopeMotion.empty()) {
                        blob.envelope.clear();
                        blob.envelopeMotion.clear();
                        blob.dirty = true;
                    }
                    report.blobsSkipped++;
                    blobsDone.insert(poolIndex);
                    continue;
                }

                const uint32_t resolution = chunk.header.resolution;
                const uint32_t envRes = envelopeResolutionFor(resolution);

                std::unique_ptr<VoxelBrickMap> geom =
                    brickMapFromTree64(blob.geometry, blob.materialIDs, resolution);
                if (!geom) { report.blobsSkipped++; continue; }

                core::ivec3 envBrickDims{
                    std::max(1, static_cast<int>(envRes / BRICK_SIZE)),
                    std::max(1, static_cast<int>(envRes / BRICK_SIZE)),
                    std::max(1, static_cast<int>(envRes / BRICK_SIZE))};
                std::unique_ptr<VoxelBrickMap> env = createVoxelBrickMap(envBrickDims);
                if (!env) { report.blobsSkipped++; continue; }

                uint64_t cellsHere = 0;
                uint64_t sourcesHere = 0;

                // The reachable offsets, once per chunk per motion set rather than once per source.
                // Per CHUNK because the field is defined in world space and this chunk may be
                // rotated: the offsets a source can take are along the direction rotated INTO this
                // chunk's voxel frame, which is what the shader's `Rinv * pjvMotionDisplacement(...)`
                // computes. Deriving them from the world direction alone would leave a rotated
                // chunk's foliage enveloped along the wrong axis.
                const core::quat worldToLocal = glm::conjugate(chunk.header.rotation);
                std::vector<core::ivec3> reach[MAX_MOTION_SETS];      // VOXEL offsets (Sway)
                std::vector<core::ivec3> cone[MAX_MOTION_SETS];       // CELL offsets (Advect)
                bool hasAdvect = false;
                for (uint32_t si = 0; si < MAX_MOTION_SETS; si++) {
                    reachableOffsets(anim.sets[si], worldToLocal, reach[si]);
                    advectCellOffsets(anim.sets[si], worldToLocal, cone[si]);
                    if (!cone[si].empty()) hasAdvect = true;
                }

                // ---- ADVECTING SETS ARE MARKED FIRST, AND THEY OVERWRITE ---------------------
                //
                // The motion byte holds ONE set and envelopes overlap, so somebody loses. Which one
                // is not a toss-up: the two kinds resolve differently and only one of them needs the
                // byte to be right.
                //
                // A sway source that loses the byte is still drawn. pjvResolveCell enumerates the
                // candidates around a cell and asks each which field IT belongs to; the byte is only
                // a hint for bracketing that search. An advecting field that loses the byte has no
                // such route -- there is no candidate to ask, because what is drawn is found by
                // tracing a field backwards and the field has to be named before the trace starts.
                // So an unmarked advect cell is a hole in the flame, while an unmarked sway cell is
                // at worst a slightly wider search.
                //
                // Implemented as two passes over the sources rather than a read-modify-write of the
                // brick map, which keeps the "first writer wins" rule below untouched. The second
                // pass is skipped entirely when no advecting set is configured, which is every scene
                // that only has grass in it.
                const int passes = hasAdvect ? 2 : 1;
                for (int pass = 0; pass < passes; pass++) {

                for (int bz = 0; bz < geom->brickDims.z; bz++)
                for (int by = 0; by < geom->brickDims.y; by++)
                for (int bx = 0; bx < geom->brickDims.x; bx++) {
                    core::ivec3 brickCoord{bx, by, bz};
                    uint32_t bi = computeBrickZOrder(brickCoord, geom->brickDims);
                    if (bi >= geom->bricks.size() || !geom->bricks[bi]) continue;
                    const BrickData& brick = *geom->bricks[bi];

                    for (const std::pair<const uint32_t, uint8_t>& entry : brick.materials) {
                        if (entry.second >= slots.motionSet.size()) continue;
                        int16_t set = slots.motionSet[entry.second];
                        if (set < 0) continue;
                        if (hasAdvect) {
                            const bool isAdvect = anim.sets[set].kind == MotionKind::Advect;
                            if (isAdvect != (pass == 0)) continue;
                        }

                        core::ivec3 local = reverseZOrderIndex(entry.first);
                        const int vx = bx * static_cast<int>(BRICK_SIZE) + local.x;
                        const int vy = by * static_cast<int>(BRICK_SIZE) + local.y;
                        const int vz = bz * static_cast<int>(BRICK_SIZE) + local.z;

                        // Voxels sealed inside solid geometry can never be seen, so enveloping them
                        // is pure waste -- and a canopy is mostly interior, which is why dense
                        // foliage costs so much more to bake than sparse grass does. Only the shell
                        // can be drawn, plus whatever a voxel of motion newly exposes, and the
                        // shell's own envelope already covers that.
                        if (brickMapHasVoxel(*geom, vx + 1, vy, vz) &&
                            brickMapHasVoxel(*geom, vx - 1, vy, vz) &&
                            brickMapHasVoxel(*geom, vx, vy + 1, vz) &&
                            brickMapHasVoxel(*geom, vx, vy - 1, vz) &&
                            brickMapHasVoxel(*geom, vx, vy, vz + 1) &&
                            brickMapHasVoxel(*geom, vx, vy, vz - 1)) continue;

                        sourcesHere++;

                        const int envCells = static_cast<int>(envRes);
                        // First writer wins where two motion sets overlap. Rare, and the alternative
                        // (a per-cell set list) costs a format field to resolve a case that reads as
                        // one field slightly influencing the other. The pass split above is what
                        // decides who gets to be first.
                        auto mark = [&](int ex, int ey, int ez) {
                            if (ex < 0 || ey < 0 || ez < 0 ||
                                ex >= envCells || ey >= envCells || ez >= envCells) return;
                            if (brickMapHasVoxel(*env, ex, ey, ez)) return;
                            brickMapSetVoxel(*env, ex, ey, ez, static_cast<uint8_t>(set));
                            cellsHere++;
                        };

                        if (anim.sets[set].kind == MotionKind::Advect) {
                            // The cone, hung off the source's OWN cell. See advectCellOffsets.
                            const int sx = vx / 4, sy = vy / 4, sz = vz / 4;
                            for (const core::ivec3& off : cone[set]) {
                                mark(sx + off.x, sy + off.y, sz + off.z);
                            }
                        } else {
                            // Every cell this source can actually be drawn in, and no others. The
                            // offsets are lattice-exact (see reachableOffsets), so a cell left
                            // unmarked here is one no ray needs to look in.
                            for (const core::ivec3& off : reach[set]) {
                                const int px = vx + off.x, py = vy + off.y, pz = vz + off.z;
                                // Drawn outside the chunk is drawn nowhere: the envelope is this
                                // chunk's own tree and has no cell to mark.
                                if (px < 0 || py < 0 || pz < 0 ||
                                    px >= static_cast<int>(resolution) ||
                                    py >= static_cast<int>(resolution) ||
                                    pz >= static_cast<int>(resolution)) continue;
                                mark(px / 4, py / 4, pz / 4);
                            }
                        }
                    }
                }
                } // pass

                if (cellsHere == 0) {
                    blob.envelope.clear();
                    blob.envelopeMotion.clear();
                    blob.dirty = true;
                    report.blobsSkipped++;
                    blobsDone.insert(poolIndex);
                    continue;
                }

                blob.envelope = buildTree64FromBrickMap(*env, static_cast<int>(envRes));
                blob.envelopeMotion.clear();
                bakeMaterialsFromBrickMap(blob.envelope, blob.envelopeMotion, *env);
                blob.dirty = true;

                report.blobsBaked++;
                report.envelopeCells += cellsHere;
                report.sourceVoxels += sourcesHere;
                blobsDone.insert(poolIndex);
            }
        }

        if (report.blobsBaked > 0) {
            core::info("bakeAnimationEnvelope: {} blob(s) enveloped, {} cell(s) from {} animated "
                       "voxel(s), across {} animated component(s).",
                       report.blobsBaked, report.envelopeCells, report.sourceVoxels,
                       report.componentsAnimated);
        } else if (report.componentsAnimated > 0) {
            // Flagged materials but nothing baked. Worth saying out loud: the scene will render
            // statically and look correct, so nothing on screen points at this.
            core::warn("bakeAnimationEnvelope: {} component(s) have animated materials but no "
                       "envelope was produced -- their geometry will render at rest.",
                       report.componentsAnimated);
        }
        return report;
    }

} // namespace projv::utils
