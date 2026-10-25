#include "mass_cached_shape.h"

#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

namespace projv::runtime {
    MassCachedShape::MassCachedShape(const JPH::Shape* inner)
        : JPH::DecoratedShape(SUBTYPE, inner), mass(inner->GetMassProperties()), bits(inner->GetSubShapeIDBitsRecursive()) {}

    JPH::TransformedShape MassCachedShape::GetSubShapeTransformedShape(const JPH::SubShapeID& id, JPH::Vec3Arg positionCOM,
                                                                       JPH::QuatArg rotation, JPH::Vec3Arg scale,
                                                                       JPH::SubShapeID& remainder) const {
        // Takes no bits of the id: what it names is the inner shape's.
        remainder = id;
        JPH::TransformedShape ts(JPH::RVec3(positionCOM), rotation, mInnerShape, JPH::BodyID());
        ts.SetShapeScale(scale);
        return ts;
    }

    void MassCachedShape::CastRay(const JPH::RayCast& ray, const JPH::RayCastSettings& settings,
                                  const JPH::SubShapeIDCreator& creator, JPH::CastRayCollector& collector,
                                  const JPH::ShapeFilter& filter) const {
        if (!filter.ShouldCollide(this, creator.GetID())) return;
        mInnerShape->CastRay(ray, settings, creator, collector, filter);
    }

    void MassCachedShape::CollidePoint(JPH::Vec3Arg point, const JPH::SubShapeIDCreator& creator,
                                       JPH::CollidePointCollector& collector, const JPH::ShapeFilter& filter) const {
        if (!filter.ShouldCollide(this, creator.GetID())) return;
        mInnerShape->CollidePoint(point, creator, collector, filter);
    }

    void MassCachedShape::CollectTransformedShapes(const JPH::AABox& box, JPH::Vec3Arg positionCOM, JPH::QuatArg rotation,
                                                   JPH::Vec3Arg scale, const JPH::SubShapeIDCreator& creator,
                                                   JPH::TransformedShapeCollector& collector,
                                                   const JPH::ShapeFilter& filter) const {
        if (!filter.ShouldCollide(this, creator.GetID())) return;
        mInnerShape->CollectTransformedShapes(box, positionCOM, rotation, scale, creator, collector, filter);
    }

    namespace {
        const JPH::Shape* innerOf(const JPH::Shape* shape) {
            JPH_ASSERT(shape->GetSubType() == MassCachedShape::SUBTYPE);
            return static_cast<const MassCachedShape*>(shape)->GetInnerShape();
        }

        void collideCachedVsShape(const JPH::Shape* a, const JPH::Shape* b, JPH::Vec3Arg scaleA, JPH::Vec3Arg scaleB,
                                  JPH::Mat44Arg transformA, JPH::Mat44Arg transformB, const JPH::SubShapeIDCreator& creatorA,
                                  const JPH::SubShapeIDCreator& creatorB, const JPH::CollideShapeSettings& settings,
                                  JPH::CollideShapeCollector& collector, const JPH::ShapeFilter& filter) {
            JPH::CollisionDispatch::sCollideShapeVsShape(innerOf(a), b, scaleA, scaleB, transformA, transformB, creatorA,
                                                         creatorB, settings, collector, filter);
        }

        void collideShapeVsCached(const JPH::Shape* a, const JPH::Shape* b, JPH::Vec3Arg scaleA, JPH::Vec3Arg scaleB,
                                  JPH::Mat44Arg transformA, JPH::Mat44Arg transformB, const JPH::SubShapeIDCreator& creatorA,
                                  const JPH::SubShapeIDCreator& creatorB, const JPH::CollideShapeSettings& settings,
                                  JPH::CollideShapeCollector& collector, const JPH::ShapeFilter& filter) {
            JPH::CollisionDispatch::sCollideShapeVsShape(a, innerOf(b), scaleA, scaleB, transformA, transformB, creatorA,
                                                         creatorB, settings, collector, filter);
        }

        void castCachedVsShape(const JPH::ShapeCast& cast, const JPH::ShapeCastSettings& settings, const JPH::Shape* shape,
                               JPH::Vec3Arg scale, const JPH::ShapeFilter& filter, JPH::Mat44Arg transform,
                               const JPH::SubShapeIDCreator& creatorA, const JPH::SubShapeIDCreator& creatorB,
                               JPH::CastShapeCollector& collector) {
            JPH::ShapeCast inner(innerOf(cast.mShape), cast.mScale, cast.mCenterOfMassStart, cast.mDirection);
            JPH::CollisionDispatch::sCastShapeVsShapeLocalSpace(inner, settings, shape, scale, filter, transform, creatorA,
                                                                creatorB, collector);
        }

        void castShapeVsCached(const JPH::ShapeCast& cast, const JPH::ShapeCastSettings& settings, const JPH::Shape* shape,
                               JPH::Vec3Arg scale, const JPH::ShapeFilter& filter, JPH::Mat44Arg transform,
                               const JPH::SubShapeIDCreator& creatorA, const JPH::SubShapeIDCreator& creatorB,
                               JPH::CastShapeCollector& collector) {
            JPH::CollisionDispatch::sCastShapeVsShapeLocalSpace(cast, settings, innerOf(shape), scale, filter, transform,
                                                                creatorA, creatorB, collector);
        }
    }

    void MassCachedShape::sRegister() {
        JPH::ShapeFunctions& f = JPH::ShapeFunctions::sGet(SUBTYPE);
        f.mConstruct = []() -> JPH::Shape* { return new MassCachedShape; };
        f.mColor = JPH::Color::sCyan;
        for (JPH::EShapeSubType s : JPH::sAllSubShapeTypes) {
            JPH::CollisionDispatch::sRegisterCollideShape(SUBTYPE, s, collideCachedVsShape);
            JPH::CollisionDispatch::sRegisterCollideShape(s, SUBTYPE, collideShapeVsCached);
            JPH::CollisionDispatch::sRegisterCastShape(SUBTYPE, s, castCachedVsShape);
            JPH::CollisionDispatch::sRegisterCastShape(s, SUBTYPE, castShapeVsCached);
        }
    }
}
