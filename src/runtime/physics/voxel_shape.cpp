#include "voxel_shape.h"

#include <algorithm>
#include <cmath>

#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

#include "utils/tile_collision.h"

namespace projv::runtime {
    namespace {
        core::vec3 toCore(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
        JPH::Vec3 toJolt(core::vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }

        // The boxes of one query, per thread: Jolt collides on several. `level` 1 for a query that
        // makes another inside it (voxels against voxels: one shape's boxes, then each against the other's).
        std::vector<utils::TileBox>& scratch(int level = 0) {
            thread_local std::vector<utils::TileBox> boxes[2];
            boxes[level].clear();
            return boxes[level];
        }

        // A box's Jolt box, centred on its middle, rounded as a compound of boxes' are (the convex
        // radius never more than half its thinnest side).
        struct CellBox {
            JPH::Vec3     centre, half;
            JPH::BoxShape shape;
            CellBox(const utils::TileBox& b, const JPH::PhysicsMaterial* material)
                : centre(toJolt(0.5f * core::vec3(b.low + b.high + core::ivec3(1)))),
                  half(toJolt(0.5f * core::vec3(b.high - b.low + core::ivec3(1)))),
                  shape(half, std::min(JPH::cDefaultConvexRadius, 0.5f * half.ReduceMin()), material) {
                shape.SetEmbedded();   // on the stack: nothing may free it
            }
        };

        // A box in the shape's space, scaled, as the inclusive voxel range it covers.
        void voxelRange(const JPH::AABox& box, JPH::Vec3Arg scale, core::ivec3& low, core::ivec3& high) {
            const JPH::Vec3 inverse = scale.Reciprocal();
            const JPH::Vec3 a = box.mMin * inverse, b = box.mMax * inverse;
            low = core::ivec3(glm::floor(toCore(JPH::Vec3::sMin(a, b))));
            high = core::ivec3(glm::floor(toCore(JPH::Vec3::sMax(a, b))));
        }
    }

