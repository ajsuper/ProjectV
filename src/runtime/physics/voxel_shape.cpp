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

        // The boxes of one query, per thread: Jolt collides on several.
        std::vector<utils::TileBox>& scratch() {
            thread_local std::vector<utils::TileBox> boxes;
            boxes.clear();
            return boxes;
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
                           std::array<JPH::RefConst<JPH::PhysicsMaterial>, 256> slotMaterials, JPH::uint idBits)
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
            utils::tileBoxesIn(*tiles, layout, corner, corner + core::ivec3(cellSize() - 1), boxes);
            if ((found = !boxes.empty())) slot = boxes.front().slot;
        }
        const JPH::PhysicsMaterial* m = found ? materials[slot].GetPtr() : nullptr;
        return m ? m : JPH::PhysicsMaterial::sDefault.GetPtr();
    }

    JPH::Vec3 VoxelShape::GetSurfaceNormal(const JPH::SubShapeID& id, JPH::Vec3Arg position) const {
        const core::vec3 p = toCore(position);
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
        utils::tileBoxesIn(*tiles, layout, corner, last, boxes);
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
        cell.shape.GetSupportingFace(JPH::SubShapeID(), direction, scale, transform * JPH::Mat44::sTranslation(scale * cell.centre),
                                     vertices);
    }

    bool VoxelShape::CastRay(const JPH::RayCast& ray, const JPH::SubShapeIDCreator& creator, JPH::RayCastResult& hit) const {
        utils::TileRayHit h;
        if (!utils::tileTableRaycast(*tiles, layout, toCore(ray.mOrigin), toCore(ray.mDirection), hit.mFraction, h) ||
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
        if (!utils::tileTableRaycast(*tiles, layout, toCore(ray.mOrigin), toCore(ray.mDirection), std::min(early, 1.0f), h) ||
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
        const core::ivec3 v(glm::floor(toCore(point)));
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
            core::ivec3 low, high;
            voxelRange(reach.Transformed(oneToTwo), scale2, low, high);
            std::vector<utils::TileBox>& boxes = scratch();
            utils::tileBoxesIn(*voxels->tiles, voxels->layout, low, high, boxes);
            auto one = [&](const utils::TileBox& b, JPH::CollideShapeCollector& into) {
                CellBox cell(b, voxels->materials[b.slot].GetPtr());
                JPH::CollisionDispatch::sCollideShapeVsShape(
                    shape1, &cell.shape, scale1, scale2, transform1, transform2 * JPH::Mat44::sTranslation(scale2 * cell.centre),
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
            const JPH::AABox start = sweep.mShapeWorldBounds;
            JPH::AABox swept = start;
            swept.Encapsulate(JPH::AABox(start.mMin + sweep.mDirection, start.mMax + sweep.mDirection));
            core::ivec3 low, high;
            voxelRange(swept, scale, low, high);
            std::vector<utils::TileBox>& boxes = scratch();
            utils::tileBoxesIn(*voxels->tiles, voxels->layout, low, high, boxes);
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
                const JPH::Mat44 local = JPH::Mat44::sTranslation(scale * cell.centre);
                JPH::CollisionDispatch::sCastShapeVsShapeLocalSpace(sweep.PostTransformed(local.InversedRotationTranslation()),
                                                                    settings, &cell.shape, scale, filter, transform2 * local, creator1,
                                                                    voxels->cellId(creator2, boxes[i].low), collector);
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
    }
}
