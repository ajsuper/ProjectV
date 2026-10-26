#pragma once

// A chunk's voxels as a Jolt shape: its tile table, read in place (docs/plans/voxel-collision-shape.md,
// Phase 2). Private to src/runtime/physics/ -- it names Jolt types.
//
// Nothing is built when one is made: it holds the blob's table, shared and copy-on-write
// (utils::shareBlobTiles), so the simulation reads the voxels it was made of whatever is edited after.
// What it answers it reads from the table's levels (utils/tile_collision.h):
//   - a convex against it (Jolt's dispatch, both ways round): the solid boxes under the convex's
//     bounds -- a full block or a run of full bricks one box, a partly filled brick's voxels merged --
//     each collided as a Jolt box, so contacts, faces and manifolds are Jolt's own, and internal edges
//     between them are removed as they are between a compound's boxes
//   - a convex cast against it: the boxes under the swept bounds, each cast against
//   - rays (a march through region, block, brick, voxel), points, surface normals (the voxel face's)
//   - materials: each palette slot's physics material
// A shape made for motion (`moving`) also weighs itself -- its boxes' masses at each slot's density,
// summed once -- and its centre of mass is its voxels' centroid; another voxel shape collides with it
// box by box, the one with fewer voxels near the other taking its boxes to the other's (the plan's
// Phase 3). A static one is MustBeStatic, and its centre is its corner.
//
// Its space is voxel units, the chunk's corner at the origin. Jolt hands a shape its queries in
// centre-of-mass space, so each is moved by the centre first (zero for a static shape); a
// ScaledShape gives it its voxel size, as it does a compound of boxes. A sub-shape id names the first
// voxel of the box a contact is on, when the bits it is given hold a voxel index (utils::tileCellShift:
// a 256^3 chunk's takes 24, so a body of up to 256 of them); otherwise a coarser cell -- a body of every
// chunk of a big level (4,140 chunks leave 19 bits: bricks) -- and then a cell's boxes give
// one contact between them, the deepest, since Jolt keeps one contact manifold per pair of ids.

#include <array>
#include <memory>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include "core/math.h"
#include "utils/tile_collision.h"
#include "utils/tile_table.h"

namespace projv::runtime {
    class VoxelShape final : public JPH::Shape {
    public:
        JPH_OVERRIDE_NEW_DELETE

        static constexpr JPH::EShapeSubType SUBTYPE = JPH::EShapeSubType::User1;

        // Once, after JPH::RegisterTypes: how Jolt collides and casts convex shapes against it.
        static void sRegister();

        // `materials[slot]` is each palette slot's material (null: the default, the body's own values).
        // `densities`, given, makes it a shape for motion: what each slot weighs a unit voxel.
        VoxelShape(std::shared_ptr<const utils::TileTable> table, std::array<JPH::RefConst<JPH::PhysicsMaterial>, 256> materials,
                   JPH::uint idBits, const std::array<float, 256>* densities = nullptr);

        const utils::TileTable& table() const { return *tiles; }
        // The voxel a sub-shape id's cell starts at, and log2 of the cell's size (0: one voxel).
        core::ivec3 cellCorner(const JPH::SubShapeID& id, JPH::SubShapeID* remainder = nullptr) const;
        int cellSize() const { return 1 << cellShift; }
        int cellLevel() const { return cellShift; }