    VoxelShape::VoxelShape(std::shared_ptr<const utils::TileTable> table,
                           std::array<JPH::RefConst<JPH::PhysicsMaterial>, 256> slotMaterials, JPH::uint idBits,
                           const std::array<float, 256>* densities)
        : JPH::Shape(JPH::EShapeType::User1, SUBTYPE), tiles(std::move(table)), materials(std::move(slotMaterials)) {
        layout = utils::tileTableLayout(*tiles);
        const core::ivec3 low = layout.blockOrigin * 16, high = (layout.blockOrigin + layout.blockDims) * 16;
        bounds = JPH::AABox(toJolt(core::vec3(low)), toJolt(core::vec3(high)));
        volume = float(utils::tileTableVoxelCount(*tiles));
        int levels = 0;   // log2 of the resolution
        while ((1u << levels) < std::max(tiles->resolution, 1u)) levels++;
        cellShift = utils::tileCellShift(tiles->resolution, idBits);
        cellsPerAxis = 1 << std::max(levels - cellShift, 0);
        bits = JPH::uint(std::max(3 * (levels - cellShift), 1));
        if (!densities) return;
        // Its weight, once: every box's mass at its slot's density, their centre, and their inertia
        // about it -- each box's own about its middle, moved there (the parallel axis theorem).
        moving = true;
        utils::tileBlockBoxes(*tiles, layout, greedy, greedyStarts);
        // Its voxels' own box, not its blocks': a piece of a few voxels in a 16^3 block would reach a
        // block's width round it, and the broadphase would pair it with everything near.
        core::ivec3 contentLow, contentHigh;
        if (utils::tileTableBounds(*tiles, contentLow, contentHigh))
            bounds = JPH::AABox(toJolt(core::vec3(contentLow)), toJolt(core::vec3(contentHigh + core::ivec3(1))));
        std::vector<utils::TileBox> boxes;
        boxesIn(core::ivec3(0), core::ivec3(int(tiles->resolution) - 1), boxes);
        double total = 0.0;
        core::dvec3 moment(0.0);
        for (const utils::TileBox& b : boxes) {
            const core::dvec3 size(b.high - b.low + core::ivec3(1));
            const double m = size.x * size.y * size.z * double((*densities)[b.slot]);
            total += m;
            moment += m * (core::dvec3(b.low) + 0.5 * size);
        }
        if (!(total > 0.0)) total = 1.0;
        for (const utils::TileBox& b : boxes)
            innerRadius = std::max(innerRadius, 0.5f * float(glm::min(glm::min(b.high.x - b.low.x, b.high.y - b.low.y), b.high.z - b.low.z) + 1));
        const core::dvec3 c = moment / total;
        centre = JPH::Vec3(float(c.x), float(c.y), float(c.z));
        double inertia[3][3] = {};
        for (const utils::TileBox& b : boxes) {
            const core::dvec3 size(b.high - b.low + core::ivec3(1));
            const double m = size.x * size.y * size.z * double((*densities)[b.slot]);
            const core::dvec3 d = core::dvec3(b.low) + 0.5 * size - c;
            const double own[3] = {m / 12.0 * (size.y * size.y + size.z * size.z), m / 12.0 * (size.x * size.x + size.z * size.z),
                                   m / 12.0 * (size.x * size.x + size.y * size.y)};
            const double dd = glm::dot(d, d);
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) inertia[i][j] += (i == j ? own[i] + m * dd : 0.0) - m * d[i] * d[j];
        }
        mass.mMass = float(total);
        mass.mInertia = JPH::Mat44::sIdentity();
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) mass.mInertia(i, j) = float(inertia[i][j]);
    }

    void VoxelShape::boxesIn(core::ivec3 low, core::ivec3 high, std::vector<utils::TileBox>& out) const {
        if (greedy.empty()) {
            utils::tileBoxesIn(*tiles, layout, low, high, out);
            return;
        }
        low = glm::max(low, layout.blockOrigin * 16);
        high = glm::min(high, (layout.blockOrigin + layout.blockDims) * 16 - core::ivec3(1));
        if (glm::any(glm::greaterThan(low, high))) return;
        const core::ivec3 blockLow = low >> 4, blockHigh = high >> 4;
        for (int bz = blockLow.z; bz <= blockHigh.z; ++bz)
            for (int by = blockLow.y; by <= blockHigh.y; ++by)
                for (int bx = blockLow.x; bx <= blockHigh.x; ++bx) {
                    const size_t index = size_t(layout.blockIndex({bx, by, bz}));
                    for (uint32_t i = greedyStarts[index]; i < greedyStarts[index + 1]; i++) {
                        const utils::TileBox& b = greedy[i];
                        if (glm::all(glm::lessThanEqual(b.low, high)) && glm::all(glm::lessThanEqual(low, b.high))) out.push_back(b);
                    }
                }
    }

    JPH::SubShapeIDCreator VoxelShape::cellId(const JPH::SubShapeIDCreator& creator, core::ivec3 voxel) const {
        const core::ivec3 c = glm::clamp(voxel >> cellShift, core::ivec3(0), core::ivec3(cellsPerAxis - 1));
        return creator.PushID(JPH::uint(c.x + cellsPerAxis * (c.y + cellsPerAxis * c.z)), bits);
    }

    core::ivec3 VoxelShape::cellCorner(const JPH::SubShapeID& id, JPH::SubShapeID* remainder) const {
        JPH::SubShapeID rest;
        const int index = int(id.PopID(bits, rest));
        if (remainder) *remainder = rest;
        const core::ivec3 c(index % cellsPerAxis, (index / cellsPerAxis) % cellsPerAxis, index / (cellsPerAxis * cellsPerAxis));
        return c << cellShift;
    }

    JPH::MassProperties VoxelShape::GetMassProperties() const {
        if (moving) return mass;
        // Static, so never weighed; a compound of it asks, to place its centre (one voxel a unit).
        JPH::MassProperties p;
        p.SetMassAndInertiaOfSolidBox(bounds.GetSize(), bounds.GetSize().ReduceMin() > 0.0f ? volume / bounds.GetVolume() : 1.0f);
        return p;
    }

    const JPH::PhysicsMaterial* VoxelShape::GetMaterial(const JPH::SubShapeID& id) const {
        const core::ivec3 corner = cellCorner(id);
        uint8_t slot = 0;
        bool found = utils::tileTableVoxel(*tiles, corner, &slot);
        if (!found) {   // the cell's first box: a contact names its cell, not its voxel
            std::vector<utils::TileBox>& boxes = scratch();
            boxesIn(corner, corner + core::ivec3(cellSize() - 1), boxes);
            if ((found = !boxes.empty())) slot = boxes.front().slot;
        }
        const JPH::PhysicsMaterial* m = found ? materials[slot].GetPtr() : nullptr;
        return m ? m : JPH::PhysicsMaterial::sDefault.GetPtr();
    }

    JPH::Vec3 VoxelShape::GetSurfaceNormal(const JPH::SubShapeID& id, JPH::Vec3Arg position) const {
        const core::vec3 p = toCore(position + centre);
        const core::ivec3 n = utils::tileSurfaceNormal(*tiles, p);
        if (n != core::ivec3(0)) return toJolt(core::vec3(n));
        // Not on a face (inside, or off the surface by more than a quarter voxel): the cell's side nearest it.
        const core::vec3 d = p - (core::vec3(cellCorner(id)) + 0.5f * float(cellSize()));
        const int a = std::abs(d.x) >= std::abs(d.y) ? (std::abs(d.x) >= std::abs(d.z) ? 0 : 2) : (std::abs(d.y) >= std::abs(d.z) ? 1 : 2);
        core::vec3 out(0.0f);
        out[a] = d[a] < 0.0f ? -1.0f : 1.0f;
        return toJolt(out);
    }

    void VoxelShape::GetSupportingFace(const JPH::SubShapeID& id, JPH::Vec3Arg direction, JPH::Vec3Arg scale,
                                       JPH::Mat44Arg transform, SupportingFace& vertices) const {
        // The face of the box the id names (its first voxel's), or of the box round a cell's solid voxels.
        const core::ivec3 corner = cellCorner(id), last = corner + core::ivec3(cellSize() - 1);
        std::vector<utils::TileBox>& boxes = scratch();
        boxesIn(corner, last, boxes);
        if (boxes.empty()) return;
        utils::TileBox all = boxes.front();
        if (boxes.size() > 1) {
            all = {last, corner, 0};
            for (const utils::TileBox& b : boxes) {
                all.low = glm::min(all.low, glm::max(b.low, corner));
                all.high = glm::max(all.high, glm::min(b.high, last));
            }
        }
        CellBox cell(all, nullptr);
        cell.shape.GetSupportingFace(JPH::SubShapeID(), direction, scale,
                                     transform * JPH::Mat44::sTranslation(scale * (cell.centre - centre)), vertices);
    }

    bool VoxelShape::CastRay(const JPH::RayCast& ray, const JPH::SubShapeIDCreator& creator, JPH::RayCastResult& hit) const {
        utils::TileRayHit h;
        if (!utils::tileTableRaycast(*tiles, layout, toCore(ray.mOrigin + centre), toCore(ray.mDirection), hit.mFraction, h) ||
            !(h.t < hit.mFraction))
            return false;
        hit.mFraction = h.t;
        hit.mSubShapeID2 = cellId(creator, h.voxel).GetID();
        return true;
    }

    void VoxelShape::CastRay(const JPH::RayCast& ray, const JPH::RayCastSettings&, const JPH::SubShapeIDCreator& creator,
                             JPH::CastRayCollector& collector, const JPH::ShapeFilter& filter) const {
        if (!filter.ShouldCollide(this, creator.GetID())) return;
        const float early = collector.GetEarlyOutFraction();
        utils::TileRayHit h;
        if (!utils::tileTableRaycast(*tiles, layout, toCore(ray.mOrigin + centre), toCore(ray.mDirection), std::min(early, 1.0f), h) ||
            !(h.t < early))
            return;
        JPH::RayCastResult result;
        result.mFraction = h.t;
        result.mSubShapeID2 = cellId(creator, h.voxel).GetID();
        collector.AddHit(result);
    }

    void VoxelShape::CollidePoint(JPH::Vec3Arg point, const JPH::SubShapeIDCreator& creator,
                                  JPH::CollidePointCollector& collector, const JPH::ShapeFilter& filter) const {
        if (!filter.ShouldCollide(this, creator.GetID())) return;
        const core::ivec3 v(glm::floor(toCore(point + centre)));
        if (!utils::tileTableVoxel(*tiles, v)) return;
        JPH::CollidePointResult result;
        result.mSubShapeID2 = cellId(creator, v).GetID();
        collector.AddHit(result);
    }

    namespace {
        // A cell's contact: the deepest of its boxes'. Jolt keeps a contact manifold per pair of
        // sub-shape ids, and the boxes of one cell share its id (there are not the bits for each box
        // its own: see the header), so a cell gives at most one, as a mesh's triangle does.
        class DeepestCollector final : public JPH::CollideShapeCollector {
        public:
            void AddHit(const JPH::CollideShapeResult& result) override {
                if (has && !(result.mPenetrationDepth > best.mPenetrationDepth)) return;
                best = result;
                has = true;
            }
            bool                    has = false;
            JPH::CollideShapeResult best;
        };

        // The deepest contact of each pair of ids, passed on at the end: voxels against voxels where
        // one shape's boxes share their cell's id (a coarse shape), so no pair of ids comes twice.
        class PairDeepestCollector final : public JPH::CollideShapeCollector {
        public:
            void AddHit(const JPH::CollideShapeResult& result) override {
                for (JPH::CollideShapeResult& r : results)
                    if (r.mSubShapeID1 == result.mSubShapeID1 && r.mSubShapeID2 == result.mSubShapeID2) {
                        if (result.mPenetrationDepth > r.mPenetrationDepth) r = result;
                        return;
                    }
                results.push_back(result);
            }
            std::vector<JPH::CollideShapeResult> results;
        };
    }

    struct VoxelShapeQueries {
        // A convex (shape 1) against the voxels (shape 2): each box under its bounds, as Jolt collides
        // a convex with a compound's boxes.
        static void collide(const JPH::Shape* shape1, const JPH::Shape* shape2, JPH::Vec3Arg scale1, JPH::Vec3Arg scale2,
                            JPH::Mat44Arg transform1, JPH::Mat44Arg transform2, const JPH::SubShapeIDCreator& creator1,
                            const JPH::SubShapeIDCreator& creator2, const JPH::CollideShapeSettings& settings,
                            JPH::CollideShapeCollector& collector, const JPH::ShapeFilter& filter) {
            JPH_ASSERT(shape2->GetSubType() == VoxelShape::SUBTYPE);
            const auto* voxels = static_cast<const VoxelShape*>(shape2);
            const JPH::Mat44 oneToTwo = transform2.InversedRotationTranslation() * transform1;
            JPH::AABox reach = shape1->GetLocalBounds().Scaled(scale1);
            reach.ExpandBy(JPH::Vec3::sReplicate(settings.mMaxSeparationDistance));
            reach = reach.Transformed(oneToTwo);
            reach.Translate(scale2 * voxels->centre);   // centre-of-mass space to voxel space
            core::ivec3 low, high;
            voxelRange(reach, scale2, low, high);
            std::vector<utils::TileBox>& boxes = scratch();
            voxels->boxesIn(low, high, boxes);
            auto one = [&](const utils::TileBox& b, JPH::CollideShapeCollector& into) {
                CellBox cell(b, voxels->materials[b.slot].GetPtr());
                JPH::CollisionDispatch::sCollideShapeVsShape(
                    shape1, &cell.shape, scale1, scale2, transform1,
                    transform2 * JPH::Mat44::sTranslation(scale2 * (cell.centre - voxels->centre)),
                    creator1, voxels->cellId(creator2, b.low), settings, into, filter);
            };
            if (voxels->cellShift == 0) {   // every box its own id
                for (const utils::TileBox& b : boxes) {
                    if (collector.ShouldEarlyOut()) break;
                    one(b, collector);
                }
                return;
            }
            // Boxes sharing a cell's id: the deepest of their contacts, cell by cell (in cell order).
            const int shift = voxels->cellShift;
            auto cellOf = [&](const utils::TileBox& b) {
                const core::ivec3 c = b.low >> shift;
                return uint32_t(c.x + voxels->cellsPerAxis * (c.y + voxels->cellsPerAxis * c.z));
            };
            std::stable_sort(boxes.begin(), boxes.end(), [&](const utils::TileBox& a, const utils::TileBox& b) { return cellOf(a) < cellOf(b); });
            for (size_t i = 0; i < boxes.size() && !collector.ShouldEarlyOut();) {
                const uint32_t cell = cellOf(boxes[i]);
                DeepestCollector deepest;
                deepest.SetContext(collector.GetContext());
                for (; i < boxes.size() && cellOf(boxes[i]) == cell; i++) one(boxes[i], deepest);
                if (deepest.has) collector.AddHit(deepest.best);
            }
        }

        // A convex swept against the voxels: the boxes under the sweep, nearest first, each cast against
        // until one is hit nearer than any box left could be.
        static void cast(const JPH::ShapeCast& sweep, const JPH::ShapeCastSettings& settings, const JPH::Shape* shape,
                         JPH::Vec3Arg scale, const JPH::ShapeFilter& filter, JPH::Mat44Arg transform2,
                         const JPH::SubShapeIDCreator& creator1, const JPH::SubShapeIDCreator& creator2,
                         JPH::CastShapeCollector& collector) {
            JPH_ASSERT(shape->GetSubType() == VoxelShape::SUBTYPE);
            const auto* voxels = static_cast<const VoxelShape*>(shape);
            const JPH::Vec3 shift = scale * voxels->centre;   // centre-of-mass space to voxel space
            const JPH::AABox start(sweep.mShapeWorldBounds.mMin + shift, sweep.mShapeWorldBounds.mMax + shift);
            JPH::AABox swept = start;
            swept.Encapsulate(JPH::AABox(start.mMin + sweep.mDirection, start.mMax + sweep.mDirection));
            core::ivec3 low, high;
            voxelRange(swept, scale, low, high);
            std::vector<utils::TileBox>& boxes = scratch();
            voxels->boxesIn(low, high, boxes);
            if (boxes.empty()) return;
            // Where the sweep's bounds first reach each box: a slab test of the box grown by them.
            const JPH::Vec3 centre = start.GetCenter(), extent = start.GetExtent();
            std::vector<std::pair<float, uint32_t>> order;
            order.reserve(boxes.size());
            for (uint32_t i = 0; i < boxes.size(); i++) {
                const JPH::Vec3 lo = scale * toJolt(core::vec3(boxes[i].low)) - extent;
                const JPH::Vec3 hi = scale * toJolt(core::vec3(boxes[i].high + core::ivec3(1))) + extent;
                float enter = 0.0f, leave = 1.0f;
                for (int a = 0; a < 3 && enter <= leave; a++) {
                    const float o = centre[a], d = sweep.mDirection[a];
                    if (std::abs(d) < 1e-12f) {
                        if (o < lo[a] || o > hi[a]) enter = 2.0f;
                        continue;
                    }
                    float t0 = (lo[a] - o) / d, t1 = (hi[a] - o) / d;
                    if (t0 > t1) std::swap(t0, t1);
                    enter = std::max(enter, t0);
                    leave = std::min(leave, t1);
                }
                if (enter <= leave) order.push_back({enter, i});
            }
            std::sort(order.begin(), order.end());
            for (const auto& [enter, i] : order) {
                if (collector.ShouldEarlyOut() || enter >= collector.GetPositiveEarlyOutFraction()) break;
                CellBox cell(boxes[i], voxels->materials[boxes[i].slot].GetPtr());
                const JPH::Mat44 local = JPH::Mat44::sTranslation(scale * (cell.centre - voxels->centre));
                JPH::CollisionDispatch::sCastShapeVsShapeLocalSpace(sweep.PostTransformed(local.InversedRotationTranslation()),
                                                                    settings, &cell.shape, scale, filter, transform2 * local, creator1,
                                                                    voxels->cellId(creator2, boxes[i].low), collector);
            }
        }
    };

    struct VoxelVoxelQueries {
        static const VoxelShape* voxels(const JPH::Shape* s) {
            JPH_ASSERT(s->GetSubType() == VoxelShape::SUBTYPE);
            return static_cast<const VoxelShape*>(s);
        }

        // A box of a shape, as an axis-aligned box in another space: where `toSpace` puts the box's
        // middle (of `scale`d voxels, centred on the shape's centre of mass), grown by the box's
        // half-extents turned by it.
        static JPH::AABox placed(const CellBox& cell, const VoxelShape& shape, JPH::Vec3Arg scale, JPH::Mat44Arg toSpace) {
            const JPH::Vec3 middle = toSpace * (scale * (cell.centre - shape.centre));
            const JPH::Vec3 half = scale.Abs() * cell.half;
            const JPH::Vec3 reach(std::abs(toSpace(0, 0)) * half.GetX() + std::abs(toSpace(0, 1)) * half.GetY() + std::abs(toSpace(0, 2)) * half.GetZ(),
                                  std::abs(toSpace(1, 0)) * half.GetX() + std::abs(toSpace(1, 1)) * half.GetY() + std::abs(toSpace(1, 2)) * half.GetZ(),
                                  std::abs(toSpace(2, 0)) * half.GetX() + std::abs(toSpace(2, 1)) * half.GetY() + std::abs(toSpace(2, 2)) * half.GetZ());
            return JPH::AABox(middle - reach, middle + reach);
        }

        // A box of `shape`, in its own centre-of-mass space, scaled.
        static JPH::AABox own(const utils::TileBox& b, const VoxelShape& shape, JPH::Vec3Arg scale) {
            const JPH::Vec3 low = scale * (toJolt(core::vec3(b.low)) - shape.centre);
            const JPH::Vec3 high = scale * (toJolt(core::vec3(b.high + core::ivec3(1))) - shape.centre);
            return JPH::AABox(JPH::Vec3::sMin(low, high), JPH::Vec3::sMax(low, high));
        }

        // Voxels against voxels, as a compound of boxes against another: the other's boxes where the
        // two shapes' bounds meet, fetched once (nothing there, nothing to do); then of the smaller
        // one's boxes there, each against those of the other its bounds reach, one Jolt box against
        // another. Not Teardown's corners and edges -- the plan's design for when this is too slow.
        static void collide(const JPH::Shape* shape1, const JPH::Shape* shape2, JPH::Vec3Arg scale1, JPH::Vec3Arg scale2,
                            JPH::Mat44Arg transform1, JPH::Mat44Arg transform2, const JPH::SubShapeIDCreator& creator1,
                            const JPH::SubShapeIDCreator& creator2, const JPH::CollideShapeSettings& settings,
                            JPH::CollideShapeCollector& collector, const JPH::ShapeFilter& filter) {
            const VoxelShape* a = voxels(shape1);
            const VoxelShape* b = voxels(shape2);
            const float volumeA = a->volume * std::abs(scale1.GetX() * scale1.GetY() * scale1.GetZ());
            const float volumeB = b->volume * std::abs(scale2.GetX() * scale2.GetY() * scale2.GetZ());
            if (volumeA > volumeB) {   // the smaller one's boxes: the other way round
                JPH::CollisionDispatch::sReversedCollideShape(shape1, shape2, scale1, scale2, transform1, transform2, creator1,
                                                              creator2, settings, collector, filter);
                return;
            }
            const float margin = settings.mMaxSeparationDistance;
            // b's boxes where a reaches, in b's space.
            const JPH::Mat44 oneToTwo = transform2.InversedRotationTranslation() * transform1;
            JPH::AABox reachB = a->GetLocalBounds().Scaled(scale1);
            reachB.ExpandBy(JPH::Vec3::sReplicate(margin));
            reachB = reachB.Transformed(oneToTwo);
            reachB.Translate(scale2 * b->centre);
            core::ivec3 low, high;
            voxelRange(reachB, scale2, low, high);
            std::vector<utils::TileBox>& nearby = scratch(1);
            b->boxesIn(low, high, nearby);
            if (nearby.empty()) return;
            // a's boxes where b reaches, in a's space.
            const JPH::Mat44 twoToOne = transform1.InversedRotationTranslation() * transform2;
            JPH::AABox reachA = b->GetLocalBounds().Scaled(scale2);
            reachA.ExpandBy(JPH::Vec3::sReplicate(margin));
            reachA = reachA.Transformed(twoToOne);
            reachA.Translate(scale1 * a->centre);
            voxelRange(reachA, scale1, low, high);
            std::vector<utils::TileBox>& boxes = scratch(0);
            a->boxesIn(low, high, boxes);
            PairDeepestCollector pairs;
            pairs.SetContext(collector.GetContext());
            JPH::CollideShapeCollector& into = a->cellShift == 0 && b->cellShift == 0 ? collector : pairs;
            std::vector<JPH::AABox> nearBounds;
            nearBounds.reserve(nearby.size());
            for (const utils::TileBox& n : nearby) nearBounds.push_back(own(n, *b, scale2));
            for (const utils::TileBox& box : boxes) {
                if (collector.ShouldEarlyOut()) break;
                CellBox cellA(box, a->materials[box.slot].GetPtr());
                JPH::AABox inB = placed(cellA, *a, scale1, oneToTwo);
                inB.ExpandBy(JPH::Vec3::sReplicate(margin));
                const JPH::Mat44 boxTransform = transform1 * JPH::Mat44::sTranslation(scale1 * (cellA.centre - a->centre));
                const JPH::SubShapeIDCreator idA = a->cellId(creator1, box.low);
                for (size_t k = 0; k < nearby.size(); k++) {
                    if (!inB.Overlaps(nearBounds[k])) continue;
                    CellBox cellB(nearby[k], b->materials[nearby[k].slot].GetPtr());
                    JPH::CollisionDispatch::sCollideShapeVsShape(&cellA.shape, &cellB.shape, scale1, scale2, boxTransform,
                                                                 transform2 * JPH::Mat44::sTranslation(scale2 * (cellB.centre - b->centre)),
                                                                 idA, b->cellId(creator2, nearby[k].low), settings, into, filter);
                }
            }
            for (const JPH::CollideShapeResult& r : pairs.results) collector.AddHit(r);
        }

        // Voxels swept against voxels: the other's boxes under the whole sweep, fetched once; then each
        // of the swept shape's boxes, cast against those its own sweep reaches.
        static void cast(const JPH::ShapeCast& sweep, const JPH::ShapeCastSettings& settings, const JPH::Shape* shape,
                         JPH::Vec3Arg scale, const JPH::ShapeFilter& filter, JPH::Mat44Arg transform2,
                         const JPH::SubShapeIDCreator& creator1, const JPH::SubShapeIDCreator& creator2,
                         JPH::CastShapeCollector& collector) {
            const VoxelShape* a = voxels(sweep.mShape);
            const VoxelShape* b = voxels(shape);
            // The sweep is in b's space: its bounds, start to end, are where b's boxes can be met.
            JPH::AABox swept = sweep.mShapeWorldBounds;
            swept.Encapsulate(JPH::AABox(swept.mMin + sweep.mDirection, swept.mMax + sweep.mDirection));
            JPH::AABox inVoxels = swept;
            inVoxels.Translate(scale * b->centre);
            core::ivec3 low, high;
            voxelRange(inVoxels, scale, low, high);
            std::vector<utils::TileBox>& nearby = scratch(1);
            b->boxesIn(low, high, nearby);
            if (nearby.empty()) return;
            std::vector<JPH::AABox> nearBounds;
            nearBounds.reserve(nearby.size());
            for (const utils::TileBox& n : nearby) nearBounds.push_back(own(n, *b, scale));
            std::vector<utils::TileBox>& boxes = scratch(0);
            a->boxesIn(core::ivec3(0), core::ivec3(int(a->tiles->resolution) - 1), boxes);
            for (const utils::TileBox& box : boxes) {
                if (collector.ShouldEarlyOut()) break;
                CellBox cellA(box, a->materials[box.slot].GetPtr());
                JPH::AABox mine = placed(cellA, *a, sweep.mScale, sweep.mCenterOfMassStart);
                mine.Encapsulate(JPH::AABox(mine.mMin + sweep.mDirection, mine.mMax + sweep.mDirection));
                const JPH::ShapeCast one(&cellA.shape, sweep.mScale,
                                         sweep.mCenterOfMassStart * JPH::Mat44::sTranslation(sweep.mScale * (cellA.centre - a->centre)),
                                         sweep.mDirection);
                const JPH::SubShapeIDCreator idA = a->cellId(creator1, box.low);
                for (size_t k = 0; k < nearby.size(); k++) {
                    if (collector.ShouldEarlyOut()) break;
                    if (!mine.Overlaps(nearBounds[k])) continue;
                    CellBox cellB(nearby[k], b->materials[nearby[k].slot].GetPtr());
                    const JPH::Mat44 local = JPH::Mat44::sTranslation(scale * (cellB.centre - b->centre));
                    JPH::CollisionDispatch::sCastShapeVsShapeLocalSpace(one.PostTransformed(local.InversedRotationTranslation()), settings,
                                                                        &cellB.shape, scale, filter, transform2 * local, idA,
                                                                        b->cellId(creator2, nearby[k].low), collector);
                }
            }
        }
    };

    void VoxelShape::sRegister() {
        JPH::ShapeFunctions& f = JPH::ShapeFunctions::sGet(SUBTYPE);
        f.mColor = JPH::Color::sDarkOrange;
        for (JPH::EShapeSubType s : JPH::sConvexSubShapeTypes) {
            JPH::CollisionDispatch::sRegisterCollideShape(s, SUBTYPE, VoxelShapeQueries::collide);
            JPH::CollisionDispatch::sRegisterCastShape(s, SUBTYPE, VoxelShapeQueries::cast);
            JPH::CollisionDispatch::sRegisterCollideShape(SUBTYPE, s, JPH::CollisionDispatch::sReversedCollideShape);
            JPH::CollisionDispatch::sRegisterCastShape(SUBTYPE, s, JPH::CollisionDispatch::sReversedCastShape);
        }
        JPH::CollisionDispatch::sRegisterCollideShape(SUBTYPE, SUBTYPE, VoxelVoxelQueries::collide);
        JPH::CollisionDispatch::sRegisterCastShape(SUBTYPE, SUBTYPE, VoxelVoxelQueries::cast);
    }
}
