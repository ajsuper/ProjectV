#pragma once

// A shape that is its inner shape, except that it works out the inner shape's mass properties (and its
// sub-shape id bits) once.
// Private to src/runtime/physics/ -- it names Jolt types.
//
// Why. Jolt asks every part of a compound for its mass properties whenever the compound is made, and a
// compound sums its own parts' every time it is asked. A voxel shape is a compound of its boxes, and a
// level's world is a compound of every chunk's voxel shape, so making the world's shape summed every
// box in the world: 1.2M on West Point Marina, ~100 ms (counting their id bits walked them all again),
// and all of the cost of re-creating the world's
// body after an edit (docs/plans/voxel-collision-shape.md). Jolt's shapes are final, so the cache is a
// decorator of its own subtype, which collides, casts and answers queries by passing them to the inner
// shape unchanged -- what a body touches, and the sub-shape ids it is told, are the inner shape's.

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>

namespace projv::runtime {
    class MassCachedShape final : public JPH::DecoratedShape {
    public:
        JPH_OVERRIDE_NEW_DELETE

        static constexpr JPH::EShapeSubType SUBTYPE = JPH::EShapeSubType::User2;

        // Once, after JPH::RegisterTypes: how Jolt collides and casts it against every other shape.
        static void sRegister();

        MassCachedShape() : JPH::DecoratedShape(SUBTYPE) {}
        explicit MassCachedShape(const JPH::Shape* inner);

        JPH::MassProperties GetMassProperties() const override { return mass; }
        // Walked down to every leaf too, by every compound made of it (to check its ids fit).
        JPH::uint GetSubShapeIDBitsRecursive() const override { return bits; }

        JPH::AABox GetLocalBounds() const override { return mInnerShape->GetLocalBounds(); }
        JPH::AABox GetWorldSpaceBounds(JPH::Mat44Arg centerOfMassTransform, JPH::Vec3Arg scale) const override {
            return mInnerShape->GetWorldSpaceBounds(centerOfMassTransform, scale);
        }
        using JPH::Shape::GetWorldSpaceBounds;
        float GetInnerRadius() const override { return mInnerShape->GetInnerRadius(); }
        float GetVolume() const override { return mInnerShape->GetVolume(); }
        Stats GetStats() const override { return Stats(sizeof(*this), 0); }

        JPH::TransformedShape GetSubShapeTransformedShape(const JPH::SubShapeID& id, JPH::Vec3Arg positionCOM,
                                                          JPH::QuatArg rotation, JPH::Vec3Arg scale,
                                                          JPH::SubShapeID& remainder) const override;
        JPH::Vec3 GetSurfaceNormal(const JPH::SubShapeID& id, JPH::Vec3Arg localSurfacePosition) const override {
            return mInnerShape->GetSurfaceNormal(id, localSurfacePosition);
        }
        void GetSubmergedVolume(JPH::Mat44Arg centerOfMassTransform, JPH::Vec3Arg scale, const JPH::Plane& surface,
                                float& totalVolume, float& submergedVolume,
                                JPH::Vec3& centerOfBuoyancy JPH_IF_DEBUG_RENDERER(, JPH::RVec3Arg baseOffset)) const override {
            mInnerShape->GetSubmergedVolume(centerOfMassTransform, scale, surface, totalVolume, submergedVolume,
                                            centerOfBuoyancy JPH_IF_DEBUG_RENDERER(, baseOffset));
        }
#ifdef JPH_DEBUG_RENDERER
        void Draw(JPH::DebugRenderer* renderer, JPH::RMat44Arg centerOfMassTransform, JPH::Vec3Arg scale,
                  JPH::ColorArg color, bool useMaterialColors, bool drawWireframe) const override {
            mInnerShape->Draw(renderer, centerOfMassTransform, scale, color, useMaterialColors, drawWireframe);
        }
#endif

        bool CastRay(const JPH::RayCast& ray, const JPH::SubShapeIDCreator& creator, JPH::RayCastResult& hit) const override {
            return mInnerShape->CastRay(ray, creator, hit);
        }
        void CastRay(const JPH::RayCast& ray, const JPH::RayCastSettings& settings, const JPH::SubShapeIDCreator& creator,
                     JPH::CastRayCollector& collector, const JPH::ShapeFilter& filter = {}) const override;
        void CollidePoint(JPH::Vec3Arg point, const JPH::SubShapeIDCreator& creator, JPH::CollidePointCollector& collector,
                          const JPH::ShapeFilter& filter = {}) const override;
        void CollideSoftBodyVertices(JPH::Mat44Arg centerOfMassTransform, JPH::Vec3Arg scale,
                                     const JPH::CollideSoftBodyVertexIterator& vertices, JPH::uint count,
                                     int collidingShapeIndex) const override {
            mInnerShape->CollideSoftBodyVertices(centerOfMassTransform, scale, vertices, count, collidingShapeIndex);
        }
        void CollectTransformedShapes(const JPH::AABox& box, JPH::Vec3Arg positionCOM, JPH::QuatArg rotation,
                                      JPH::Vec3Arg scale, const JPH::SubShapeIDCreator& creator,
                                      JPH::TransformedShapeCollector& collector, const JPH::ShapeFilter& filter) const override;
        void TransformShape(JPH::Mat44Arg centerOfMassTransform, JPH::TransformedShapeCollector& collector) const override {
            mInnerShape->TransformShape(centerOfMassTransform, collector);
        }
        // A compound's, never asked of a leaf (CollectTransformedShapes reaches the leaves first).
        void GetTrianglesStart(GetTrianglesContext&, const JPH::AABox&, JPH::Vec3Arg, JPH::QuatArg, JPH::Vec3Arg) const override {
            JPH_ASSERT(false, "Cannot call on non-leaf shapes, use CollectTransformedShapes to collect the leaves first!");
        }
        int GetTrianglesNext(GetTrianglesContext&, int, JPH::Float3*, const JPH::PhysicsMaterial** = nullptr) const override {
            JPH_ASSERT(false, "Cannot call on non-leaf shapes, use CollectTransformedShapes to collect the leaves first!");
            return 0;
        }

    private:
        JPH::MassProperties mass;
        JPH::uint           bits = 0;
    };
}