        bool MustBeStatic() const override { return !moving; }
        JPH::Vec3 GetCenterOfMass() const override { return centre; }
        JPH::AABox GetLocalBounds() const override { return JPH::AABox(bounds.mMin - centre, bounds.mMax - centre); }
        JPH::uint GetSubShapeIDBitsRecursive() const override { return bits; }
        // The biggest sphere surely inside, for a moving one: its largest box's -- as a compound's is its
        // largest part's. Jolt sweeps a body (continuous collision) when a step moves it more than a
        // share of this, so too small a radius sweeps every piece every step.
        float GetInnerRadius() const override { return innerRadius; }
        JPH::MassProperties GetMassProperties() const override;
        const JPH::PhysicsMaterial* GetMaterial(const JPH::SubShapeID& id) const override;
        JPH::Vec3 GetSurfaceNormal(const JPH::SubShapeID& id, JPH::Vec3Arg localSurfacePosition) const override;
        void GetSupportingFace(const JPH::SubShapeID& id, JPH::Vec3Arg direction, JPH::Vec3Arg scale,
                               JPH::Mat44Arg centerOfMassTransform, SupportingFace& vertices) const override;
        void GetSubmergedVolume(JPH::Mat44Arg, JPH::Vec3Arg, const JPH::Plane&, float& total, float& submerged,
                                JPH::Vec3& centre JPH_IF_DEBUG_RENDERER(, JPH::RVec3Arg)) const override {
            total = submerged = 0.0f;
            centre = JPH::Vec3::sZero();
        }
#ifdef JPH_DEBUG_RENDERER
        void Draw(JPH::DebugRenderer*, JPH::RMat44Arg, JPH::Vec3Arg, JPH::ColorArg, bool, bool) const override {}
#endif
        bool CastRay(const JPH::RayCast& ray, const JPH::SubShapeIDCreator& creator, JPH::RayCastResult& hit) const override;
        void CastRay(const JPH::RayCast& ray, const JPH::RayCastSettings& settings, const JPH::SubShapeIDCreator& creator,
                     JPH::CastRayCollector& collector, const JPH::ShapeFilter& filter = {}) const override;
        void CollidePoint(JPH::Vec3Arg point, const JPH::SubShapeIDCreator& creator, JPH::CollidePointCollector& collector,
                          const JPH::ShapeFilter& filter = {}) const override;
        void CollideSoftBodyVertices(JPH::Mat44Arg, JPH::Vec3Arg, const JPH::CollideSoftBodyVertexIterator&, JPH::uint,
                                     int) const override {}
        void GetTrianglesStart(GetTrianglesContext&, const JPH::AABox&, JPH::Vec3Arg, JPH::QuatArg, JPH::Vec3Arg) const override {}
        int GetTrianglesNext(GetTrianglesContext&, int, JPH::Float3*, const JPH::PhysicsMaterial** = nullptr) const override {
            return 0;
        }
        Stats GetStats() const override { return Stats(sizeof(*this), 0); }
        float GetVolume() const override { return volume; }

    private:
        friend struct VoxelShapeQueries;
        friend struct VoxelVoxelQueries;
        JPH::SubShapeIDCreator cellId(const JPH::SubShapeIDCreator& creator, core::ivec3 voxel) const;
        // The boxes reaching into [low, high]: its greedy ones when it has them, else the table's.
        void boxesIn(core::ivec3 low, core::ivec3 high, std::vector<utils::TileBox>& out) const;

        std::shared_ptr<const utils::TileTable>               tiles;
        utils::TileLayout                                     layout;
        std::array<JPH::RefConst<JPH::PhysicsMaterial>, 256>  materials;
        JPH::AABox                                            bounds;          // in voxel space
        bool                                                  moving = false;
        JPH::Vec3                                             centre = JPH::Vec3::sZero();   // of mass, in voxel space
        JPH::MassProperties                                   mass;            // about the centre (moving)
        float                                                 innerRadius = 0.0f;
        // A moving one's voxels merged across bricks within each block (utils::tileBlockBoxes), made
        // once: what it collides by, looked up block by block. Empty (and tileBoxesIn instead) for a
        // static one.
        std::vector<utils::TileBox>                           greedy;
        std::vector<uint32_t>                                 greedyStarts;
        float                                                 volume = 0.0f;
        int                                                   cellShift = 0;   // ids name cells of 2^cellShift voxels a side
        int                                                   cellsPerAxis = 1;
        JPH::uint                                             bits = 0;
    };
}
