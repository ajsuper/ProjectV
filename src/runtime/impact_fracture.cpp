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

        // Whether a hit carrying `energy` may break `entity`: a voxel body that asked to
        // (breaksOnImpact), and the hit at least its minImpactEnergy.
        bool breaksOnImpact(const World& world, Entity entity, float energy) {
            if (const RigidBody* rigid = world.try_get<RigidBody>(entity))
                return rigid->breaksOnImpact && rigid->shape == RigidBody::Shape::Voxels && energy >= rigid->minImpactEnergy;
            const StaticCollider* fixed = world.try_get<StaticCollider>(entity);
            return fixed && fixed->breaksOnImpact && energy >= fixed->minImpactEnergy;
        }

        // After each step: every side of every impact that can break is struck, at the next step, in
        // the order the impacts come -- which every peer agrees on.
        void strikeImpacts(Application& app) {
            World& world = app.world;
            const ImpactFractureConfig& config = world.ctx().get<ImpactFracture>().config;
            for (const BodyImpact& hit : impacts(world)) {
                // What the hardest-hit point meets: a body landing on many points spreads its energy
                // over them, and only the worst of them cracks -- a boat set down hard chips at its
                // keel, rather than making the crater its whole energy would.
                const float energy = hit.energy * hit.share;
                const Entity sides[2] = {hit.a, hit.b};
                const core::vec3 velocities[2] = {hit.velocityA, hit.velocityB};
                for (int k = 0; k < 2; k++) {
                    const Entity struck = sides[k], striker = sides[1 - k];
                    if (!world.valid(struck) || !breaksOnImpact(world, struck, energy) || !canBreak(world, struck)) continue;
                    Strike strike;
                    strike.at = hit.point;
                    strike.energy = energy;
                    strike.striker = striker;
                    strike.cause = BreakCause::Impact;
                    // The body's motion from before the hit: the contact has since stopped it.
                    strike.bodyVelocity = velocities[k];
                    if (config.shardsTakeStrikerMotion) strike.push = velocities[1 - k] - velocities[k];
                    strike.returnUnusedEnergy = config.returnUnusedEnergy;
                    strike.strikerVelocity = velocities[1 - k];
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
