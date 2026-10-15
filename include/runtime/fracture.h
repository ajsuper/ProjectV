#ifndef PROJECTV_RUNTIME_FRACTURE_H
#define PROJECTV_RUNTIME_FRACTURE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/world.h"
#include "runtime/physics/physics_world.h"

// Fracture patterns: how a material breaks (runtime::fracture, in runtime/physics.h).
//
// A strike breaks the voxels within a reach set by its energy and the struck material's strength.
// *Which* of them go together as one shard is the pattern's call, and only that: a pattern is
// handed the voxels in reach and says, for each, which shard it joins or that it stays. The engine
// does everything else -- cuts the shards out, keeps every voxel's palette entry, makes each shard
// a body moving as that part of the body was, gives it a NetId when the body had one -- and holds
// the result to its rules whatever the pattern said: a voxel whose material does not break stays,
// a shard is cut into its face-connected parts, and one bigger than a chunk is cut in two.
//
//     runtime::registerFracturePattern(world, "slabs", [](const FractureInput& in, FractureRandom&) {
//         std::vector<int32_t> shard(in.voxels.size(), -1);
//         for (size_t i = 0; i < in.voxels.size(); i++)
//             if (glm::distance(core::vec3(in.voxels[i].position) + 0.5f, in.at) <= in.reach)
//                 shard[i] = in.voxels[i].position.y / 4;   // breaks in layers four voxels thick
//         return shard;
//     });
//     // physics_materials.json: { "name": "shale", "strength": 2e5, "fracture": "slabs" }
//
// **A pattern is part of the simulation.** Its shards become bodies, so it must give the same
// answer for the same input on every machine and in every replay: no clocks, no rand(), no
// unordered iteration, no state kept between calls. FractureRandom is the randomness it may use --
// seeded from the body, the step and the strike. Every peer registers the same patterns under the
// same names; a material names its pattern, and the library is in the physics state hash, so peers
// whose materials differ are told apart at once (peers whose *code* differs are not: keep it
// versioned with the game).
namespace projv::runtime {
    // A voxel a strike reaches, in the struck component's voxel coordinates.
    struct FractureVoxel {
        core::ivec3 position{0};
        uint8_t     slot = 0;            // its palette entry
        uint16_t    material = 0;        // its physics material (the body's own where its entry names none)
        bool        breakable = false;   // that material has a strength
    };

    // What a pattern is given. Lengths are in voxels of the struck component and directions in its
    // frame; a voxel's centre is its position + 0.5.
    struct FractureInput {
        const std::vector<FractureVoxel>& voxels;   // every voxel within `outer` of `at`, in z, y, x order
        core::vec3 at{0.0f};                        // the strike
        float      reach = 0.0f;                    // what it breaks: the struck material's strength against the energy
        float      outer = 0.0f;                    // 1.5 reach: voxels beyond reach, for an edge that is not a circle
        float      shardSize = 1.0f;                // the material's
        core::vec3 grain{0.0f, 1.0f, 0.0f};         // the material's, unit length
        const PhysicsMaterial& material;            // the struck voxel's
    };

    // The randomness a pattern may use (splitmix64): the same numbers for the same strike on every
    // machine.
    class FractureRandom {
    public:
        explicit FractureRandom(uint64_t seed) : state(seed) {}
        uint64_t next() {
            uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        // In [low, high).
        float uniform(float low = 0.0f, float high = 1.0f) {
            return low + (high - low) * float(next() >> 40) * (1.0f / 16777216.0f);
        }
    private:
        uint64_t state;
    };

    // For each voxel of `voxels`, in order: the shard it joins (any number from 0; voxels with the
    // same number go together) or -1 for "stays". The same length as `voxels`, or nothing breaks.
    using FracturePattern = std::function<std::vector<int32_t>(const FractureInput&, FractureRandom&)>;

    // Registers (or replaces) a pattern for materials to name. installPhysics registers the four
    // built in. False, logged, for an empty name or pattern.
    bool registerFracturePattern(World& world, const std::string& name, FracturePattern pattern);
    // Null when there is none of that name.
    const FracturePattern* fracturePattern(const World& world, const std::string& name);

    // The built-in patterns, as functions: for wrapping in a pattern of your own, and for tests.
    namespace fracture_patterns {
        // Stone, glass: Voronoi cells round seeds scattered among the voxels, small shards at the
        // strike growing to shardSize at the reach. Seeds beyond the reach stay, so the break's edge
        // runs along cell boundaries, jagged, rather than round a sphere.
        std::vector<int32_t> shatter(const FractureInput& in, FractureRandom& random);
        // Wood: as shatter, but the cells are three times longer along the grain than across it.
        std::vector<int32_t> splinter(const FractureInput& in, FractureRandom& random);
        // Plaster, earth, snow: cells of shardSize throughout, and the break round, at the reach.
        std::vector<int32_t> crumble(const FractureInput& in, FractureRandom& random);
        // Brick, planks: apart where palette entries meet first. Each run of touching voxels of one
        // entry that the strike reaches comes away whole -- a brick, a plank -- and is crumbled
        // further only when it is much bigger than shardSize. Neighbouring bricks need different
        // entries (two shades are enough) to come apart.
        std::vector<int32_t> seams(const FractureInput& in, FractureRandom& random);
    }

    // What the engine makes of a pattern's answer: the shards, each a list of indices into
    // `in.voxels`, ascending, the shards ordered by their first voxel. A voxel that is not
    // breakable stays whatever the pattern said; a shard is cut into its face-connected parts, and
    // one more than `maxExtent` voxels across into halves until it fits. At most `maxShards` (the
    // rest stay, logged).
    std::vector<std::vector<size_t>> fractureShards(const FractureInput& in, const std::vector<int32_t>& shardOf,
                                                    int maxExtent = 64, size_t maxShards = 512);
}

#endif
