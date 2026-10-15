// The physics core over Jolt. The only translation unit family that includes Jolt (the `layering`
// test holds every other file to that), compiled as its own object library so Jolt's SIMD flags and
// configuration defines reach exactly these files. See physics_world.h for the guarantees.

#include "runtime/physics/physics_world.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <tuple>
#include <set>
#include <mutex>
#include <string>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include "core/log.h"
#include "job_pool.h"

namespace projv::runtime {
    struct CollisionShape::Native {
        JPH::RefConst<JPH::Shape> shape;
    };

    namespace {
        // ---- Jolt's process-wide setup ----------------------------------------------------------
        // Jolt keeps a type factory and an allocator hook per process. Every PhysicsWorld holds a
        // reference; the last one out tears it down, so a test that makes and drops worlds leaves
        // nothing behind.

        void joltTrace(const char* format, ...) {
            char buffer[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            core::trace("jolt: {}", buffer);
        }

#ifdef JPH_ENABLE_ASSERTS
        // A failed Jolt assert is a misuse of Jolt by this file, so it is fatal in the builds that
        // have them: logged with its location, then the breakpoint Jolt raises.
        bool joltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line) {
            core::error("jolt assert failed: {} ({}) at {}:{}", expression, message ? message : "", file, line);
            return true;
        }
#endif

        std::mutex joltMutex;
        int        joltUsers = 0;

        void acquireJolt() {
            std::lock_guard lock(joltMutex);
            if (joltUsers++ > 0) return;
            JPH::RegisterDefaultAllocator();
            JPH::Trace = joltTrace;
            JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssertFailed;)
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }

        void releaseJolt() {
            std::lock_guard lock(joltMutex);
            if (--joltUsers > 0) return;
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }

        // ---- Layers -------------------------------------------------------------------------------
        // Two broadphase trees, the split Jolt recommends: one for what never moves (rebuilt rarely)
        // and one for everything else.
        namespace bp {
            constexpr JPH::BroadPhaseLayer NonMoving(0);
            constexpr JPH::BroadPhaseLayer Moving(1);
            constexpr JPH::uint            Count = 2;
        }

        PhysicsLayer layerOf(JPH::ObjectLayer layer) { return static_cast<PhysicsLayer>(layer); }

        class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
        public:
            JPH::uint GetNumBroadPhaseLayers() const override { return bp::Count; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
                return layerOf(layer) == PhysicsLayer::Static ? bp::NonMoving : bp::Moving;
            }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
                return layer == bp::NonMoving ? "non-moving" : "moving";
            }
