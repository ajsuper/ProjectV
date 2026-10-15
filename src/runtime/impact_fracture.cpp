// Breaking on impact: a system over the physics, not part of it. It reads the last step's impacts
// and strikes what can break, through the same public calls a game would use -- so a game that wants
// other rules leaves it out and writes its own (the physics plan: mechanism in the engine, policy in
// systems).
#include "runtime/physics.h"

namespace projv::runtime {
    namespace {
        struct ImpactFracture {
            ImpactFractureConfig config;
        };

        // Whether a hit may break `entity`: a voxel body that asked to (breaksOnImpact).
        bool breaksOnImpact(const World& world, Entity entity) {
            if (const RigidBody* rigid = world.try_get<RigidBody>(entity))
                return rigid->breaksOnImpact && rigid->shape == RigidBody::Shape::Voxels;
            const StaticCollider* fixed = world.try_get<StaticCollider>(entity);
            return fixed && fixed->breaksOnImpact;
        }

        // After each step: every side of every impact that can break is struck, at the next step, in
        // the order the impacts come -- which every peer agrees on.
        void strikeImpacts(Application& app) {
            World& world = app.world;
            const ImpactFractureConfig& config = world.ctx().get<ImpactFracture>().config;
            for (const BodyImpact& hit : impacts(world)) {
                const Entity sides[2] = {hit.a, hit.b};
                const core::vec3 velocities[2] = {hit.velocityA, hit.velocityB};
                for (int k = 0; k < 2; k++) {
                    const Entity struck = sides[k], striker = sides[1 - k];
                    if (!world.valid(struck) || !breaksOnImpact(world, struck) || !canBreak(world, struck)) continue;
                    Strike strike;
                    strike.at = hit.point;
                    strike.energy = hit.energy;
                    strike.striker = striker;
                    strike.minimumBreak = config.minimumBreak;
                    // The body's motion from before the hit: the contact has since stopped it.
                    strike.bodyVelocity = velocities[k];
                    if (config.shardsTakeStrikerMotion) strike.push = velocities[1 - k] - velocities[k];
                    strike.returnUnusedEnergy = config.returnUnusedEnergy;
                    strike.strikerVelocity = velocities[1 - k];
                    strike.shardsBreakOnImpact = config.shardsBreakOnImpact;
                    fracture(world, struck, strike);
                }
            }
        }
    }

    void installImpactFracture(Application& app, const ImpactFractureConfig& config) {
        World& world = app.world;
        if (world.ctx().contains<ImpactFracture>()) return;
        world.ctx().emplace<ImpactFracture>(ImpactFracture{config});
        app.addSystem(Stage::FixedUpdate, "physics: impact fracture", strikeImpacts, {.after = "physics"});
    }
}