#endif
        };

        class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
        public:
            bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer tree) const override {
                // Static meets only the moving tree; everything else meets both. Finer rules are
                // the object-pair filter's.
                if (layerOf(layer) == PhysicsLayer::Static) return tree == bp::Moving;
                return true;
            }
        };

        class ObjectPairs final : public JPH::ObjectLayerPairFilter {
        public:
            bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
                return layersCollide(layerOf(a), layerOf(b));
            }
        };

        // ---- Conversions --------------------------------------------------------------------------
        JPH::Vec3  toJolt(core::vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
        JPH::RVec3 toJoltR(core::vec3 v) { return JPH::RVec3(v.x, v.y, v.z); }
        JPH::Quat  toJolt(core::quat q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
        core::vec3 fromJolt(JPH::Vec3 v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
#ifdef JPH_DOUBLE_PRECISION
        core::vec3 fromJolt(JPH::RVec3 v) {
            return {float(v.GetX()), float(v.GetY()), float(v.GetZ())};
        }
#endif
        core::quat fromJolt(JPH::Quat q) { return core::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

        bool finite(core::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        bool finite(core::quat q) {
            return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
        }

        // The shape a description asks for, or null with the reason logged.
        JPH::ShapeRefC makeShape(const PhysicsShape& shape, float density = 1000.0f);

        // Shapes are made at Jolt's default density and the body's mass set from PhysicsShape::volume
        // instead (createBody): a voxel shape is shared by bodies of different densities, and one
        // rule for every kind is simpler than two. The exception is a body with voxel materials,
        // whose mass is the shape's own: its primitives are made at `density`, the body's.
        JPH::ShapeRefC makeCompound(const PhysicsShape& shape, float density) {
            if (shape.parts.empty()) {
                core::warn("physics: refused a compound with no parts");
                return nullptr;
            }
            std::vector<JPH::ShapeRefC> made;
            made.reserve(shape.parts.size());
            for (const PhysicsShapePart& part : shape.parts) {
                if (!finite(part.position) || !finite(part.rotation)) {
                    core::warn("physics: refused a compound part with a non-finite placement");
                    return nullptr;
                }
                JPH::ShapeRefC child = makeShape(part.shape, density);
                if (!child) return nullptr;
                made.push_back(child);
            }
            auto rotationOf = [](const PhysicsShapePart& part) {
                return toJolt(glm::normalize(part.rotation));
            };
            if (made.size() == 1) {
                const PhysicsShapePart& part = shape.parts[0];
                if (part.position == core::vec3(0.0f) && part.rotation == core::quat(1, 0, 0, 0)) return made[0];
                return new JPH::RotatedTranslatedShape(toJolt(part.position), rotationOf(part), made[0]);
            }
            JPH::StaticCompoundShapeSettings compound;
            for (size_t i = 0; i < made.size(); i++)
                compound.AddShape(toJolt(shape.parts[i].position), rotationOf(shape.parts[i]), made[i], uint32_t(i));
            JPH::Shape::ShapeResult result = compound.Create();
            if (result.HasError()) {
                core::warn("physics: compound shape failed: {}", result.GetError().c_str());
                return nullptr;
            }
            return result.Get();
        }

        // The shape a description asks for, or null with the reason logged.
        JPH::ShapeRefC makeShape(const PhysicsShape& shape, float density) {
            JPH::Shape::ShapeResult result;
            switch (shape.kind) {
                case PhysicsShape::Kind::Box: {
                    core::vec3 h = shape.halfExtents;
                    if (!finite(h) || h.x <= 0.0f || h.y <= 0.0f || h.z <= 0.0f) {
                        core::warn("physics: refused a box with half extents ({}, {}, {})", h.x, h.y, h.z);
                        return nullptr;
                    }
                    // Jolt rounds a box's edges by its convex radius, which may not exceed the
                    // smallest half extent; a thin box gets a proportionally smaller one.
                    float smallest = std::min({h.x, h.y, h.z});
                    float convexRadius = std::min(JPH::cDefaultConvexRadius, 0.5f * smallest);
                    JPH::BoxShapeSettings box(toJolt(h), convexRadius);
                    box.SetDensity(density);
                    result = box.Create();
                    break;
                }
                case PhysicsShape::Kind::Sphere: {
                    if (!std::isfinite(shape.radius) || shape.radius <= 0.0f) {
                        core::warn("physics: refused a sphere with radius {}", shape.radius);
                        return nullptr;
                    }
                    JPH::SphereShapeSettings sphere(shape.radius);
                    sphere.SetDensity(density);
                    result = sphere.Create();
                    break;
                }
                case PhysicsShape::Kind::Capsule: {
                    if (!std::isfinite(shape.radius) || !std::isfinite(shape.halfHeight) ||
                        shape.radius <= 0.0f || shape.halfHeight <= 0.0f) {
                        core::warn("physics: refused a capsule with half height {} and radius {}",
                                   shape.halfHeight, shape.radius);
                        return nullptr;
                    }
                    JPH::CapsuleShapeSettings capsule(shape.halfHeight, shape.radius);
                    capsule.SetDensity(density);
                    result = capsule.Create();
                    break;
                }
                case PhysicsShape::Kind::Voxels: {
                    if (!shape.voxels || !shape.voxels->native || !shape.voxels->native->shape) {
                        core::warn("physics: refused a voxel shape that is empty");
                        return nullptr;
                    }
                    if (!std::isfinite(shape.voxelSize) || shape.voxelSize <= 0.0f) {
                        core::warn("physics: refused a voxel shape with voxel size {}", shape.voxelSize);
                        return nullptr;
                    }
                    const JPH::Shape* unit = shape.voxels->native->shape.GetPtr();
                    if (shape.voxelSize == 1.0f) return unit;
                    // Scaled, not rebuilt: the unit shape stays shared by every body of any size.
                    return new JPH::ScaledShape(unit, JPH::Vec3::sReplicate(shape.voxelSize));
                }
                case PhysicsShape::Kind::Compound:
                    return makeCompound(shape, density);
            }
            if (result.HasError()) {
                core::warn("physics: shape creation failed: {}", result.GetError().c_str());
                return nullptr;
            }
            return result.Get();
        }

        bool containsVoxels(const PhysicsShape& shape) {
            if (shape.kind == PhysicsShape::Kind::Voxels) return true;
            for (const PhysicsShapePart& part : shape.parts)
                if (containsVoxels(part.shape)) return true;
            return false;
        }

        bool containsMaterials(const PhysicsShape& shape) {
            if (shape.kind == PhysicsShape::Kind::Voxels) return shape.voxels && shape.voxels->hasMaterials;
            for (const PhysicsShapePart& part : shape.parts)
                if (containsMaterials(part.shape)) return true;
            return false;
        }

        // ---- Materials ----------------------------------------------------------------------------
        // A voxel piece's Jolt material: only which entry of the table it is. The values are read
        // from the table at each contact, so redefining a material changes every contact at once.
        class VoxelMaterial final : public JPH::PhysicsMaterial {
        public:
            explicit VoxelMaterial(uint16_t index) : index(index) {}
            const uint16_t index;
        };

        // Each side of a contact is the material of the piece touched, or the body's own values where
        // the piece has none; combined as Jolt combines bodies (friction the geometric mean,
        // restitution the larger), so a world without materials behaves exactly as before. Called
        // from the solver's threads; it only reads the table, which changes only between steps.
        class MaterialContacts final : public JPH::ContactListener {
        public:
            const std::vector<PhysicsMaterial>* table = nullptr;
            bool any = false;   // a material is defined: there is something to look up
            void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                                JPH::ContactSettings& settings) override {
                combine(a, b, manifold, settings);
            }
            void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                                    JPH::ContactSettings& settings) override {
                combine(a, b, manifold, settings);
            }
        private:
            static const VoxelMaterial* ours(const JPH::PhysicsMaterial* m) {
                // Every material other than Jolt's default is one of ours: nothing else makes one.
                return m && m != JPH::PhysicsMaterial::sDefault.GetPtr() ? static_cast<const VoxelMaterial*>(m) : nullptr;
            }
            void combine(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                         JPH::ContactSettings& settings) const {
                if (!any) return;
                const VoxelMaterial* ma = ours(a.GetShape()->GetMaterial(manifold.mSubShapeID1));
                const VoxelMaterial* mb = ours(b.GetShape()->GetMaterial(manifold.mSubShapeID2));
                if (!ma && !mb) return;
                float fa = ma ? (*table)[ma->index].friction : a.GetFriction();
                float fb = mb ? (*table)[mb->index].friction : b.GetFriction();
                float ra = ma ? (*table)[ma->index].restitution : a.GetRestitution();
                float rb = mb ? (*table)[mb->index].restitution : b.GetRestitution();
                settings.mCombinedFriction = std::sqrt(fa * fb);
                settings.mCombinedRestitution = std::max(ra, rb);
            }
        };

        JPH::EMotionType toJolt(MotionType motion) {
            switch (motion) {
                case MotionType::Static:    return JPH::EMotionType::Static;
                case MotionType::Kinematic: return JPH::EMotionType::Kinematic;
                case MotionType::Dynamic:   break;
            }
            return JPH::EMotionType::Dynamic;
        }

        // FNV-1a, 64-bit. Not cryptographic; only has to make different states hash differently.
        struct Hasher {
            uint64_t value = 1469598103934665603ull;
            void bytes(const void* data, size_t size) {
                const auto* p = static_cast<const uint8_t*>(data);
                for (size_t i = 0; i < size; i++) {
                    value ^= p[i];
                    value *= 1099511628211ull;
                }
            }
            template <typename T> void add(const T& v) { bytes(&v, sizeof(v)); }
            void add(JPH::Vec3 v) { add(v.GetX()); add(v.GetY()); add(v.GetZ()); }
            void add(JPH::Quat q) { add(q.GetX()); add(q.GetY()); add(q.GetZ()); add(q.GetW()); }
        };

        // Snapshot layout: a header that lets restoreState check, before touching anything, that the
        // snapshot is one and fits this world; then Jolt's own bytes.
        constexpr uint32_t SNAPSHOT_MAGIC = 0x50564a50u;   // "PJVP"
        constexpr uint32_t SNAPSHOT_VERSION = 5;   // 2: rest damping state per body; 3: characters; 4: joints; 5: motor modes

        template <typename T> void put(std::vector<uint8_t>& out, const T& v) {
            const auto* p = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), p, p + sizeof(T));
        }
        template <typename T> bool get(const std::vector<uint8_t>& in, size_t& at, T& v) {
            if (at > in.size() || in.size() - at < sizeof(T)) return false;
            std::memcpy(&v, in.data() + at, sizeof(T));
            at += sizeof(T);
            return true;
        }
    }

    bool layersCollide(PhysicsLayer a, PhysicsLayer b) {
        if (a > b) std::swap(a, b);   // the matrix is symmetric; look up with a <= b
        switch (a) {
            case PhysicsLayer::Static:    return b != PhysicsLayer::Static && b != PhysicsLayer::Sensor;
            case PhysicsLayer::Moving:    return true;
            case PhysicsLayer::Debris:    return b != PhysicsLayer::Debris && b != PhysicsLayer::Sensor;
            case PhysicsLayer::Sensor:    return b == PhysicsLayer::Character;
            case PhysicsLayer::Character: return b == PhysicsLayer::Character;
            case PhysicsLayer::Count:     break;
        }
        return false;
    }

    float PhysicsShape::volume() const {
        constexpr float pi = 3.14159265358979f;
        switch (kind) {
            case Kind::Box:     return 8.0f * halfExtents.x * halfExtents.y * halfExtents.z;
            case Kind::Sphere:  return 4.0f / 3.0f * pi * radius * radius * radius;
            case Kind::Capsule: return pi * radius * radius * (2.0f * halfHeight + 4.0f / 3.0f * radius);
            case Kind::Voxels:  return voxels ? voxels->volume * voxelSize * voxelSize * voxelSize : 0.0f;
            case Kind::Compound: {
                float total = 0.0f;
                for (const PhysicsShapePart& part : parts) total += part.shape.volume();
                return total;
            }
        }
        return 0.0f;
    }

    // Bodies joined by a joint that does not let them collide (JointDesc::collideConnected false, the
    // default): every body's collision group is its own body index, and this refuses the pairs that
    // such joints join -- a car's wheels and its chassis, the links of a chain. Counted, as two joints
    // can join the same pair. Read from the solver's threads during a step, changed only between steps.
    class JointedPairs final : public JPH::GroupFilter {
    public:
        static uint64_t key(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }
        void add(uint32_t a, uint32_t b) { pairs[key(a, b)]++; }
        void remove(uint32_t a, uint32_t b) {
            auto it = pairs.find(key(a, b));
            if (it != pairs.end() && --it->second == 0) pairs.erase(it);
        }
        bool CanCollide(const JPH::CollisionGroup& one, const JPH::CollisionGroup& two) const override {
            if (pairs.empty()) return true;
            return pairs.find(key(one.GetGroupID(), two.GetGroupID())) == pairs.end();
        }
    private:
        std::map<uint64_t, int> pairs;
    };

    // ---- Recording ---------------------------------------------------------------------------------
    // A log is a header (magic, version, the settings) and then one record per call that changes the
    // simulation, in the order made: what a fresh world needs to be given to end up the same.
    namespace {
        constexpr uint32_t LOG_MAGIC = 0x4c564a50u;   // "PJVL"
        constexpr uint32_t LOG_VERSION = 4;   // 2: joint motor modes; 3: collideConnected; 4: materials

        enum class Op : uint8_t {
            Create = 1, Destroy, Impulse, AngularImpulse, AddVelocity, SetVelocity, SetPose, MoveKinematic,
            Gravity, Step, Restore, CreateCharacter, DestroyCharacter, CharacterInput, CharacterPosition,
            CreateJoint, DestroyJoint, JointMotor, SetMaterial
        };

        void writeSettings(std::vector<uint8_t>& out, const PhysicsSettings& s) {
            put(out, s.maxBodies); put(out, s.maxBodyPairs); put(out, s.maxContactConstraints);
            put(out, s.gravity); put(out, s.worldMin); put(out, s.worldMax);
            put(out, s.maxLinearSpeed); put(out, s.maxAngularSpeed); put(out, s.minMass); put(out, s.minGyrationRadius);
            put(out, s.restRadius); put(out, s.restAngle); put(out, s.restSeconds); put(out, s.restDamping);
            put(out, int32_t(s.velocitySteps)); put(out, int32_t(s.positionSteps));
        }
        bool readSettings(const std::vector<uint8_t>& in, size_t& at, PhysicsSettings& s) {
            int32_t velocity = 0, position = 0;
            bool ok = get(in, at, s.maxBodies) && get(in, at, s.maxBodyPairs) && get(in, at, s.maxContactConstraints) &&
                      get(in, at, s.gravity) && get(in, at, s.worldMin) && get(in, at, s.worldMax) &&
                      get(in, at, s.maxLinearSpeed) && get(in, at, s.maxAngularSpeed) && get(in, at, s.minMass) &&
                      get(in, at, s.minGyrationRadius) && get(in, at, s.restRadius) && get(in, at, s.restAngle) &&
                      get(in, at, s.restSeconds) && get(in, at, s.restDamping) && get(in, at, velocity) &&
                      get(in, at, position);
            s.velocitySteps = velocity;
            s.positionSteps = position;
            return ok;
        }

        void writeShape(std::vector<uint8_t>& out, const PhysicsShape& shape) {
            put(out, uint8_t(shape.kind));
            switch (shape.kind) {
                case PhysicsShape::Kind::Box:     put(out, shape.halfExtents); break;
                case PhysicsShape::Kind::Sphere:  put(out, shape.radius); break;
                case PhysicsShape::Kind::Capsule: put(out, shape.halfHeight); put(out, shape.radius); break;
                case PhysicsShape::Kind::Voxels: {
                    put(out, shape.voxelSize);
                    const utils::CollisionPieces* pieces = shape.voxels ? shape.voxels->source.get() : nullptr;
                    put(out, uint8_t(pieces != nullptr));
                    if (!pieces) break;
                    put(out, shape.voxels->otherDensity);
                    put(out, uint8_t(pieces->fallback));
                    put(out, uint32_t(pieces->pieces.size()));
                    for (const utils::CollisionPiece& p : pieces->pieces) {
                        put(out, p.min); put(out, p.max); put(out, p.voxelMin); put(out, p.voxelMax); put(out, p.material);
                    }
                    break;
                }
                case PhysicsShape::Kind::Compound:
                    put(out, uint32_t(shape.parts.size()));
                    for (const PhysicsShapePart& part : shape.parts) {
                        writeShape(out, part.shape);
                        put(out, part.position);
                        put(out, part.rotation);
                    }
                    break;
            }
        }

        template <typename BuildVoxels>
        bool readShape(const std::vector<uint8_t>& in, size_t& at, PhysicsShape& shape, BuildVoxels& build, int depth = 0) {
            uint8_t kind = 0;
            if (depth > 64 || !get(in, at, kind) || kind > uint8_t(PhysicsShape::Kind::Compound)) return false;
            shape.kind = PhysicsShape::Kind(kind);
            switch (shape.kind) {
                case PhysicsShape::Kind::Box:     return get(in, at, shape.halfExtents);
                case PhysicsShape::Kind::Sphere:  return get(in, at, shape.radius);
                case PhysicsShape::Kind::Capsule: return get(in, at, shape.halfHeight) && get(in, at, shape.radius);
                case PhysicsShape::Kind::Voxels: {
                    uint8_t has = 0;
                    if (!get(in, at, shape.voxelSize) || !get(in, at, has)) return false;
                    if (!has) return true;
                    utils::CollisionPieces pieces;
                    uint8_t fallback = 0;
                    uint32_t count = 0;
                    float otherDensity = 0.0f;
                    if (!get(in, at, otherDensity) || !get(in, at, fallback) || !get(in, at, count) || count > (1u << 24))
                        return false;
                    pieces.fallback = utils::CollisionFallback(fallback);
                    pieces.pieces.resize(count);
                    for (utils::CollisionPiece& p : pieces.pieces)
                        if (!get(in, at, p.min) || !get(in, at, p.max) || !get(in, at, p.voxelMin) || !get(in, at, p.voxelMax) ||
                            !get(in, at, p.material))
                            return false;
                    shape.voxels = build(pieces, otherDensity);
                    return true;
                }
                case PhysicsShape::Kind::Compound: {
                    uint32_t count = 0;
                    if (!get(in, at, count) || count > (1u << 20)) return false;
                    shape.parts.resize(count);
                    for (PhysicsShapePart& part : shape.parts)
                        if (!readShape(in, at, part.shape, build, depth + 1) || !get(in, at, part.position) ||
                            !get(in, at, part.rotation))
                            return false;
                    return true;
                }
            }
            return false;
        }

        void writeDesc(std::vector<uint8_t>& out, const BodyDesc& d) {
            writeShape(out, d.shape);
            put(out, uint8_t(d.motion)); put(out, uint8_t(d.layer));
            put(out, d.position); put(out, d.rotation); put(out, d.linearVelocity); put(out, d.angularVelocity);
            put(out, d.density); put(out, d.mass); put(out, d.friction); put(out, d.restitution);
            put(out, d.linearDamping); put(out, d.angularDamping); put(out, d.gravityFactor);
            put(out, uint8_t(d.continuous));
        }

        template <typename BuildVoxels>
        bool readDesc(const std::vector<uint8_t>& in, size_t& at, BodyDesc& d, BuildVoxels& build) {
            uint8_t motion = 0, layer = 0, continuous = 0;
            bool ok = readShape(in, at, d.shape, build) && get(in, at, motion) && get(in, at, layer) &&
                      get(in, at, d.position) && get(in, at, d.rotation) && get(in, at, d.linearVelocity) &&
                      get(in, at, d.angularVelocity) && get(in, at, d.density) && get(in, at, d.mass) &&
                      get(in, at, d.friction) && get(in, at, d.restitution) && get(in, at, d.linearDamping) &&
                      get(in, at, d.angularDamping) && get(in, at, d.gravityFactor) && get(in, at, continuous);
            d.motion = MotionType(motion);
            d.layer = PhysicsLayer(layer);
            d.continuous = continuous != 0;
            return ok;
        }
    }

    struct PhysicsWorld::Impl {
        // Declared in the order they must be built: the layer objects outlive the system that
        // points at them, and the allocators outlive every step.
        BroadPhaseLayers   broadPhaseLayers;
        ObjectVsBroadPhase objectVsBroadPhase;
        ObjectPairs        objectPairs;
        // Starts at 32 MB and falls back to malloc past it, rather than aborting mid-step the way
        // the fixed-block allocator does when a scene outgrows it.
        std::unique_ptr<JPH::TempAllocatorImplWithMallocFallback> tempAllocator;
        // A thread pool, or the calling thread alone. Jolt's results do not depend on which, or on
        // the thread count: "the same results with any number of threads" is a test, not a hope.
        std::unique_ptr<JPH::JobSystem> jobSystem;
        JPH::PhysicsSystem system;
        PhysicsSettings    settings;
        PhysicsStats       stats;
        uint64_t           tick = 0;

        // ---- Guards ----
        // Per body index: the pose after the last step it came through finite (or its creation
        // pose), what a non-finite body is put back to; and whether it is outside the world now.
        struct GoodPose { JPH::RVec3 position; JPH::Quat rotation; };
        std::vector<GoodPose> lastGood;
        // Rest damping (PhysicsSettings::restSeconds): where each awake body was when it last
        // moved appreciably, and how long it has stayed there since. Part of the snapshot, since it
        // changes what the next steps do.
        struct Rest { JPH::RVec3 position; JPH::Quat rotation; float seconds; };
        std::vector<Rest>     rest;
        std::vector<uint8_t>  outside;
        std::vector<BodyId>   leftWorld;
        std::set<uint32_t>    faultInjected;      // corruptBodyForTesting
        bool                  warnedCapacity = false;

        // ---- Characters ----
        // Slots, reused lowest first; an id is the slot and its generation, so a stale one fails.
        struct Character {
            JPH::Ref<JPH::CharacterVirtual> controller;
            CharacterDesc desc;
            core::vec3    input{0.0f};
            bool          jump = false;
            float         jumpSpeed = 5.0f;
            uint16_t      generation = 0;
        };
        std::vector<Character> characters;
        Character* character(CharacterId id) {
            uint32_t slot = id.value & 0xffffu;
            if (!id.valid() || slot >= characters.size()) return nullptr;
            Character& c = characters[slot];
            return c.controller && c.generation == uint16_t(id.value >> 16) ? &c : nullptr;
        }
        const Character* character(CharacterId id) const { return const_cast<Impl*>(this)->character(id); }

        // ---- Joints ----
        struct Joint {
            JPH::Ref<JPH::TwoBodyConstraint> constraint;
            JointDesc  desc;
            core::vec3 localA{0.0f}, localB{0.0f};   // the anchors in each body's frame (b: world if none)
            float      force = 0.0f, torque = 0.0f;   // held in the last step
            uint16_t   generation = 0;
        };
        std::vector<Joint>   joints;
        std::vector<PhysicsWorld::BrokenJoint> brokenJoints;
        JPH::Ref<JointedPairs> jointedPairs = new JointedPairs;
        // The pair a joint keeps from colliding, if any.
        void separate(const Joint& j, bool on) {
            if (j.desc.collideConnected || !j.desc.b.valid()) return;
            uint32_t a = JPH::BodyID(j.desc.a.value).GetIndex(), b = JPH::BodyID(j.desc.b.value).GetIndex();
            if (on) jointedPairs->add(a, b);
            else jointedPairs->remove(a, b);
        }
        Joint* joint(JointId id) {
            uint32_t slot = id.value & 0xffffu;
            if (!id.valid() || slot >= joints.size()) return nullptr;
            Joint& j = joints[slot];
            return j.constraint && j.generation == uint16_t(id.value >> 16) ? &j : nullptr;
        }
        const Joint* joint(JointId id) const { return const_cast<Impl*>(this)->joint(id); }

        // ---- Recording (PhysicsSettings::record) ----
        bool                  recording = false;
        std::vector<uint8_t>  log;
        uint64_t              loggedSteps = 0;
        void op(Op o) { log.push_back(uint8_t(o)); }
        // Voxel shapes by (content stamp, parameters, materials). Ordered, so nothing about it can
        // make two runs differ (it does not feed the simulation, but it costs nothing to rule out).
        std::map<std::tuple<uint64_t, uint64_t, uint64_t>, std::shared_ptr<const CollisionShape>> shapeCache;

        // ---- Materials ----
        std::vector<PhysicsMaterial>          materials = std::vector<PhysicsMaterial>(MAX_PHYSICS_MATERIALS);
        std::vector<uint8_t>                  materialDefined = std::vector<uint8_t>(MAX_PHYSICS_MATERIALS, 0);
        std::vector<JPH::Ref<VoxelMaterial>>  joltMaterials = std::vector<JPH::Ref<VoxelMaterial>>(MAX_PHYSICS_MATERIALS);
        uint64_t                              materialsVersion = 0;   // bumped by every setMaterial
        MaterialContacts                      contacts;
        const VoxelMaterial* joltMaterial(uint16_t index) {
            if (!joltMaterials[index]) joltMaterials[index] = new VoxelMaterial(index);
            return joltMaterials[index].GetPtr();
        }

        // Every live body id, ascending. What the hash and the snapshot iterate, so neither
        // depends on Jolt's internal storage order.
        std::vector<uint32_t> sortedBodies() const {
            JPH::BodyIDVector ids;
            system.GetBodies(ids);
            std::vector<uint32_t> out;
            out.reserve(ids.size());
            for (const JPH::BodyID& id : ids) out.push_back(id.GetIndexAndSequenceNumber());
            std::sort(out.begin(), out.end());
            return out;
        }
    };

    PhysicsWorld::PhysicsWorld(const PhysicsSettings& settings) {
        acquireJolt();
        impl = std::make_unique<Impl>();
        impl->tempAllocator = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(32u * 1024u * 1024u);
        impl->settings = settings;
        int threads = settings.threads;
        if (threads < 0) threads = std::clamp(int(std::thread::hardware_concurrency()) - 1, 0, 7);
        if (threads == 0) impl->jobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        else impl->jobSystem = std::make_unique<JobPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);   // see job_pool.h
        impl->stats.threads = threads;
        impl->lastGood.resize(settings.maxBodies);
        impl->rest.resize(settings.maxBodies);
        impl->outside.assign(settings.maxBodies, 0);
        // Body mutexes: 0 lets Jolt pick a default.
        impl->system.Init(settings.maxBodies, 0, settings.maxBodyPairs, settings.maxContactConstraints,
                          impl->broadPhaseLayers, impl->objectVsBroadPhase, impl->objectPairs);
        impl->system.SetGravity(toJolt(settings.gravity));
        impl->contacts.table = &impl->materials;
        impl->system.SetContactListener(&impl->contacts);
        // Solver iterations: see PhysicsSettings::velocitySteps for why not Jolt's default.
        JPH::PhysicsSettings solver = impl->system.GetPhysicsSettings();
        solver.mNumVelocitySteps = uint32_t(std::max(settings.velocitySteps, 1));
        solver.mNumPositionSteps = uint32_t(std::max(settings.positionSteps, 1));
        impl->system.SetPhysicsSettings(solver);
        if (settings.record) {
            impl->recording = true;
            put(impl->log, LOG_MAGIC);
            put(impl->log, LOG_VERSION);
            writeSettings(impl->log, settings);
        }
    }

    PhysicsWorld::~PhysicsWorld() {
        impl.reset();   // the system, its bodies and shapes go before Jolt's factory does
        releaseJolt();
    }

    BodyId PhysicsWorld::createBody(const BodyDesc& desc) {
        BodyId id = createBodyUnrecorded(desc);
        if (impl->recording) {
            impl->op(Op::Create);
            writeDesc(impl->log, desc);
            put(impl->log, id.value);
        }
        return id;
    }

    BodyId PhysicsWorld::createBodyUnrecorded(const BodyDesc& desc) {
        auto refuse = [&](const char* why) {
            core::warn("physics: refused a body: {}", why);
            impl->stats.refusedBodies++;
            return BodyId{};
        };
        if (!finite(desc.position) || !finite(desc.rotation) || !finite(desc.linearVelocity) ||
            !finite(desc.angularVelocity))
            return refuse("a non-finite position, rotation or velocity");
        if (desc.layer >= PhysicsLayer::Count) return refuse("an unknown layer");
        float qlen = glm::length(glm::vec4(desc.rotation.x, desc.rotation.y, desc.rotation.z, desc.rotation.w));
        if (qlen < 0.5f) return refuse("a rotation that is not a rotation (near-zero quaternion)");
        if (!(desc.density > 0.0f) || !std::isfinite(desc.density) || !std::isfinite(desc.mass) || desc.mass < 0.0f)
            return refuse("a density that is not positive, or a negative mass");

        // Voxel materials: the shape is built with each piece's density, and weighs what it says.
        const bool materials = containsMaterials(desc.shape);
        JPH::ShapeRefC shape = materials ? makeShape(desc.shape, desc.density) : makeShape(desc.shape);
        if (!shape) return refuse("its shape");

        core::quat rotation = desc.rotation / qlen;
        JPH::BodyCreationSettings settings(shape.GetPtr(), toJoltR(desc.position), toJolt(rotation),
                                           toJolt(desc.motion), static_cast<JPH::ObjectLayer>(desc.layer));
        settings.mFriction = desc.friction;
        settings.mRestitution = desc.restitution;
        settings.mLinearDamping = desc.linearDamping;
        settings.mAngularDamping = desc.angularDamping;
        settings.mGravityFactor = desc.gravityFactor;
        settings.mMotionQuality = desc.continuous ? JPH::EMotionQuality::LinearCast
                                                  : JPH::EMotionQuality::Discrete;
        if (desc.motion != MotionType::Static) {
            // Within the body's speed limits, which Jolt asserts on at creation: a velocity carried
            // over from a body at its limit (a rebuild, a piece cut off) can come back a hair over.
            auto capped = [](core::vec3 v, float limit) {
                float length = glm::length(v);
                return length > limit ? v * (limit * 0.999f / length) : v;
            };
            settings.mLinearVelocity = toJolt(capped(desc.linearVelocity, impl->settings.maxLinearSpeed));
            settings.mAngularVelocity = toJolt(capped(desc.angularVelocity, impl->settings.maxAngularSpeed));
        }
        settings.mMaxLinearVelocity = impl->settings.maxLinearSpeed;
        settings.mMaxAngularVelocity = impl->settings.maxAngularSpeed;
        if (desc.motion == MotionType::Dynamic) {
            JPH::MassProperties properties = shape->GetMassProperties();
            float mass = desc.mass > 0.0f ? desc.mass : materials ? properties.mMass : desc.shape.volume() * desc.density;
            if (!(mass > 0.0f) || !std::isfinite(mass)) return refuse("a shape with no volume, so no mass");
            mass = std::max(mass, impl->settings.minMass);
            // The shape's own distribution, scaled to the mass -- and then floored about every axis,
            // so no body is light enough to turn for the solver to spin it up without limit.
            properties.ScaleToMass(mass);
            JPH::Mat44 axes;
            JPH::Vec3 moments;
            if (properties.DecomposePrincipalMomentsOfInertia(axes, moments)) {
                float floor = mass * impl->settings.minGyrationRadius * impl->settings.minGyrationRadius;
                moments = JPH::Vec3::sMax(moments, JPH::Vec3::sReplicate(floor));
                properties.mInertia = axes * JPH::Mat44::sScale(moments) * axes.Transposed3x3();
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                settings.mMassPropertiesOverride = properties;
            } else {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = mass;
            }
        }
        // Voxel shapes are many boxes side by side; without this a body sliding across them catches
        // on the seams between neighbours.
        if (containsVoxels(desc.shape)) settings.mEnhancedInternalEdgeRemoval = true;

        JPH::BodyInterface& bodies = impl->system.GetBodyInterface();
        JPH::Body* body = bodies.CreateBody(settings);
        if (!body) return refuse("the body table is full (PhysicsSettings::maxBodies)");
        body->SetCollisionGroup(JPH::CollisionGroup(impl->jointedPairs, body->GetID().GetIndex(), 0));
        bodies.AddBody(body->GetID(), desc.motion == MotionType::Static ? JPH::EActivation::DontActivate
                                                                        : JPH::EActivation::Activate);
        uint32_t index = body->GetID().GetIndex();
        impl->lastGood[index] = {body->GetPosition(), body->GetRotation()};
        impl->rest[index] = {body->GetPosition(), body->GetRotation(), 0.0f};
        impl->outside[index] = 0;
        uint32_t count = impl->system.GetNumBodies();
        if (!impl->warnedCapacity && count * 5 > impl->settings.maxBodies * 4) {
            impl->warnedCapacity = true;
            core::warn("physics: {} bodies is over 80% of PhysicsSettings::maxBodies ({}); past it, bodies are refused",
                       count, impl->settings.maxBodies);
        }
        return BodyId{body->GetID().GetIndexAndSequenceNumber()};
    }

    void PhysicsWorld::wakeBodiesAround(BodyId id) {
        if (!id.valid()) return;
        JPH::AABox bounds;
        {
            JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), JPH::BodyID(id.value));
            if (!lock.Succeeded()) return;
            bounds = lock.GetBody().GetWorldSpaceBounds();
        }
        bounds.ExpandBy(JPH::Vec3::sReplicate(0.05f));   // and what rests exactly on its faces
        impl->system.GetBodyInterface().ActivateBodiesInAABox(bounds, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
    }

    void PhysicsWorld::destroyBody(BodyId id) {
        if (impl->recording) { impl->op(Op::Destroy); put(impl->log, id.value); }
        if (!isAlive(id)) return;
        wakeBodiesAround(id);
        removeJointsOf(id);
        JPH::BodyInterface& bodies = impl->system.GetBodyInterface();
        JPH::BodyID jid(id.value);
        bodies.RemoveBody(jid);
        bodies.DestroyBody(jid);
    }

    bool PhysicsWorld::isAlive(BodyId id) const {
        if (!id.valid()) return false;
        // The lock checks the sequence number, so an id whose slot now holds another body fails.
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), JPH::BodyID(id.value));
        return lock.Succeeded();
    }

    BodyState PhysicsWorld::bodyState(BodyId id) const {
        BodyState state;
        if (!id.valid()) return state;
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), JPH::BodyID(id.value));
        if (!lock.Succeeded()) return state;
        const JPH::Body& body = lock.GetBody();
        state.centerOfMass = fromJolt(body.GetCenterOfMassPosition());
        state.position = fromJolt(body.GetPosition());
        state.rotation = fromJolt(body.GetRotation());
        state.linearVelocity = fromJolt(body.GetLinearVelocity());
        state.angularVelocity = fromJolt(body.GetAngularVelocity());
        state.awake = body.IsActive();
        return state;
    }

    // ---- Characters ------------------------------------------------------------------------------

    namespace {
        // What a character collides with: its layer's row of the matrix, never its own inner body.
        class CharacterLayers final : public JPH::ObjectLayerFilter {
        public:
            explicit CharacterLayers(PhysicsLayer layer) : layer(layer) {}
            bool ShouldCollide(JPH::ObjectLayer other) const override { return layersCollide(layer, PhysicsLayer(other)); }
        private:
            PhysicsLayer layer;
        };

        // An upright capsule whose origin is at its feet.
        JPH::RefConst<JPH::Shape> characterCapsule(float radius, float height) {
            float halfCylinder = std::max(0.5f * height - radius, 0.01f);
            return new JPH::RotatedTranslatedShape(JPH::Vec3(0, halfCylinder + radius, 0), JPH::Quat::sIdentity(),
                                                   new JPH::CapsuleShape(halfCylinder, radius));
        }
    }

    CharacterId PhysicsWorld::createCharacter(const CharacterDesc& desc) {
        CharacterId id;
        bool usable = finite(desc.position) && desc.radius > 0.0f && std::isfinite(desc.radius) &&
                      std::isfinite(desc.height) && desc.height >= 2.0f * desc.radius && desc.mass > 0.0f &&
                      desc.layer < PhysicsLayer::Count;
        if (!usable) {
            core::warn("physics: refused a character (radius {}, height {}, mass {})", desc.radius, desc.height, desc.mass);
            impl->stats.refusedBodies++;
        } else {
            size_t slot = 0;
            while (slot < impl->characters.size() && impl->characters[slot].controller) slot++;
            if (slot >= 0xffffu) {
                core::warn("physics: refused a character: too many");
            } else {
                if (slot == impl->characters.size()) impl->characters.emplace_back();
                Impl::Character& c = impl->characters[slot];
                JPH::CharacterVirtualSettings settings;
                settings.mShape = characterCapsule(desc.radius, desc.height);
                // A little inside the controller's own shape, so the controller's sweeps never meet it.
                settings.mInnerBodyShape = characterCapsule(desc.radius * 0.9f, desc.height * 0.95f);
                settings.mInnerBodyLayer = JPH::ObjectLayer(desc.layer);
                settings.mMaxSlopeAngle = desc.maxSlope;
                settings.mMass = desc.mass;
                settings.mMaxStrength = desc.maxStrength;
                settings.mEnhancedInternalEdgeRemoval = true;   // voxel floors are many boxes side by side
                c.controller = new JPH::CharacterVirtual(&settings, toJoltR(desc.position), JPH::Quat::sIdentity(), &impl->system);
                c.desc = desc;
                c.input = core::vec3(0.0f);
                c.jump = false;
                c.generation = uint16_t(c.generation + 1);
                id.value = uint32_t(slot) | (uint32_t(c.generation) << 16);
            }
        }
        if (impl->recording) {
            impl->op(Op::CreateCharacter);
            put(impl->log, desc.radius); put(impl->log, desc.height); put(impl->log, desc.position);
            put(impl->log, desc.maxSlope); put(impl->log, desc.mass); put(impl->log, desc.maxStrength);
            put(impl->log, desc.stepUp); put(impl->log, desc.stickDown); put(impl->log, uint8_t(desc.layer));
            put(impl->log, id.value);
        }
        return id;
    }

    void PhysicsWorld::destroyCharacter(CharacterId id) {
        if (impl->recording) { impl->op(Op::DestroyCharacter); put(impl->log, id.value); }
        Impl::Character* c = impl->character(id);
        if (!c) return;
        wakeBodiesAround(BodyId{c->controller->GetInnerBodyID().GetIndexAndSequenceNumber()});
        c->controller = nullptr;   // removes its inner body
    }

    bool PhysicsWorld::isAlive(CharacterId id) const { return impl->character(id) != nullptr; }

    void PhysicsWorld::setCharacterInput(CharacterId id, core::vec3 horizontal, bool jump, float jumpSpeed) {
        if (impl->recording) {
            impl->op(Op::CharacterInput);
            put(impl->log, id.value); put(impl->log, horizontal); put(impl->log, uint8_t(jump)); put(impl->log, jumpSpeed);
        }
        Impl::Character* c = impl->character(id);
        if (!c || !finite(horizontal) || !std::isfinite(jumpSpeed)) return;
        c->input = core::vec3(horizontal.x, 0.0f, horizontal.z);
        c->jump = c->jump || jump;
        c->jumpSpeed = jumpSpeed;
    }

    void PhysicsWorld::setCharacterPosition(CharacterId id, core::vec3 feet) {
        if (impl->recording) { impl->op(Op::CharacterPosition); put(impl->log, id.value); put(impl->log, feet); }
        Impl::Character* c = impl->character(id);
        if (!c || !finite(feet)) return;
        c->controller->SetPosition(toJoltR(feet));
    }

    CharacterState PhysicsWorld::characterState(CharacterId id) const {
        CharacterState state;
        const Impl::Character* c = impl->character(id);
        if (!c) return state;
        const JPH::CharacterVirtual& ch = *c->controller;
        state.position = fromJolt(ch.GetPosition());
        state.velocity = fromJolt(ch.GetLinearVelocity());
        state.onGround = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        state.onSteepGround = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnSteepGround;
        state.groundNormal = fromJolt(ch.GetGroundNormal());
        if (!ch.GetGroundBodyID().IsInvalid()) state.groundBody = BodyId{ch.GetGroundBodyID().GetIndexAndSequenceNumber()};
        return state;
    }

    BodyId PhysicsWorld::characterBody(CharacterId id) const {
        const Impl::Character* c = impl->character(id);
        return c ? BodyId{c->controller->GetInnerBodyID().GetIndexAndSequenceNumber()} : BodyId{};
    }

    // Each character, in slot order, before the bodies step: the velocity Jolt's own controller
    // sample builds -- carried by the ground while standing, falling otherwise, plus what it walks
    // at -- then the move, with stair stepping and floor sticking.
    void PhysicsWorld::updateCharacters(float dt) {
        const JPH::Vec3 gravity = impl->system.GetGravity();
        const JPH::Vec3 up = JPH::Vec3::sAxisY();
        for (Impl::Character& c : impl->characters) {
            if (!c.controller) continue;
            JPH::CharacterVirtual& ch = *c.controller;
            ch.UpdateGroundVelocity();
            JPH::Vec3 current = ch.GetLinearVelocity();
            JPH::Vec3 vertical = up * current.Dot(up);
            JPH::Vec3 ground = ch.GetGroundVelocity();
            JPH::Vec3 velocity;
            if (ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround && (vertical.GetY() - ground.GetY()) < 0.1f) {
                velocity = ground;
                if (c.jump) {
                    velocity += up * c.jumpSpeed;
                    c.jump = false;
                }
            } else {
                velocity = vertical;
            }
            velocity += gravity * dt + toJolt(c.input);
            ch.SetLinearVelocity(velocity);
            JPH::CharacterVirtual::ExtendedUpdateSettings settings;
            settings.mWalkStairsStepUp = up * c.desc.stepUp;
            settings.mStickToFloorStepDown = -up * c.desc.stickDown;
            CharacterLayers layers(c.desc.layer);
            JPH::IgnoreSingleBodyFilter self(ch.GetInnerBodyID());
            ch.ExtendedUpdate(dt, gravity, settings, JPH::BroadPhaseLayerFilter(), layers, self, JPH::ShapeFilter(),
                              *impl->tempAllocator);
        }
    }

    // ---- Joints ----------------------------------------------------------------------------------

    namespace {
        // A hinge's or slider's motor: off, driving at a velocity, or holding a target (an angle or an
        // offset) the way a servo does.
        void applyMotor(JPH::TwoBodyConstraint* c, JointType type, MotorMode mode, float velocity, float target, bool on = true) {
            JPH::EMotorState state = !on ? JPH::EMotorState::Off
                                   : mode == MotorMode::Position ? JPH::EMotorState::Position : JPH::EMotorState::Velocity;
            if (type == JointType::Hinge) {
                auto* h = static_cast<JPH::HingeConstraint*>(c);
                h->SetMotorState(state);
                h->SetTargetAngularVelocity(velocity);
                h->SetTargetAngle(target);
            } else if (type == JointType::Slider) {
                auto* sl = static_cast<JPH::SliderConstraint*>(c);
                sl->SetMotorState(state);
                sl->SetTargetVelocity(velocity);
                sl->SetTargetPosition(target);
            }
        }
        void writeJointDesc(std::vector<uint8_t>& out, const JointDesc& d) {
            put(out, uint8_t(d.type)); put(out, d.a.value); put(out, d.b.value); put(out, d.anchor); put(out, d.anchorB);
            put(out, d.axis); put(out, d.limitMin); put(out, d.limitMax); put(out, d.springFrequency); put(out, d.springDamping);
            put(out, d.friction); put(out, uint8_t(d.motor)); put(out, d.motorVelocity); put(out, d.motorLimit);
            put(out, uint8_t(d.motorMode)); put(out, d.motorTarget); put(out, d.motorFrequency);
            put(out, uint8_t(d.collideConnected));
            put(out, d.breakForce); put(out, d.breakTorque);
        }
        bool readJointDesc(const std::vector<uint8_t>& in, size_t& at, JointDesc& d) {
            uint8_t type = 0, motor = 0, mode = 0, collide = 0;
            bool ok = get(in, at, type) && get(in, at, d.a.value) && get(in, at, d.b.value) && get(in, at, d.anchor) &&
                      get(in, at, d.anchorB) && get(in, at, d.axis) && get(in, at, d.limitMin) && get(in, at, d.limitMax) &&
                      get(in, at, d.springFrequency) && get(in, at, d.springDamping) && get(in, at, d.friction) &&
                      get(in, at, motor) && get(in, at, d.motorVelocity) && get(in, at, d.motorLimit) &&
                      get(in, at, mode) && get(in, at, d.motorTarget) && get(in, at, d.motorFrequency) &&
                      get(in, at, collide) &&
                      get(in, at, d.breakForce) && get(in, at, d.breakTorque);
            d.type = JointType(type);
            d.motor = motor != 0;
            d.motorMode = MotorMode(mode);
            d.collideConnected = collide != 0;
            return ok;
        }
    }

    JointId PhysicsWorld::createJoint(const JointDesc& desc) {
        JointId id;
        auto record = [&] {
            if (!impl->recording) return;
            impl->op(Op::CreateJoint);
            writeJointDesc(impl->log, desc);
            put(impl->log, id.value);
        };
        float axisLength = glm::length(desc.axis);
        bool usable = isAlive(desc.a) && (!desc.b.valid() || isAlive(desc.b)) && !(desc.a == desc.b) &&
                      finite(desc.anchor) && finite(desc.anchorB) && finite(desc.axis) && axisLength > 1e-6f &&
                      std::isfinite(desc.limitMin) && std::isfinite(desc.limitMax) && desc.type <= JointType::Distance &&
                      (desc.type != JointType::Hinge || (desc.limitMin <= 0.0f && desc.limitMax >= 0.0f)) &&
                      (desc.type != JointType::Slider || desc.limitMin <= desc.limitMax);
        size_t slot = 0;
        while (slot < impl->joints.size() && impl->joints[slot].constraint) slot++;
        if (!usable || slot >= 0xffffu) {
            core::warn("physics: refused a joint (type {}): a body is not alive, or its placement is unusable", int(desc.type));
            record();
            return id;
        }
        const JPH::Vec3 axis = toJolt(desc.axis / axisLength);
        const JPH::Vec3 normal = axis.GetNormalizedPerpendicular();
        const JPH::RVec3 anchor = toJoltR(desc.anchor);
        JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
        switch (desc.type) {
            case JointType::Fixed: {
                auto* f = new JPH::FixedConstraintSettings;
                f->mPoint1 = f->mPoint2 = anchor;   // world space, axes left equal: hold the bodies as they are
                settings = f;
                break;
            }
            case JointType::Point: {
                auto* p = new JPH::PointConstraintSettings;
                p->mPoint1 = p->mPoint2 = anchor;
                settings = p;
                break;
            }
            case JointType::Hinge: {
                auto* h = new JPH::HingeConstraintSettings;
                h->mPoint1 = h->mPoint2 = anchor;
                h->mHingeAxis1 = h->mHingeAxis2 = axis;
                h->mNormalAxis1 = h->mNormalAxis2 = normal;
                if (desc.limitMin != 0.0f || desc.limitMax != 0.0f) {
                    h->mLimitsMin = std::max(desc.limitMin, -JPH::JPH_PI);
                    h->mLimitsMax = std::min(desc.limitMax, JPH::JPH_PI);
                }
                h->mMaxFrictionTorque = std::max(desc.friction, 0.0f);
                if (desc.motorLimit > 0.0f) h->mMotorSettings.SetTorqueLimit(desc.motorLimit);
                if (desc.motorFrequency > 0.0f) h->mMotorSettings.mSpringSettings.mFrequency = desc.motorFrequency;
                settings = h;
                break;
            }
            case JointType::Slider: {
                auto* sl = new JPH::SliderConstraintSettings;
                sl->mPoint1 = sl->mPoint2 = anchor;
                sl->mSliderAxis1 = sl->mSliderAxis2 = axis;
                sl->mNormalAxis1 = sl->mNormalAxis2 = normal;
                if (desc.limitMin != 0.0f || desc.limitMax != 0.0f) {
                    sl->mLimitsMin = desc.limitMin;
                    sl->mLimitsMax = desc.limitMax;
                }
                sl->mMaxFrictionForce = std::max(desc.friction, 0.0f);
                if (desc.motorLimit > 0.0f) sl->mMotorSettings.SetForceLimit(desc.motorLimit);
                if (desc.motorFrequency > 0.0f) sl->mMotorSettings.mSpringSettings.mFrequency = desc.motorFrequency;
                settings = sl;
                break;
            }
            case JointType::Distance: {
                auto* d = new JPH::DistanceConstraintSettings;
                d->mPoint1 = toJoltR(desc.anchorB);   // Jolt's body 1 is b (see below)
                d->mPoint2 = anchor;
                // Both 0: held at the distance the anchors are apart now. Negative: likewise, Jolt's way.
                bool asMade = desc.limitMin == 0.0f && desc.limitMax == 0.0f;
                d->mMinDistance = asMade ? -1.0f : desc.limitMin;
                d->mMaxDistance = asMade ? -1.0f : desc.limitMax;
                if (desc.limitMin >= 0.0f && desc.limitMax >= 0.0f && desc.limitMin > desc.limitMax) d->mMaxDistance = desc.limitMin;
                d->mLimitsSpringSettings.mFrequency = std::max(desc.springFrequency, 0.0f);
                d->mLimitsSpringSettings.mDamping = std::clamp(desc.springDamping, 0.0f, 1.0f);
                settings = d;
                break;
            }
        }
        JPH::BodyInterface& bodies = impl->system.GetBodyInterface();
        JPH::BodyID a(desc.a.value), b = desc.b.valid() ? JPH::BodyID(desc.b.value) : JPH::BodyID();
        // Jolt measures body 2 relative to body 1 -- a hinge's angle, a slider's offset, a motor's
        // velocity -- and the description speaks of `a` relative to `b`: so b is Jolt's body 1.
        JPH::TwoBodyConstraint* constraint = bodies.CreateConstraint(settings.GetPtr(), b, a);
        if (!constraint) {
            core::warn("physics: refused a joint: Jolt could not make it");
            record();
            return id;
        }
        if (desc.motor) applyMotor(constraint, desc.type, desc.motorMode, desc.motorVelocity, desc.motorTarget);
        // Joints get more solver iterations than contacts: chains and ragdolls are where a stretch
        // shows, and they are few enough that it costs little.
        constraint->SetNumVelocityStepsOverride(uint32_t(std::max(impl->settings.jointVelocitySteps, 1)));
        constraint->SetNumPositionStepsOverride(uint32_t(std::max(impl->settings.jointPositionSteps, 1)));
        impl->system.AddConstraint(constraint);
        bodies.ActivateConstraint(constraint);

        if (slot == impl->joints.size()) impl->joints.emplace_back();
        Impl::Joint& j = impl->joints[slot];
        j.constraint = constraint;
        j.desc = desc;
        j.force = j.torque = 0.0f;
        j.generation = uint16_t(j.generation + 1);
        impl->separate(j, true);
        // The anchors in each body's own frame, for measuring a distance joint's length later.
        auto local = [&](BodyId body, core::vec3 world) {
            if (!body.valid()) return world;
            BodyState s = bodyState(body);
            return glm::conjugate(s.rotation) * (world - s.position);
        };
        j.localA = local(desc.a, desc.anchor);
        j.localB = local(desc.b, desc.type == JointType::Distance ? desc.anchorB : desc.anchor);
        id.value = uint32_t(slot) | (uint32_t(j.generation) << 16);
        record();
        return id;
    }

    void PhysicsWorld::destroyJoint(JointId id) {
        if (impl->recording) { impl->op(Op::DestroyJoint); put(impl->log, id.value); }
        Impl::Joint* j = impl->joint(id);
        if (!j) return;
        impl->system.GetBodyInterface().ActivateConstraint(j->constraint);   // what it held lets go now
        impl->system.RemoveConstraint(j->constraint);
        impl->separate(*j, false);
        j->constraint = nullptr;
    }

    bool PhysicsWorld::isAlive(JointId id) const { return impl->joint(id) != nullptr; }

    void PhysicsWorld::setJointMotor(JointId id, bool on, float velocity) {
        setJointMotor(id, on, MotorMode::Velocity, velocity);
    }

    void PhysicsWorld::setJointMotor(JointId id, bool on, MotorMode mode, float value) {
        if (impl->recording) {
            impl->op(Op::JointMotor);
            put(impl->log, id.value); put(impl->log, uint8_t(on)); put(impl->log, uint8_t(mode)); put(impl->log, value);
        }
        Impl::Joint* j = impl->joint(id);
        if (!j || !std::isfinite(value) || (j->desc.type != JointType::Hinge && j->desc.type != JointType::Slider)) return;
        j->desc.motor = on;
        j->desc.motorMode = mode;
        if (mode == MotorMode::Position) j->desc.motorTarget = value;
        else j->desc.motorVelocity = value;
        applyMotor(j->constraint.GetPtr(), j->desc.type, mode, j->desc.motorVelocity, j->desc.motorTarget, on);
        impl->system.GetBodyInterface().ActivateConstraint(j->constraint);
    }

    JointState PhysicsWorld::jointState(JointId id) const {
        JointState state;
        const Impl::Joint* j = impl->joint(id);
        if (!j) return state;
        state.alive = true;
        state.force = j->force;
        state.torque = j->torque;
        switch (j->desc.type) {
            case JointType::Hinge: state.position = static_cast<const JPH::HingeConstraint*>(j->constraint.GetPtr())->GetCurrentAngle(); break;
            case JointType::Slider: state.position = static_cast<const JPH::SliderConstraint*>(j->constraint.GetPtr())->GetCurrentPosition(); break;
            case JointType::Distance: {
                auto world = [&](BodyId body, core::vec3 local) {
                    if (!body.valid()) return local;
                    BodyState s = bodyState(body);
                    return s.position + s.rotation * local;
                };
                state.position = glm::length(world(j->desc.a, j->localA) - world(j->desc.b, j->localB));
                break;
            }
            default: break;
        }
        return state;
    }

    std::vector<PhysicsWorld::BrokenJoint> PhysicsWorld::takeBrokenJoints() {
        std::vector<BrokenJoint> out;
        out.swap(impl->brokenJoints);
        return out;
    }

    // After every step, in slot order: what each joint held with -- its constraint's accumulated
    // impulse over the step -- and those that held with more than they can take, broken.
    void PhysicsWorld::checkJoints(float dt) {
        const float inverseDt = dt > 0.0f ? 1.0f / dt : 0.0f;
        for (size_t slot = 0; slot < impl->joints.size(); slot++) {
            Impl::Joint& j = impl->joints[slot];
            if (!j.constraint) continue;
            float force = 0.0f, torque = 0.0f;
            JPH::TwoBodyConstraint* c = j.constraint.GetPtr();
            switch (j.desc.type) {
                case JointType::Fixed: {
                    auto* f = static_cast<JPH::FixedConstraint*>(c);
                    force = f->GetTotalLambdaPosition().Length();
                    torque = f->GetTotalLambdaRotation().Length();
                    break;
                }
                case JointType::Point: force = static_cast<JPH::PointConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
                case JointType::Hinge: {
                    auto* h = static_cast<JPH::HingeConstraint*>(c);
                    force = h->GetTotalLambdaPosition().Length();
                    JPH::Vector<2> r = h->GetTotalLambdaRotation();
                    torque = std::sqrt(r[0] * r[0] + r[1] * r[1]) + std::abs(h->GetTotalLambdaRotationLimits());
                    break;
                }
                case JointType::Slider: {
                    auto* sl = static_cast<JPH::SliderConstraint*>(c);
                    JPH::Vector<2> p = sl->GetTotalLambdaPosition();
                    force = std::sqrt(p[0] * p[0] + p[1] * p[1]) + std::abs(sl->GetTotalLambdaPositionLimits());
                    torque = sl->GetTotalLambdaRotation().Length();
                    break;
                }
                case JointType::Distance: force = std::abs(static_cast<JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition()); break;
            }
            j.force = force * inverseDt;
            j.torque = torque * inverseDt;
            bool broke = (j.desc.breakForce > 0.0f && j.force > j.desc.breakForce) ||
                         (j.desc.breakTorque > 0.0f && j.torque > j.desc.breakTorque);
            if (!broke) continue;
            // Jolt solved the step with the joint whole, so the impulse that broke it was spent holding
            // the bodies together -- a rope that snaps under a yank would have stopped the yank. What it
            // took beyond what it could hold is given back, so what broke it carries on.
            if (j.desc.breakForce > 0.0f && j.force > j.desc.breakForce &&
                (j.desc.type == JointType::Point || j.desc.type == JointType::Fixed || j.desc.type == JointType::Hinge)) {
                JPH::Vec3 lambda = j.desc.type == JointType::Point ? static_cast<JPH::PointConstraint*>(c)->GetTotalLambdaPosition()
                                 : j.desc.type == JointType::Fixed ? static_cast<JPH::FixedConstraint*>(c)->GetTotalLambdaPosition()
                                                                   : static_cast<JPH::HingeConstraint*>(c)->GetTotalLambdaPosition();
                float held = j.desc.breakForce * dt;
                float length = lambda.Length();
                if (length > held) {
                    JPH::Vec3 excess = lambda * ((length - held) / length);
                    BodyState sa = bodyState(j.desc.a);
                    JPH::RVec3 at = toJoltR(sa.position + sa.rotation * j.localA);
                    JPH::BodyInterface& bodies = impl->system.GetBodyInterface();
                    // Jolt gave body 2 (a) +lambda and body 1 (b) -lambda.
                    bodies.AddImpulse(JPH::BodyID(j.desc.a.value), -excess, at);
                    if (j.desc.b.valid()) bodies.AddImpulse(JPH::BodyID(j.desc.b.value), excess, at);
                }
            }
            impl->system.GetBodyInterface().ActivateConstraint(c);
            impl->system.RemoveConstraint(c);
            impl->separate(j, false);   // broken apart, they collide again
            j.constraint = nullptr;
            impl->brokenJoints.push_back({JointId{uint32_t(slot) | (uint32_t(j.generation) << 16)}, j.force, j.torque});
        }
    }

    void PhysicsWorld::removeJointsOf(BodyId body) {
        for (Impl::Joint& j : impl->joints) {
            if (!j.constraint || !(j.desc.a == body || j.desc.b == body)) continue;
            impl->system.RemoveConstraint(j.constraint);
            impl->separate(j, false);
            j.constraint = nullptr;
        }
    }

    void PhysicsWorld::step(float dt) {
        auto started = std::chrono::steady_clock::now();
        updateCharacters(dt);
        // A running motor keeps what it drives awake. Jolt lets a body sleep once it stops moving,
        // and a motor stalled for a moment -- a windmill jammed by a crate -- would otherwise sleep
        // for good once the jam cleared, its motor still on and nothing to wake it.
        for (const Impl::Joint& j : impl->joints)
            if (j.constraint && j.desc.motor && j.desc.motorVelocity != 0.0f)
                impl->system.GetBodyInterface().ActivateConstraint(j.constraint);
        JPH::EPhysicsUpdateError errors =
            impl->system.Update(dt, 1, impl->tempAllocator.get(), impl->jobSystem.get());
        checkJoints(dt);
        sanityPass(dt);
        impl->stats.lastStepMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        impl->tick++;
        if (impl->recording) {
            impl->op(Op::Step);
            put(impl->log, dt);
            put(impl->log, stateHash());
            impl->loggedSteps++;
        }
        impl->stats.steps++;
        if (impl->tick % 60 == 0) pruneShapeCache();
        impl->stats.bodies = impl->system.GetNumBodies();
        impl->stats.awakeBodies = impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
        if (errors != JPH::EPhysicsUpdateError::None) {
            // Jolt carries on without the contacts it had no room for, so bodies can sink into each
            // other or fall through. Loud, every time: it means the capacities are wrong.
            impl->stats.stepsWithDroppedContacts++;
            auto has = [&](JPH::EPhysicsUpdateError e) { return (errors & e) != JPH::EPhysicsUpdateError::None; };
            core::error("physics: tick {} dropped contacts ({}{}{}); {} bodies. Raise the capacities in "
                        "PhysicsSettings.", impl->tick,
                        has(JPH::EPhysicsUpdateError::ManifoldCacheFull) ? "manifold cache full " : "",
                        has(JPH::EPhysicsUpdateError::BodyPairCacheFull) ? "body pair cache full " : "",
                        has(JPH::EPhysicsUpdateError::ContactConstraintsFull) ? "contact constraints full" : "",
                        impl->stats.bodies);
        }
    }

    // After every step: what moved is checked. A non-finite pose or velocity -- a solver blow-up --
    // is put back to the body's last finite pose, at rest and asleep, rather than spreading to
    // everything it touches next step. A body outside the world bounds is reported once per
    // excursion. Only awake bodies can have changed, so only they are looked at; Jolt's list of them
    // is in a deterministic order, so the recoveries are too.
    void PhysicsWorld::sanityPass(float dt) {
        JPH::BodyIDVector active;
        impl->system.GetActiveBodies(JPH::EBodyType::RigidBody, active);
        const JPH::Vec3 low = toJolt(impl->settings.worldMin), high = toJolt(impl->settings.worldMax);
        std::vector<JPH::BodyID> broken, resting;
        const float restRadius = impl->settings.restRadius;
        const float restCos = std::cos(0.5f * impl->settings.restAngle);   // |dot| of quaternions within the angle
        {
            const JPH::BodyLockInterfaceLocking& locks = impl->system.GetBodyLockInterface();
            for (const JPH::BodyID& id : active) {
                JPH::BodyLockRead lock(locks, id);
                if (!lock.Succeeded()) continue;
                const JPH::Body& body = lock.GetBody();
                JPH::Vec3 position(body.GetPosition());
                JPH::Quat rotation = body.GetRotation();
                bool finiteState = !position.IsNaN() && !rotation.IsNaN() && !body.GetLinearVelocity().IsNaN() &&
                                   !body.GetAngularVelocity().IsNaN() &&
                                   std::isfinite(position.GetX() + position.GetY() + position.GetZ()) &&
                                   !impl->faultInjected.count(id.GetIndexAndSequenceNumber());
                uint32_t index = id.GetIndex();
                if (!finiteState) { broken.push_back(id); continue; }
                impl->lastGood[index] = {body.GetPosition(), rotation};
                if (impl->settings.restSeconds > 0.0f && body.IsDynamic()) {
                    Impl::Rest& r = impl->rest[index];
                    bool moved = JPH::Vec3(body.GetPosition() - r.position).LengthSq() > restRadius * restRadius ||
                                 std::abs(r.rotation.Dot(rotation)) < restCos;
                    if (moved) r = {body.GetPosition(), rotation, 0.0f};
                    else if ((r.seconds += dt) >= impl->settings.restSeconds) resting.push_back(id);
                }
                bool out = JPH::Vec3::sLess(position, low).TestAnyXYZTrue() ||
                           JPH::Vec3::sGreater(position, high).TestAnyXYZTrue();
                if (out && !impl->outside[index]) impl->leftWorld.push_back(BodyId{id.GetIndexAndSequenceNumber()});
                impl->outside[index] = out ? 1 : 0;
            }
        }
        JPH::BodyInterface& bodies = impl->system.GetBodyInterface();
        // Awake, and going nowhere: the slow rocking a stack can settle into, which the solver does
        // not damp and Jolt's sleep test, watching speeds, never ends. Bled off until it sleeps.
        for (const JPH::BodyID& id : resting) {
            JPH::Vec3 linear = bodies.GetLinearVelocity(id), angular = bodies.GetAngularVelocity(id);
            float keep = impl->settings.restDamping;
            bodies.SetLinearAndAngularVelocity(id, linear * keep, angular * keep);
        }
        for (const JPH::BodyID& id : broken) {
            const Impl::GoodPose& good = impl->lastGood[id.GetIndex()];
            bodies.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            bodies.SetPositionAndRotation(id, good.position, good.rotation, JPH::EActivation::DontActivate);
            bodies.DeactivateBody(id);
            impl->faultInjected.erase(id.GetIndexAndSequenceNumber());
            impl->stats.nonFiniteRecoveries++;
            core::error("physics: body {} had a non-finite pose or velocity after tick {}; put back where it "
                        "last was, at rest. This is a bug worth reporting with the scene that caused it.",
                        id.GetIndexAndSequenceNumber(), impl->tick + 1);
        }
    }

    std::vector<BodyId> PhysicsWorld::takeBodiesThatLeftTheWorld() {
        std::vector<BodyId> out;
        out.swap(impl->leftWorld);
        std::sort(out.begin(), out.end(), [](BodyId a, BodyId b) { return a.value < b.value; });
        impl->stats.leftWorld += out.size();
        return out;
    }

    void PhysicsWorld::corruptBodyForTesting(BodyId id) {
        if (isAlive(id)) impl->faultInjected.insert(id.value);
    }

    namespace {
        bool canMove(const JPH::PhysicsSystem& system, BodyId id) {
            if (!id.valid()) return false;
            JPH::BodyLockRead lock(system.GetBodyLockInterface(), JPH::BodyID(id.value));
            return lock.Succeeded() && !lock.GetBody().IsStatic();
        }
    }

    void PhysicsWorld::addImpulse(BodyId id, core::vec3 impulse) {
        if (impl->recording) { impl->op(Op::Impulse); put(impl->log, id.value); put(impl->log, impulse); }
        if (!finite(impulse) || !canMove(impl->system, id)) return;
        impl->system.GetBodyInterface().AddImpulse(JPH::BodyID(id.value), toJolt(impulse));
    }

    void PhysicsWorld::addAngularImpulse(BodyId id, core::vec3 impulse) {
        if (impl->recording) { impl->op(Op::AngularImpulse); put(impl->log, id.value); put(impl->log, impulse); }
        if (!finite(impulse) || !canMove(impl->system, id)) return;
        impl->system.GetBodyInterface().AddAngularImpulse(JPH::BodyID(id.value), toJolt(impulse));
    }

    void PhysicsWorld::addVelocity(BodyId id, core::vec3 linear) {
        if (impl->recording) { impl->op(Op::AddVelocity); put(impl->log, id.value); put(impl->log, linear); }
        if (!finite(linear) || !canMove(impl->system, id)) return;
        JPH::BodyInterface& bodies = impl->system.GetBodyInterface();
        bodies.AddLinearVelocity(JPH::BodyID(id.value), toJolt(linear));
    }

    void PhysicsWorld::setVelocity(BodyId id, core::vec3 linear, core::vec3 angular) {
        if (impl->recording) { impl->op(Op::SetVelocity); put(impl->log, id.value); put(impl->log, linear); put(impl->log, angular); }
        if (!finite(linear) || !finite(angular) || !canMove(impl->system, id)) return;
        impl->system.GetBodyInterface().SetLinearAndAngularVelocity(JPH::BodyID(id.value), toJolt(linear),
                                                                    toJolt(angular));
    }

    void PhysicsWorld::setPose(BodyId id, core::vec3 position, core::quat rotation) {
        if (impl->recording) { impl->op(Op::SetPose); put(impl->log, id.value); put(impl->log, position); put(impl->log, rotation); }
        if (!finite(position) || !finite(rotation) || !isAlive(id)) return;
        float length = glm::length(glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w));
        if (length < 0.5f) return;
        impl->system.GetBodyInterface().SetPositionAndRotation(JPH::BodyID(id.value), toJoltR(position),
                                                               toJolt(rotation / length), JPH::EActivation::Activate);
    }

    void PhysicsWorld::moveKinematic(BodyId id, core::vec3 position, core::quat rotation, float dt) {
        if (impl->recording) { impl->op(Op::MoveKinematic); put(impl->log, id.value); put(impl->log, position); put(impl->log, rotation); put(impl->log, dt); }
        if (!finite(position) || !finite(rotation) || !(dt > 0.0f) || !canMove(impl->system, id)) return;
        float length = glm::length(glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w));
        if (length < 0.5f) return;
        impl->system.GetBodyInterface().MoveKinematic(JPH::BodyID(id.value), toJoltR(position),
                                                      toJolt(rotation / length), dt);
    }

    void PhysicsWorld::setGravity(core::vec3 gravity) {
        if (impl->recording) { impl->op(Op::Gravity); put(impl->log, gravity); }
        if (finite(gravity)) impl->system.SetGravity(toJolt(gravity));
    }

    core::vec3 PhysicsWorld::gravity() const { return fromJolt(impl->system.GetGravity()); }

    uint64_t PhysicsWorld::tick() const { return impl->tick; }

    float PhysicsWorld::bodyMass(BodyId id) const {
        if (!id.valid()) return 0.0f;
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), JPH::BodyID(id.value));
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) return 0.0f;
        float inverse = lock.GetBody().GetMotionProperties()->GetInverseMass();
        return inverse > 0.0f ? 1.0f / inverse : 0.0f;
    }

    CollisionShapeRef PhysicsWorld::shapeFromPieces(const utils::CollisionPieces& pieces, float otherDensity) {
        if (pieces.pieces.empty()) return nullptr;
        if (!(otherDensity > 0.0f) || !std::isfinite(otherDensity)) {
            core::warn("physics: voxel shape density {} is not usable; using 1000", otherDensity);
            otherDensity = 1000.0f;
        }
        bool hasMaterials = false;
        for (const utils::CollisionPiece& piece : pieces.pieces) {
            if (piece.material >= MAX_PHYSICS_MATERIALS) {
                core::warn("physics: voxel piece material {} is out of range; the shape is refused", piece.material);
                return nullptr;
            }
            hasMaterials = hasMaterials || piece.material != 0;
        }
        // Without materials, exactly the shape there always was: Jolt's default density and
        // material, the body's mass set from its volume.
        auto boxFor = [&](const utils::CollisionPiece& piece) -> JPH::RefConst<JPH::Shape> {
            core::vec3 half = 0.5f * (piece.max - piece.min);
            if (!finite(half) || half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) return nullptr;
            float convexRadius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({half.x, half.y, half.z}));
            if (!hasMaterials) return new JPH::BoxShape(toJolt(half), convexRadius);
            uint16_t m = piece.material;
            auto* box = new JPH::BoxShape(toJolt(half), convexRadius, m ? impl->joltMaterial(m) : nullptr);
            box->SetDensity(m && impl->materialDefined[m] ? impl->materials[m].density : otherDensity);
            return box;
        };
        float volume = 0.0f;
        JPH::RefConst<JPH::Shape> shape;
        if (pieces.pieces.size() == 1) {
            const utils::CollisionPiece& piece = pieces.pieces[0];
            JPH::RefConst<JPH::Shape> box = boxFor(piece);
            if (!box) return nullptr;
            core::vec3 size = piece.max - piece.min;
            volume = size.x * size.y * size.z;
            shape = new JPH::RotatedTranslatedShape(toJolt(0.5f * (piece.min + piece.max)), JPH::Quat::sIdentity(), box);
        } else {
            JPH::StaticCompoundShapeSettings compound;
            for (uint32_t i = 0; i < pieces.pieces.size(); i++) {
                const utils::CollisionPiece& piece = pieces.pieces[i];
                JPH::RefConst<JPH::Shape> box = boxFor(piece);
                if (!box) {
                    core::warn("physics: voxel piece {} has no size; the shape is refused", i);
                    return nullptr;
                }
                core::vec3 size = piece.max - piece.min;
                volume += size.x * size.y * size.z;
                // The piece index rides along as user data, so a hit sub-shape leads back to voxels.
                compound.AddShape(toJolt(0.5f * (piece.min + piece.max)), JPH::Quat::sIdentity(), box, i);
            }
            JPH::Shape::ShapeResult result = compound.Create(*impl->tempAllocator);
            if (result.HasError()) {
                core::warn("physics: voxel compound shape failed: {}", result.GetError().c_str());
                return nullptr;
            }
            shape = result.Get();
        }
        auto out = std::make_shared<CollisionShape>();
        out->pieces = uint32_t(pieces.pieces.size());
        out->volume = volume;
        out->fallback = pieces.fallback;
        out->hasMaterials = hasMaterials;
        out->otherDensity = otherDensity;
        out->source = std::make_shared<const utils::CollisionPieces>(pieces);
        out->native = std::make_shared<CollisionShape::Native>();
        out->native->shape = shape;
        return out;
    }

    CollisionShapeRef PhysicsWorld::voxelShape(const GeometryBlob& blob, uint32_t resolution,
                                               const utils::CollisionParams& params,
                                               const utils::SlotMaterials* materials, float otherDensity) {
        // The materials' part of the key: 0 without any, so those shapes are shared as they always
        // were; otherwise the slots, the material-0 density and the table's version (densities are
        // baked into the shape).
        uint64_t materialKey = 0;
        if (materials && std::any_of(materials->begin(), materials->end(), [](uint16_t m) { return m != 0; })) {
            Hasher h;
            h.bytes(materials->data(), sizeof(uint16_t) * materials->size());
            h.add(otherDensity);
            h.add(impl->materialsVersion);
            materialKey = h.value | 1u;
        } else {
            materials = nullptr;
        }
        auto key = std::make_tuple(blob.contentStamp, params.key(), materialKey);
        auto found = impl->shapeCache.find(key);
        if (found != impl->shapeCache.end()) return found->second;
        utils::CollisionPieces pieces = utils::buildCollisionPieces(blob, resolution, params, materials);
        CollisionShapeRef shape = shapeFromPieces(pieces, otherDensity);
        // An empty blob is cached as "no shape" too: asking again is as cheap as the first answer.
        impl->shapeCache.emplace(key, shape);
        return shape;
    }

    bool PhysicsWorld::setMaterial(uint16_t index, const PhysicsMaterial& material) {
        if (index == 0 || index >= MAX_PHYSICS_MATERIALS) {
            core::warn("physics: refused material {}: indices are 1..{}", index, MAX_PHYSICS_MATERIALS - 1);
            return false;
        }
        if (!(material.density > 0.0f) || !std::isfinite(material.density) || !(material.friction >= 0.0f) ||
            !std::isfinite(material.friction) || !std::isfinite(material.restitution)) {
            core::warn("physics: refused material {}: density {}, friction {}, restitution {}", index, material.density,
                       material.friction, material.restitution);
            return false;
        }
        PhysicsMaterial m = material;
        m.restitution = std::clamp(m.restitution, 0.0f, 1.0f);
        if (impl->recording) {
            impl->op(Op::SetMaterial);
            put(impl->log, index); put(impl->log, m.density); put(impl->log, m.friction); put(impl->log, m.restitution);
        }
        impl->materials[index] = m;
        impl->materialDefined[index] = 1;
        impl->materialsVersion++;
        impl->contacts.any = true;
        return true;
    }

    PhysicsMaterial PhysicsWorld::material(uint16_t index) const {
        return index < MAX_PHYSICS_MATERIALS && impl->materialDefined[index] ? impl->materials[index] : PhysicsMaterial{};
    }

    void PhysicsWorld::pruneShapeCache() {
        for (auto it = impl->shapeCache.begin(); it != impl->shapeCache.end();) {
            const auto& shape = it->second;
            // Unused: only the cache holds our handle, and only that handle holds the Jolt shape (a
            // body, or a scaled wrapper a body holds, adds a reference of its own).
            bool unused = !shape || (shape.use_count() == 1 && shape->native.use_count() == 1 &&
                                     shape->native->shape->GetRefCount() == 1);
            it = unused ? impl->shapeCache.erase(it) : std::next(it);
        }
    }

    size_t PhysicsWorld::cachedShapeCount() const { return impl->shapeCache.size(); }

    std::vector<uint8_t> PhysicsWorld::saveState() const {
        std::vector<uint8_t> out;
        std::vector<uint32_t> bodies = impl->sortedBodies();
        put(out, SNAPSHOT_MAGIC);
        put(out, SNAPSHOT_VERSION);
        put(out, impl->tick);
        put(out, static_cast<uint32_t>(bodies.size()));
        for (uint32_t b : bodies) {
            put(out, b);
            const Impl::Rest& r = impl->rest[JPH::BodyID(b).GetIndex()];
            put(out, r.position);
            put(out, r.rotation);
            put(out, r.seconds);
        }

        JPH::StateRecorderImpl recorder;
        impl->system.SaveState(recorder, JPH::EStateRecorderState::All);
        std::string data = recorder.GetData();
        put(out, static_cast<uint64_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());

        // Characters: Jolt's own state for each (the system's snapshot does not include them), and
        // the input they are acting on.
        uint32_t live = 0;
        for (const Impl::Character& c : impl->characters) live += c.controller ? 1 : 0;
        put(out, live);
        for (size_t slot = 0; slot < impl->characters.size(); slot++) {
            const Impl::Character& c = impl->characters[slot];
            if (!c.controller) continue;
            put(out, uint32_t(slot) | (uint32_t(c.generation) << 16));
            put(out, c.input); put(out, uint8_t(c.jump)); put(out, c.jumpSpeed);
            JPH::StateRecorderImpl state;
            c.controller->SaveState(state);
            std::string bytes = state.GetData();
            put(out, static_cast<uint64_t>(bytes.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
        }

        // Joints: which exist (their state is in Jolt's own bytes above), and what each held with.
        uint32_t liveJoints = 0;
        for (const Impl::Joint& j : impl->joints) liveJoints += j.constraint ? 1 : 0;
        put(out, liveJoints);
        for (size_t slot = 0; slot < impl->joints.size(); slot++) {
            const Impl::Joint& j = impl->joints[slot];
            if (!j.constraint) continue;
            put(out, uint32_t(slot) | (uint32_t(j.generation) << 16));
            put(out, uint8_t(j.desc.motor)); put(out, j.desc.motorVelocity); put(out, j.force); put(out, j.torque);
            put(out, uint8_t(j.desc.motorMode)); put(out, j.desc.motorTarget);
        }
        return out;
    }

    bool PhysicsWorld::restoreState(const std::vector<uint8_t>& snapshot) {
        if (impl->recording) { impl->op(Op::Restore); put(impl->log, uint64_t(snapshot.size())); impl->log.insert(impl->log.end(), snapshot.begin(), snapshot.end()); }
        size_t at = 0;
        uint32_t magic = 0, version = 0, count = 0;
        uint64_t tick = 0, joltSize = 0;
        if (!get(snapshot, at, magic) || magic != SNAPSHOT_MAGIC || !get(snapshot, at, version) ||
            version != SNAPSHOT_VERSION || !get(snapshot, at, tick) || !get(snapshot, at, count)) {
            core::warn("physics: restoreState refused: not a physics snapshot (or another version)");
            return false;
        }
        std::vector<uint32_t> saved(count);
        std::vector<Impl::Rest> savedRest(count);
        for (uint32_t i = 0; i < count; i++)
            if (!get(snapshot, at, saved[i]) || !get(snapshot, at, savedRest[i].position) ||
                !get(snapshot, at, savedRest[i].rotation) || !get(snapshot, at, savedRest[i].seconds)) {
                core::warn("physics: restoreState refused: the snapshot is truncated");
                return false;
            }
        if (!get(snapshot, at, joltSize) || snapshot.size() - at < joltSize) {
            core::warn("physics: restoreState refused: the snapshot is truncated");
            return false;
        }
        const size_t joltAt = at;
        at += joltSize;
        struct SavedCharacter { uint32_t id; core::vec3 input; uint8_t jump; float jumpSpeed; std::vector<uint8_t> state; };
        std::vector<SavedCharacter> savedCharacters;
        uint32_t characterCount = 0;
        if (!get(snapshot, at, characterCount)) {
            core::warn("physics: restoreState refused: the snapshot is truncated");
            return false;
        }
        for (uint32_t i = 0; i < characterCount; i++) {
            SavedCharacter c;
            uint64_t size = 0;
            if (!get(snapshot, at, c.id) || !get(snapshot, at, c.input) || !get(snapshot, at, c.jump) ||
                !get(snapshot, at, c.jumpSpeed) || !get(snapshot, at, size) || snapshot.size() - at < size) {
                core::warn("physics: restoreState refused: the snapshot is truncated");
                return false;
            }
            c.state.assign(snapshot.begin() + long(at), snapshot.begin() + long(at + size));
            at += size;
            savedCharacters.push_back(std::move(c));
        }
        std::vector<uint32_t> liveCharacters;
        for (size_t slot = 0; slot < impl->characters.size(); slot++)
            if (impl->characters[slot].controller)
                liveCharacters.push_back(uint32_t(slot) | (uint32_t(impl->characters[slot].generation) << 16));
        struct SavedJoint { uint32_t id; uint8_t motor; float velocity, force, torque; uint8_t mode; float target; };
        std::vector<SavedJoint> savedJoints;
        uint32_t jointCount = 0;
        if (!get(snapshot, at, jointCount)) {
            core::warn("physics: restoreState refused: the snapshot is truncated");
            return false;
        }
        for (uint32_t i = 0; i < jointCount; i++) {
            SavedJoint j{};
            if (!get(snapshot, at, j.id) || !get(snapshot, at, j.motor) || !get(snapshot, at, j.velocity) ||
                !get(snapshot, at, j.force) || !get(snapshot, at, j.torque) || !get(snapshot, at, j.mode) ||
                !get(snapshot, at, j.target)) {
                core::warn("physics: restoreState refused: the snapshot is truncated");
                return false;
            }
            savedJoints.push_back(j);
        }
        std::vector<uint32_t> liveJoints;
        for (size_t slot = 0; slot < impl->joints.size(); slot++)
            if (impl->joints[slot].constraint) liveJoints.push_back(uint32_t(slot) | (uint32_t(impl->joints[slot].generation) << 16));
        bool sameJoints = liveJoints.size() == savedJoints.size();
        for (size_t i = 0; sameJoints && i < liveJoints.size(); i++) sameJoints = liveJoints[i] == savedJoints[i].id;
        if (!sameJoints) {
            core::warn("physics: restoreState refused: the snapshot's joints are not this world's");
            return false;
        }
        bool sameCharacters = liveCharacters.size() == savedCharacters.size();
        for (size_t i = 0; sameCharacters && i < liveCharacters.size(); i++) sameCharacters = liveCharacters[i] == savedCharacters[i].id;
        if (!sameCharacters) {
            core::warn("physics: restoreState refused: the snapshot's characters are not this world's");
            return false;
        }
        // Jolt restores body state into the bodies that exist and does not create or destroy any,
        // and a mismatch can fail partway through. So the set is checked first, here, where
        // refusing still changes nothing.
        if (saved != impl->sortedBodies()) {
            core::warn("physics: restoreState refused: the snapshot has {} bodies and this world has a "
                       "different set ({}); restore needs the bodies that existed when it was saved",
                       saved.size(), impl->system.GetNumBodies());
            return false;
        }

        // Keep the current state, so a failure inside Jolt can be undone rather than leaving a
        // half-restored world.
        JPH::StateRecorderImpl backup;
        impl->system.SaveState(backup, JPH::EStateRecorderState::All);

        JPH::StateRecorderImpl recorder;
        recorder.WriteBytes(snapshot.data() + joltAt, joltSize);
        recorder.Rewind();
        if (!impl->system.RestoreState(recorder)) {
            core::error("physics: restoreState: Jolt rejected the snapshot; the previous state is kept");
            backup.Rewind();
            impl->system.RestoreState(backup);
            return false;
        }
        impl->tick = tick;
        for (uint32_t i = 0; i < count; i++) impl->rest[JPH::BodyID(saved[i]).GetIndex()] = savedRest[i];
        for (const SavedCharacter& saved : savedCharacters) {
            Impl::Character& c = *impl->character(CharacterId{saved.id});
            c.input = saved.input;
            c.jump = saved.jump != 0;
            c.jumpSpeed = saved.jumpSpeed;
            JPH::StateRecorderImpl state;
            state.WriteBytes(saved.state.data(), saved.state.size());
            state.Rewind();
            c.controller->RestoreState(state);
        }
        for (const SavedJoint& saved : savedJoints) {
            Impl::Joint& j = *impl->joint(JointId{saved.id});
            j.desc.motor = saved.motor != 0;
            j.desc.motorVelocity = saved.velocity;
            j.force = saved.force;
            j.torque = saved.torque;
            j.desc.motorMode = MotorMode(saved.mode);
            j.desc.motorTarget = saved.target;
        }
        return true;
    }

    uint64_t PhysicsWorld::stateHash() const {
        Hasher h;
        h.add(impl->tick);
        const JPH::BodyLockInterface& locks = impl->system.GetBodyLockInterface();
        for (uint32_t raw : impl->sortedBodies()) {
            JPH::BodyLockRead lock(locks, JPH::BodyID(raw));
            if (!lock.Succeeded()) continue;
            const JPH::Body& body = lock.GetBody();
            h.add(raw);
            h.add(JPH::Vec3(body.GetPosition()));
            h.add(body.GetRotation());
            h.add(body.GetLinearVelocity());
            h.add(body.GetAngularVelocity());
            h.add(static_cast<uint8_t>(body.IsActive()));
        }
        for (size_t slot = 0; slot < impl->characters.size(); slot++) {
            const Impl::Character& c = impl->characters[slot];
            if (!c.controller) continue;
            h.add(uint32_t(slot));
            h.add(JPH::Vec3(c.controller->GetPosition()));
            h.add(c.controller->GetLinearVelocity());
            h.add(uint8_t(c.controller->GetGroundState()));
        }
        for (size_t slot = 0; slot < impl->joints.size(); slot++)
            if (impl->joints[slot].constraint) h.add(uint32_t(slot) | (uint32_t(impl->joints[slot].generation) << 16));
        // The material table: two peers with a different stone are a mismatch, not a slow drift.
        for (uint16_t i = 1; i < MAX_PHYSICS_MATERIALS; i++)
            if (impl->materialDefined[i]) {
                const PhysicsMaterial& m = impl->materials[i];
                h.add(i); h.add(m.density); h.add(m.friction); h.add(m.restitution);
            }
        return h.value;
    }

    const PhysicsStats& PhysicsWorld::stats() const { return impl->stats; }

    namespace {
        class LayerMask final : public JPH::ObjectLayerFilter {
        public:
            explicit LayerMask(uint32_t mask) : mask(mask) {}
            bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer < 32 && ((mask >> layer) & 1u); }
        private:
            uint32_t mask;
        };

        class SkipOne final : public JPH::BodyFilter {
        public:
            explicit SkipOne(BodyId skip) : skip(skip) {}
            bool ShouldCollide(const JPH::BodyID& id) const override {
                return !skip.valid() || id.GetIndexAndSequenceNumber() != skip.value;
            }
        private:
            BodyId skip;
        };

        // The compound indices from a body's shape down to the sub-shape `id` names. Decorators
        // (rotated, translated, scaled) take no bits of the id, so they are passed through.
        std::vector<uint32_t> pathTo(const JPH::Shape* shape, JPH::SubShapeID id) {
            std::vector<uint32_t> path;
            while (shape) {
                JPH::EShapeSubType type = shape->GetSubType();
                if (type == JPH::EShapeSubType::StaticCompound || type == JPH::EShapeSubType::MutableCompound) {
                    const auto* compound = static_cast<const JPH::CompoundShape*>(shape);
                    JPH::SubShapeID rest;
                    uint32_t index = compound->GetSubShapeIndexFromID(id, rest);
                    if (index >= compound->GetNumSubShapes()) break;
                    path.push_back(compound->GetCompoundUserData(index));
                    shape = compound->GetSubShape(index).mShape;
                    id = rest;
                } else if (type == JPH::EShapeSubType::RotatedTranslated || type == JPH::EShapeSubType::Scaled ||
                           type == JPH::EShapeSubType::OffsetCenterOfMass) {
                    shape = static_cast<const JPH::DecoratedShape*>(shape)->GetInnerShape();
                } else {
                    break;
                }
            }
            return path;
        }
    }

    std::optional<PhysicsHit> PhysicsWorld::castRay(core::vec3 origin, core::vec3 direction, float maxDistance,
                                                    const QueryFilter& filter) const {
        if (!finite(origin) || !finite(direction) || !(maxDistance > 0.0f)) return std::nullopt;
        float length = glm::length(direction);
        if (length < 1e-6f) return std::nullopt;
        direction /= length;
        JPH::RRayCast ray(toJoltR(origin), toJolt(direction * maxDistance));
        JPH::RayCastResult result;
        LayerMask layers(filter.layers);
        SkipOne skip(filter.ignore);
        if (!impl->system.GetNarrowPhaseQuery().CastRay(ray, result, JPH::BroadPhaseLayerFilter(), layers, skip))
            return std::nullopt;
        PhysicsHit hit;
        hit.body = BodyId{result.mBodyID.GetIndexAndSequenceNumber()};
        hit.distance = result.mFraction * maxDistance;
        hit.position = origin + direction * hit.distance;
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            hit.normal = fromJolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, toJoltR(hit.position)));
            hit.path = pathTo(body.GetShape(), result.mSubShapeID2);
        }
        return hit;
    }

    std::optional<PhysicsHit> PhysicsWorld::castSphere(core::vec3 origin, float radius, core::vec3 direction,
                                                       float maxDistance, const QueryFilter& filter) const {
        if (!finite(origin) || !finite(direction) || !(radius > 0.0f) || !(maxDistance > 0.0f)) return std::nullopt;
        float length = glm::length(direction);
        if (length < 1e-6f) return std::nullopt;
        direction /= length;
        JPH::SphereShape sphere(radius);
        sphere.SetEmbedded();
        JPH::RShapeCast cast(&sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(toJoltR(origin)),
                             toJolt(direction * maxDistance));
        JPH::ShapeCastSettings settings;
        settings.mReturnDeepestPoint = true;
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        LayerMask layers(filter.layers);
        SkipOne skip(filter.ignore);
        impl->system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector,
                                                    JPH::BroadPhaseLayerFilter(), layers, skip);
        if (!collector.HadHit()) return std::nullopt;
        const JPH::ShapeCastResult& result = collector.mHit;
        PhysicsHit hit;
        hit.body = BodyId{result.mBodyID2.GetIndexAndSequenceNumber()};
        hit.distance = result.mFraction * maxDistance;
        hit.position = fromJolt(JPH::Vec3(result.mContactPointOn2));
        // The penetration axis points from the sphere into what it hit; the surface faces back.
        JPH::Vec3 axis = result.mPenetrationAxis;
        hit.normal = axis.LengthSq() > 1e-12f ? fromJolt(-axis.Normalized()) : -direction;
        JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), result.mBodyID2);
        if (lock.Succeeded()) hit.path = pathTo(lock.GetBody().GetShape(), result.mSubShapeID2);
        return hit;
    }

    namespace {
        std::vector<BodyId> overlap(const JPH::PhysicsSystem& system, const JPH::Shape& shape, JPH::RMat44Arg transform,
                                    const QueryFilter& filter) {
            JPH::CollideShapeSettings settings;
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            LayerMask layers(filter.layers);
            SkipOne skip(filter.ignore);
            system.GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sOne(), transform, settings, JPH::RVec3::sZero(),
                                                       collector, JPH::BroadPhaseLayerFilter(), layers, skip);
            std::vector<BodyId> out;
            for (const JPH::CollideShapeResult& r : collector.mHits) out.push_back(BodyId{r.mBodyID2.GetIndexAndSequenceNumber()});
            std::sort(out.begin(), out.end(), [](BodyId a, BodyId b) { return a.value < b.value; });
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }
    }

    std::vector<BodyId> PhysicsWorld::overlapSphere(core::vec3 centre, float radius, const QueryFilter& filter) const {
        if (!finite(centre) || !(radius > 0.0f)) return {};
        JPH::SphereShape sphere(radius);
        sphere.SetEmbedded();
        return overlap(impl->system, sphere, JPH::RMat44::sTranslation(toJoltR(centre)), filter);
    }

    std::vector<BodyId> PhysicsWorld::overlapBox(core::vec3 centre, core::vec3 halfExtents, core::quat rotation,
                                                 const QueryFilter& filter) const {
        if (!finite(centre) || !finite(halfExtents) || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
            return {};
        JPH::BoxShape box(toJolt(halfExtents), std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({halfExtents.x, halfExtents.y, halfExtents.z})));
        box.SetEmbedded();
        return overlap(impl->system, box, JPH::RMat44::sRotationTranslation(toJolt(glm::normalize(rotation)), toJoltR(centre)), filter);
    }

    PhysicsLog PhysicsWorld::recording() const {
        PhysicsLog log;
        if (!impl->recording) return log;
        log.bytes = impl->log;
        log.steps = impl->loggedSteps;
        return log;
    }

    ReplayResult PhysicsWorld::replay(const PhysicsLog& log) {
        ReplayResult result;
        const std::vector<uint8_t>& in = log.bytes;
        size_t at = 0;
        uint32_t magic = 0, version = 0;
        PhysicsSettings settings;
        if (!get(in, at, magic) || magic != LOG_MAGIC || !get(in, at, version) || version != LOG_VERSION ||
            !readSettings(in, at, settings)) {
            result.problem = "not a physics recording (or another version)";
            return result;
        }
        settings.record = false;
        PhysicsWorld world(settings);
        auto build = [&](const utils::CollisionPieces& pieces, float otherDensity) {
            return world.shapeFromPieces(pieces, otherDensity);
        };
        auto truncated = [&] {
            result.problem = "the recording is truncated after step " + std::to_string(result.stepsReplayed);
            return result;
        };
        while (at < in.size()) {
            uint8_t raw = in[at++];
            uint32_t id = 0;
            core::vec3 a{0.0f}, b{0.0f};
            core::quat q{1.0f, 0.0f, 0.0f, 0.0f};
            float dt = 0.0f;
            switch (Op(raw)) {
                case Op::Create: {
                    BodyDesc desc;
                    uint32_t expected = 0;
                    if (!readDesc(in, at, desc, build) || !get(in, at, expected)) return truncated();
                    BodyId made = world.createBody(desc);
                    if (made.value != expected) {
                        result.firstDivergentStep = result.stepsReplayed + 1;
                        result.problem = "a body was given a different id than when recorded";
                        return result;
                    }
                    break;
                }
                case Op::Destroy:
                    if (!get(in, at, id)) return truncated();
                    world.destroyBody(BodyId{id});
                    break;
                case Op::Impulse:
                    if (!get(in, at, id) || !get(in, at, a)) return truncated();
                    world.addImpulse(BodyId{id}, a);
                    break;
                case Op::AngularImpulse:
                    if (!get(in, at, id) || !get(in, at, a)) return truncated();
                    world.addAngularImpulse(BodyId{id}, a);
                    break;
                case Op::AddVelocity:
                    if (!get(in, at, id) || !get(in, at, a)) return truncated();
                    world.addVelocity(BodyId{id}, a);
                    break;
                case Op::SetVelocity:
                    if (!get(in, at, id) || !get(in, at, a) || !get(in, at, b)) return truncated();
                    world.setVelocity(BodyId{id}, a, b);
                    break;
                case Op::SetPose:
                    if (!get(in, at, id) || !get(in, at, a) || !get(in, at, q)) return truncated();
                    world.setPose(BodyId{id}, a, q);
                    break;
                case Op::MoveKinematic:
                    if (!get(in, at, id) || !get(in, at, a) || !get(in, at, q) || !get(in, at, dt)) return truncated();
                    world.moveKinematic(BodyId{id}, a, q, dt);
                    break;
                case Op::Gravity:
                    if (!get(in, at, a)) return truncated();
                    world.setGravity(a);
                    break;
                case Op::Step: {
                    uint64_t expected = 0;
                    if (!get(in, at, dt) || !get(in, at, expected)) return truncated();
                    world.step(dt);
                    result.stepsReplayed++;
                    if (world.stateHash() != expected) {
                        result.firstDivergentStep = result.stepsReplayed;
                        result.problem = "the state after step " + std::to_string(result.stepsReplayed) +
                                         " differs from the recording";
                        return result;
                    }
                    break;
                }
                case Op::CreateCharacter: {
                    CharacterDesc d;
                    uint8_t layer = 0;
                    uint32_t expected = 0;
                    if (!get(in, at, d.radius) || !get(in, at, d.height) || !get(in, at, d.position) ||
                        !get(in, at, d.maxSlope) || !get(in, at, d.mass) || !get(in, at, d.maxStrength) ||
                        !get(in, at, d.stepUp) || !get(in, at, d.stickDown) || !get(in, at, layer) || !get(in, at, expected))
                        return truncated();
                    d.layer = PhysicsLayer(layer);
                    if (world.createCharacter(d).value != expected) {
                        result.firstDivergentStep = result.stepsReplayed + 1;
                        result.problem = "a character was given a different id than when recorded";
                        return result;
                    }
                    break;
                }
                case Op::DestroyCharacter:
                    if (!get(in, at, id)) return truncated();
                    world.destroyCharacter(CharacterId{id});
                    break;
                case Op::CharacterInput: {
                    uint8_t jump = 0;
                    if (!get(in, at, id) || !get(in, at, a) || !get(in, at, jump) || !get(in, at, dt)) return truncated();
                    world.setCharacterInput(CharacterId{id}, a, jump != 0, dt);
                    break;
                }
                case Op::CharacterPosition:
                    if (!get(in, at, id) || !get(in, at, a)) return truncated();
                    world.setCharacterPosition(CharacterId{id}, a);
                    break;
                case Op::CreateJoint: {
                    JointDesc d;
                    uint32_t expected = 0;
                    if (!readJointDesc(in, at, d) || !get(in, at, expected)) return truncated();
                    if (world.createJoint(d).value != expected) {
                        result.firstDivergentStep = result.stepsReplayed + 1;
                        result.problem = "a joint was given a different id than when recorded";
                        return result;
                    }
                    break;
                }
                case Op::DestroyJoint:
                    if (!get(in, at, id)) return truncated();
                    world.destroyJoint(JointId{id});
                    break;
                case Op::JointMotor: {
                    uint8_t on = 0, mode = 0;
                    if (!get(in, at, id) || !get(in, at, on) || !get(in, at, mode) || !get(in, at, dt)) return truncated();
                    world.setJointMotor(JointId{id}, on != 0, MotorMode(mode), dt);
                    break;
                }
                case Op::SetMaterial: {
                    uint16_t index = 0;
                    PhysicsMaterial m;
                    if (!get(in, at, index) || !get(in, at, m.density) || !get(in, at, m.friction) ||
                        !get(in, at, m.restitution))
                        return truncated();
                    world.setMaterial(index, m);
                    break;
                }
                case Op::Restore: {
                    uint64_t size = 0;
                    if (!get(in, at, size) || in.size() - at < size) return truncated();
                    std::vector<uint8_t> snapshot(in.begin() + long(at), in.begin() + long(at + size));
                    at += size;
                    world.restoreState(snapshot);
                    break;
                }
                default:
                    result.problem = "an unknown record after step " + std::to_string(result.stepsReplayed);
                    return result;
            }
        }
        result.matched = true;
        return result;
    }

    bool PhysicsLog::saveToFile(const std::string& path) const {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return bool(file);
    }

    bool PhysicsLog::loadFromFile(const std::string& path, PhysicsLog& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        out.steps = 0;   // counted again by replay
        return true;
    }
}
