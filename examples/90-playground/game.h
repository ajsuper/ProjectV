// The playground's rules: what only the server runs. Players and their tools, the things they make,
// blasts and their chains of TNT, the gravity, and putting the world back. playground_server runs it
// alone; a client started without --host runs it beside itself in the same process, and others can
// join that.
//
// Everything a player does reaches here as a request (runtime::sendToServer, read in onMessage) or
// as walking input (applyInput). What breaks reaches every client by itself: the structures, the
// blocks and every piece have NetIds, so their edits and breaks are replicated (runtime/replication.h).
#pragma once

#include <deque>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "core/application.h"
#include "core/log.h"
#include "net/transport.h"
#include "net/wire.h"
#include "playground.h"
#include "runtime/entities.h"
#include "runtime/net.h"
#include "runtime/physics.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/scene_query.h"

namespace playground {
    using projv::Entity;
    using projv::NullEntity;
    using projv::World;
    namespace runtime = projv::runtime;

    struct ServerOptions {
        uint16_t    port = PORT;
        std::string folder;   // the scene folder: "playground" beside the binary
    };

    // A player, on the entity its character is.
    struct Player {
        std::string name;
        runtime::ClientId client = 0;
        int   slot = 0;                      // its colour
        float yaw = 0.0f, shownYaw = 1e9f;
        float cooldown[TOOLS] = {};
        std::deque<Entity> blocks;           // what it built, oldest first
    };

    // On what a player made: a cannonball, TNT, a crate, a ball. `fuse` is TNT's: it goes off when it
    // runs out, or sooner when jolted (after `TNT_ARMED`) or caught in a blast.
    struct Thing {
        Tool   tool = Cannon;
        float  age = 0.0f;
        float  fuse = -1.0f;
        Entity by = NullEntity;
    };
    // On the crates and TNT the field starts with (put back with the world).
    struct Prop {};
    struct Built {};

    struct Game {
        ServerOptions options;
        // Everything that breaks, as the files have it: what a reset puts back.
        struct Structure { Entity entity = NullEntity; std::vector<projv::utils::ComponentVoxel> original; };
        std::vector<Structure> structures;
        std::deque<Entity> things;           // made by players, oldest first
        // A blast's push, a couple of steps after it (so its shards exist).
        struct Kick { vec3 at; float radius, speed; int steps; };
        std::vector<Kick> kicks;
        std::vector<std::pair<Entity, int>> newborn;   // shards still in their break's family, and steps left
        uint32_t nextFamily = 1000;
        bool     moon = false;
        float    stateClock = 0.0f, sweepClock = 0.0f;
        uint64_t shots = 0, blasts = 0, breaks = 0, resets = 0;   // for the server's report

        std::string prefab(const std::string& name) const { return (std::filesystem::path(options.folder) / name).string(); }
    };

    constexpr int SMALL_DEBRIS_VOXELS = 24;   // a piece this small is walked through
    constexpr int SHARD_FAMILY_STEPS = 15;    // a quarter second at 60 Hz

    // ---- Telling clients ----------------------------------------------------------------------------

    inline vec3 colourOf(int slot) { return PLAYER_COLOURS[std::clamp(slot, 0, MAX_PLAYERS - 1)]; }

    inline void banner(World& world, const std::string& text, vec3 colour, float seconds = 3.0f) {
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Banner));
        w.bytes(std::vector<uint8_t>(text.begin(), text.end()));
        writeVec(w, colour);
        w.f32(seconds);
        runtime::sendToClients(world, w.data(), true);
        projv::core::info("playground: {}", text);
    }

    inline void boom(World& world, vec3 at, BoomKind kind, float size = 1.0f) {
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Boom));
        writeVec(w, at);
        w.varint(uint8_t(kind));
        w.f32(size);
        runtime::sendToClients(world, w.data(), kind == BoomKind::Blast);
    }

    // A push on a player, told to its client, whose walking carries it.
    inline void knock(World& world, Entity e, vec3 velocity) {
        const Player& p = world.get<Player>(e);
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Knock));
        writeVec(w, velocity);
        runtime::sendToClient(world, p.client, w.data());
    }

    // ---- Players ------------------------------------------------------------------------------------

    inline vec3 feet(const World& world, Entity e) {
        if (world.all_of<projv::PhysicsCharacter>(e)) return runtime::characterState(world, e).position;
        const projv::Transform* t = world.try_get<projv::Transform>(e);
        return t ? t->position : vec3(0.0f);
    }
    inline vec3 headOf(const World& world, Entity e) { return feet(world, e) + vec3(0.0f, EYE_HEIGHT, 0.0f); }

    // Moves a character at once, still. The client that walks it is put right by its next snapshot.
    inline void moveCharacter(World& world, Entity e, vec3 to) {
        runtime::setCharacterInput(world, e, vec3(0.0f));
        if (const projv::PhysicsCharacter* c = world.try_get<projv::PhysicsCharacter>(e)) {
            runtime::physicsWorld(world).setCharacterPosition(c->id, to);
            runtime::physicsWorld(world).setCharacterVelocity(c->id, vec3(0.0f));
        } else {
            world.patch<projv::Transform>(e, [&](projv::Transform& t) { t.position = to; });
        }
    }

    inline int freeSlot(World& world) {
        for (int slot = 0; slot < MAX_PLAYERS; slot++) {
            bool taken = false;
            for (auto [e, p] : world.view<Player>().each()) taken = taken || p.slot == slot;
            if (!taken) return slot;
        }
        return -1;
    }

    inline Entity makePlayer(World& world, Game& game, runtime::ClientId client) {
        const int slot = freeSlot(world);
        if (slot < 0) return NullEntity;
        Entity e = runtime::instantiatePrefab(world, game.prefab(playerPrefab(slot)), spawnPoint(slot), facing(0.0f));
        if (e == NullEntity) return e;
        world.emplace<projv::CharacterController>(e);
        // Crumbs of debris are walked through.
        world.emplace<projv::CollisionGroups>(e, projv::CollisionGroups{{"player"}, {"small debris"}, 0});
        world.emplace<projv::Replicated>(e, projv::Replicated{playerPrefab(slot), {"physics.character"}});
        runtime::assignNetId(world, e);
        Player p;
        p.name = "PLAYER";
        p.client = client;
        p.slot = slot;
        p.yaw = std::atan2(castleCentre().z - spawnPoint(slot).z, castleCentre().x - spawnPoint(slot).x);
        world.emplace<Player>(e, std::move(p));
        return e;
    }

    // ---- Things -------------------------------------------------------------------------------------

    inline uint32_t solidLayers() {
        return runtime::layerBit(runtime::PhysicsLayer::Static) | runtime::layerBit(runtime::PhysicsLayer::Moving);
    }

    // A thing a player made, flying off from `from` at `velocity`. The oldest goes past MAX_THINGS.
    inline Entity launch(World& world, Game& game, Entity by, Tool tool, vec3 from, vec3 velocity) {
        static const std::map<Tool, const char*> prefabs = {{Cannon, CANNONBALL}, {Tnt, TNT}, {Crate, CRATE}, {Ball, BALL}};
        const char* name = prefabs.at(tool);
        Entity e = runtime::instantiatePrefab(world, game.prefab(name), from);
        if (e == NullEntity) return e;
        projv::RigidBody body;
        if (tool == Cannon || tool == Ball) body.shape = projv::RigidBody::Shape::Sphere;
        if (tool == Cannon) {
            body.material = "iron";
            body.quality = projv::RigidBody::Quality::Continuous;
        }
        if (tool == Ball) body.material = "rubber";
        if (tool == Tnt) body.shape = projv::RigidBody::Shape::Box;
        // A crate breaks where it lands hard; so does what a cannonball hits (the structures'
        // breaksOnImpact), not the ball.
        body.breaksOnImpact = tool == Crate;
        world.emplace<projv::RigidBody>(e, body);
        world.emplace<projv::Replicated>(e, projv::Replicated{name, {}});
        world.emplace<Thing>(e, Thing{tool, 0.0f, tool == Tnt ? TNT_FUSE : -1.0f, by});
        runtime::assignNetId(world, e);
        runtime::setVelocity(world, e, velocity, tool == Ball || tool == Tnt ? vec3(1.5f, 0.0f, 0.8f) : vec3(0.0f));
        game.things.push_back(e);
        while (int(game.things.size()) > MAX_THINGS) {
            if (world.valid(game.things.front())) world.destroy(game.things.front());
            game.things.pop_front();
        }
        return e;
    }

    // The field's crates and TNT, as it starts: a pyramid of crates east of the plaza, a stack of TNT
    // by the castle's gate.
    inline void makeProps(World& world, Game& game) {
        auto prop = [&](const char* name, vec3 at, bool breaks) {
            Entity e = runtime::instantiatePrefab(world, game.prefab(name), at);
            if (e == NullEntity) return;
            projv::RigidBody body;
            body.breaksOnImpact = breaks;
            if (std::string(name) == TNT) body.shape = projv::RigidBody::Shape::Box;
            world.emplace<projv::RigidBody>(e, body);
            world.emplace<projv::Replicated>(e, projv::Replicated{name, {}});
            world.emplace<Prop>(e);
            if (std::string(name) == TNT) world.emplace<Thing>(e, Thing{Tnt, 0.0f, 1e9f, NullEntity});
            runtime::assignNetId(world, e);
        };
        const vec3 crates = crateStack();
        for (int row = 0; row < 4; row++)
            for (int i = 0; i < 4 - row; i++)
                prop(CRATE, crates + vec3(float(i) * 1.04f + float(row) * 0.52f, 0.5f + float(row) * 1.01f, 0.0f), true);
        const vec3 tnt = tntStack();
        for (int i = 0; i < 4; i++) prop(TNT, tnt + vec3(float(i % 2) * 0.66f, 0.39f, float(i / 2) * 0.66f), false);
        prop(TNT, tnt + vec3(0.33f, 1.16f, 0.33f), false);
    }

    // ---- Blasts -------------------------------------------------------------------------------------

    // A blast at `at`: what a ray from it reaches within the radius is struck, with the energy falling
    // off with the square of the distance; two steps later everything near (the shards too) is thrown
    // outward; players near are knocked away; TNT near goes off a moment later.
    inline void explode(World& world, Game& game, vec3 at) {
        std::map<Entity, runtime::QueryHit> nearest;
        for (int i = -1; i <= 1; i++)
            for (int j = -1; j <= 1; j++)
                for (int k = -1; k <= 1; k++) {
                    if (i == 0 && j == 0 && k == 0) continue;
                    auto hit = runtime::castRay(world, at, glm::normalize(vec3(i, j, k)), BLAST_RADIUS, solidLayers());
                    if (!hit || hit->entity == NullEntity) continue;
                    auto [it, added] = nearest.try_emplace(hit->entity, *hit);
                    if (!added && hit->distance < it->second.distance) it->second = *hit;
                }
        for (auto& [e, hit] : nearest) {
            if (!runtime::canBreak(world, e)) continue;
            const float falloff = std::max(0.0f, 1.0f - hit.distance / BLAST_RADIUS);
            runtime::Strike strike;
            strike.at = hit.position + glm::normalize(hit.position - at + vec3(1e-4f)) * 0.1f;
            strike.energy = BLAST_ENERGY * falloff * falloff;
            runtime::fracture(world, e, strike);
        }
        game.kicks.push_back({at, BLAST_RADIUS * 1.8f, BLAST_KICK, 2});
        game.blasts++;
        for (auto [e, thing] : world.view<Thing>().each())
            if (thing.tool == Tnt && runtime::hasBody(world, e) && glm::distance(runtime::bodyPose(world, e).position, at) < BLAST_RADIUS * 1.2f)
                thing.fuse = std::min(thing.fuse, TNT_CHAIN);
        std::vector<std::pair<Entity, vec3>> pushed;
        for (auto [e, p] : world.view<Player>().each()) {
            vec3 d = feet(world, e) + vec3(0.0f, 0.9f, 0.0f) - at;
            const float distance = glm::length(d);
            if (distance > BLAST_RADIUS * 1.6f) continue;
            d.y = 0.0f;
            const vec3 dir = glm::length(d) > 0.05f ? glm::normalize(d) : vec3(1, 0, 0);
            pushed.push_back({e, dir * BLAST_KNOCK * (1.0f - 0.6f * distance / (BLAST_RADIUS * 1.6f))});
        }
        for (auto [e, v] : pushed) knock(world, e, v);
        boom(world, at, BoomKind::Blast, BLAST_RADIUS);
    }

    // ---- Tools --------------------------------------------------------------------------------------

    // Whether a player stands in a metre cell.
    inline bool someoneIn(World& world, vec3 lo, vec3 hi) {
        for (auto [e, p] : world.view<Player>().each()) {
            const vec3 f = feet(world, e);
            if (f.x + 0.3f > lo.x && f.x - 0.3f < hi.x && f.z + 0.3f > lo.z && f.z - 0.3f < hi.z && f.y + 1.8f > lo.y && f.y < hi.y) return true;
        }
        return false;
    }

    // A metre block of stone against the face the crosshair is on, on the metre grid.
    inline bool build(World& world, Game& game, Entity by, const runtime::QueryHit& hit) {
        Player& p = world.get<Player>(by);
        const vec3 cell = glm::floor(hit.position + hit.normal * 0.5f);
        const vec3 centre = cell + vec3(0.5f);
        if (!runtime::overlapBox(world, centre, vec3(0.45f), quat(1, 0, 0, 0), solidLayers()).empty()) return false;
        if (someoneIn(world, cell, cell + vec3(1.0f))) return false;
        Entity block = runtime::instantiatePrefab(world, game.prefab(BLOCK), vec3(centre.x, cell.y, centre.z));
        if (block == NullEntity) return false;
        projv::StaticCollider collider;
        collider.splits = true;
        collider.anchorBase = true;
        collider.breaksOnImpact = true;
        world.emplace<projv::StaticCollider>(block, collider);
        world.emplace<projv::Replicated>(block, projv::Replicated{BLOCK, {}});
        world.emplace<Built>(block);
        runtime::assignNetId(world, block);
        p.blocks.push_back(block);
        while (int(p.blocks.size()) > MAX_BLOCKS) {
            if (world.valid(p.blocks.front())) world.destroy(p.blocks.front());
            p.blocks.pop_front();
        }
        boom(world, centre, BoomKind::Puff, 0.5f);
        return true;
    }

    // Every voxel within DIG_RADIUS of where the crosshair is, of everything there that is shared
    // (has a NetId): what it cuts loose falls.
    inline bool dig(World& world, vec3 at) {
        bool dug = false;
        for (Entity e : runtime::overlapSphere(world, at, DIG_RADIUS, solidLayers())) {
            if (!world.all_of<projv::NetId>(e) || world.all_of<Player>(e)) continue;
            std::vector<projv::PendingVoxelOp> ops = runtime::voxelsInSphere(world, e, at, DIG_RADIUS);
            if (ops.empty()) continue;
            runtime::editVoxels(world, e, ops);
            dug = true;
        }
        if (dug) boom(world, at, BoomKind::Dust, DIG_RADIUS);
        return dug;
    }

    // Everything in a cone in front, thrown along it: bodies, and players (by their own walking).
    inline void push(World& world, Entity by, vec3 head, vec3 dir) {
        const float reach = REACH[Push];
        for (Entity e : runtime::overlapSphere(world, head, reach, runtime::layerBit(runtime::PhysicsLayer::Moving))) {
            if (!world.all_of<projv::RigidBody>(e) || world.get<projv::RigidBody>(e).motion != runtime::MotionType::Dynamic) continue;
            const vec3 d = runtime::bodyPose(world, e).centerOfMass - head;
            const float distance = glm::length(d);
            if (distance < 1e-3f || glm::dot(d / distance, dir) < PUSH_CONE) continue;
            runtime::addVelocity(world, e, (dir * PUSH_SPEED + vec3(0.0f, 3.0f, 0.0f)) * (1.0f - 0.5f * distance / reach));
        }
        std::vector<std::pair<Entity, vec3>> pushed;
        for (auto [e, p] : world.view<Player>().each()) {
            if (e == by) continue;
            const vec3 d = headOf(world, e) - head;
            const float distance = glm::length(d);
            if (distance > reach || distance < 1e-3f || glm::dot(d / distance, dir) < PUSH_CONE) continue;
            pushed.push_back({e, vec3(dir.x, 0.0f, dir.z) * PUSH_SPEED});
        }
        for (auto [e, v] : pushed) knock(world, e, v);
        boom(world, head + dir * 1.2f, BoomKind::Puff, 1.0f);
    }

    // A tool used: aimed from the client's camera at `eye` along `dir`. The camera must be near the
    // player's head; the ray is the server's own, skipping the player.
    inline void use(World& world, Game& game, Entity e, Tool tool, vec3 eye, vec3 dir) {
        Player& p = world.get<Player>(e);
        if (p.cooldown[tool] > 0.0f) return;
        const vec3 head = headOf(world, e);
        if (glm::distance(eye, head) > CAMERA_SLACK) eye = head;
        dir = glm::normalize(dir);
        const auto hit = runtime::castRay(world, eye, dir, REACH[tool] + glm::distance(eye, head), solidLayers(), e);
        // Built, dug or blown up within reach of the player, not of its camera.
        const bool inReach = hit && glm::distance(hit->position, head) <= REACH[tool];
        // What is thrown leaves from just in front of the head, toward what the crosshair is on.
        const vec3 target = hit ? hit->position : eye + dir * REACH[tool];
        vec3 toward = target - head;
        toward = glm::length(toward) > 0.5f ? glm::normalize(toward) : dir;
        const vec3 from = head + toward * 0.9f;
        bool used = true;
        switch (tool) {
            case Cannon:
                launch(world, game, e, Cannon, from, toward * CANNON_SPEED);
                boom(world, from, BoomKind::Muzzle, 0.6f);
                game.shots++;
                break;
            case Blast:
                used = inReach;
                if (used) explode(world, game, hit->position + hit->normal * 0.3f);
                break;
            case Tnt:   launch(world, game, e, Tnt, from, toward * TNT_THROW + vec3(0.0f, 2.0f, 0.0f)); break;
            case Crate: launch(world, game, e, Crate, head + toward * 1.4f, toward * THROW_SPEED); break;
            case Ball:  launch(world, game, e, Ball, head + toward * 1.6f, toward * THROW_SPEED); break;
            case Build: used = inReach && build(world, game, e, *hit); break;
            case Dig:   used = inReach && dig(world, hit->position - hit->normal * 0.2f); break;
            case Push:  push(world, e, head, dir); break;
        }
        if (used) p.cooldown[tool] = COOLDOWN[tool];
    }

    // ---- The world put back -------------------------------------------------------------------------

    // Everything as the files have it: what was built, thrown and broken off gone, what broke put back.
    inline void resetWorld(World& world, Game& game) {
        std::vector<Entity> doomed;
        for (Entity e : world.view<projv::Piece>()) doomed.push_back(e);
        for (Entity e : world.view<Thing>()) doomed.push_back(e);
        for (Entity e : world.view<Prop>()) doomed.push_back(e);
        for (Entity e : world.view<Built>()) doomed.push_back(e);
        std::sort(doomed.begin(), doomed.end());
        doomed.erase(std::unique(doomed.begin(), doomed.end()), doomed.end());
        for (Entity e : doomed) if (world.valid(e)) world.destroy(e);
        game.things.clear();
        game.kicks.clear();
        game.newborn.clear();
        for (auto [e, p] : world.view<Player>().each()) p.blocks.clear();

        auto& scene = world.ctx().get<projv::Scene>();
        for (const Game::Structure& s : game.structures) {
            const projv::ComponentHandle h = runtime::voxelComponentOf(world, s.entity);
            if (h == projv::INVALID_COMPONENT_HANDLE) continue;
            std::map<std::tuple<int, int, int>, uint8_t> present;
            for (const auto& v : projv::utils::componentVoxels(scene, h)) present[{v.position.x, v.position.y, v.position.z}] = v.slot;
            std::vector<projv::PendingVoxelOp> ops;
            const auto& palette = scene.components[h].materialPalette;
            for (const auto& v : s.original) {
                auto it = present.find({v.position.x, v.position.y, v.position.z});
                if (it != present.end() && it->second == v.slot) continue;
                projv::PendingVoxelOp op{true, v.position, v.slot < palette.size() ? palette[v.slot].packedColor : 0u};
                op.slot = v.slot;
                ops.push_back(op);
            }
            if (!ops.empty()) runtime::editVoxels(world, s.entity, ops);
        }
        makeProps(world, game);
        for (auto [e, p] : world.view<Player>().each()) moveCharacter(world, e, spawnPoint(p.slot));
        game.resets++;
    }

    // ---- Requests from players ----------------------------------------------------------------------

    inline void onMessage(World& world, Game& game, Entity player, std::span<const uint8_t> message) {
        if (player == NullEntity || !world.valid(player) || !world.all_of<Player>(player)) return;
        Player& p = world.get<Player>(player);
        projv::net::BitReader r(message);
        const auto kind = ToServer(r.varint());
        if (kind == ToServer::Look) {
            const float yaw = r.f32();
            if (!r.failed && std::isfinite(yaw)) p.yaw = wrapAngle(yaw);
        } else if (kind == ToServer::Use) {
            const uint64_t tool = r.varint();
            const vec3 eye = readVec(r), dir = readVec(r);
            if (r.failed || tool >= TOOLS || !finite(eye) || !finite(dir) || glm::length(dir) < 0.5f) return;
            use(world, game, player, Tool(tool), eye, dir);
        } else if (kind == ToServer::Gravity) {
            game.moon = !game.moon;
            runtime::setGravity(world, vec3(0.0f, -(game.moon ? MOON_GRAVITY : GRAVITY), 0.0f));
            banner(world, p.name + (game.moon ? ": MOON GRAVITY" : ": EARTH GRAVITY"), colourOf(p.slot), 2.0f);
        } else if (kind == ToServer::Reset) {
            resetWorld(world, game);
            banner(world, p.name + " REBUILT IT ALL", colourOf(p.slot), 2.5f);
        }
    }

    // ---- Each frame ---------------------------------------------------------------------------------

    inline void rules(projv::Application& app) {
        World& world = app.world;
        Game& game = world.ctx().get<Game>();
        const float dt = app.time().delta;
        if (dt <= 0.0f) return;

        // Players: cooldowns, facing, falls.
        std::vector<Entity> fell;
        for (auto [e, p] : world.view<Player>().each()) {
            for (float& c : p.cooldown) c = std::max(0.0f, c - dt);
            if (std::abs(p.yaw - p.shownYaw) > 0.01f) {
                p.shownYaw = p.yaw;
                world.patch<projv::Transform>(e, [&](projv::Transform& t) { t.rotation = facing(p.yaw); });
            }
            if (feet(world, e).y < FALLEN_BELOW) fell.push_back(e);
        }
        for (Entity e : fell) moveCharacter(world, e, spawnPoint(world.get<Player>(e).slot));

        // Things: cannonballs gone after a while, TNT going off.
        std::vector<Entity> spent, bang;
        for (auto [e, thing] : world.view<Thing>().each()) {
            thing.age += dt;
            if (thing.tool == Cannon && thing.age > CANNONBALL_SECONDS) spent.push_back(e);
            if (thing.tool == Tnt && (thing.fuse -= dt) <= 0.0f) bang.push_back(e);
        }
        for (Entity e : bang) {
            const vec3 at = runtime::hasBody(world, e) ? runtime::bodyPose(world, e).position : feet(world, e);
            world.destroy(e);
            explode(world, game, at);
        }
        for (Entity e : spent) if (world.valid(e)) world.destroy(e);

        // What has fallen off the field is gone.
        if ((game.sweepClock -= dt) <= 0.0f) {
            game.sweepClock = 0.5f;
            std::vector<Entity> lost;
            for (auto [e, body] : world.view<projv::RigidBody>().each())
                if (runtime::hasBody(world, e) && runtime::bodyPose(world, e).position.y < FALLEN_BELOW) lost.push_back(e);
            for (Entity e : lost) world.destroy(e);
            std::erase_if(game.things, [&](Entity e) { return !world.valid(e); });
        }

        // Each player told how things stand, ten times a second.
        if ((game.stateClock -= dt) <= 0.0f) {
            game.stateClock = 0.1f;
            State state;
            state.moon = game.moon;
            state.things = uint16_t(std::min<size_t>(game.things.size(), 65535));
            state.pieces = uint16_t(std::min<size_t>(world.view<projv::Piece>().size(), 65535));
            for (auto [e, p] : world.view<Player>().each()) {
                state.players[p.slot].present = true;
                state.players[p.slot].name = p.name;
            }
            for (auto [e, p] : world.view<Player>().each()) {
                state.you = int8_t(p.slot);
                for (int t = 0; t < TOOLS; t++) state.cooldown[t] = p.cooldown[t] / COOLDOWN[t];
                projv::net::BitWriter w;
                state.write(w);
                runtime::sendToClient(world, p.client, w.data(), false);
            }
        }
    }

    // FixedUpdate, after the physics: a blast's push, two steps on (so its shards exist); TNT jolted
    // hard going off; a break's shards let collide with each other again.
    inline void afterStep(projv::Application& app) {
        World& world = app.world;
        Game& game = world.ctx().get<Game>();
        for (Game::Kick& k : game.kicks) {
            if (--k.steps != 0) continue;
            for (Entity e : runtime::overlapSphere(world, k.at, k.radius, runtime::layerBit(runtime::PhysicsLayer::Moving))) {
                if (!world.all_of<projv::RigidBody>(e) || world.get<projv::RigidBody>(e).motion != runtime::MotionType::Dynamic) continue;
                const vec3 d = runtime::bodyPose(world, e).centerOfMass - k.at;
                const float distance = glm::length(d);
                if (distance < 1e-3f) continue;
                const float falloff = 1.0f - std::min(distance / k.radius, 1.0f);
                runtime::addVelocity(world, e, (d / distance * k.speed + vec3(0, 5, 0)) * falloff);
            }
        }
        std::erase_if(game.kicks, [](const Game::Kick& k) { return k.steps <= 0; });
        for (const runtime::BodyImpact& impact : runtime::impacts(world))
            for (Entity e : {impact.a, impact.b}) {
                Thing* thing = world.valid(e) ? world.try_get<Thing>(e) : nullptr;
                if (thing && thing->tool == Tnt && thing->age > TNT_ARMED && impact.speed > TNT_JOLT) thing->fuse = std::min(thing->fuse, 0.05f);
            }
        for (auto& [e, steps] : game.newborn)
            if (--steps == 0 && world.valid(e) && world.all_of<projv::CollisionGroups>(e))
                world.patch<projv::CollisionGroups>(e, [](projv::CollisionGroups& g) { g.family = 0; });
        std::erase_if(game.newborn, [](const auto& n) { return n.second <= 0; });
    }

    // A break's pieces: crumbs walked through (the "small debris" group), and a strike's shards in one
    // family for their first quarter second, so the solver does not spend the break's first steps on
    // every shard against its neighbours.
    inline void onBroken(World& world, const projv::BodyBroken& e) {
        Game& game = world.ctx().get<Game>();
        game.breaks++;
        const auto& scene = world.ctx().get<projv::Scene>();
        const uint32_t family = e.shards > 1 ? game.nextFamily++ : 0;
        for (size_t i = 0; i < e.pieces.size(); i++) {
            const Entity piece = e.pieces[i];
            if (!world.valid(piece)) continue;
            const projv::VoxelComponent* link = world.try_get<projv::VoxelComponent>(piece);
            projv::CollisionGroups groups;
            if (link && projv::utils::getComponentVoxelCount(scene, link->handle) <= SMALL_DEBRIS_VOXELS) groups.groups.push_back("small debris");
            if (i < e.shards && family) {
                groups.family = family;
                game.newborn.push_back({piece, SHARD_FAMILY_STEPS});
            }
            world.emplace_or_replace<projv::CollisionGroups>(piece, std::move(groups));
        }
    }

    // ---- Installing it ------------------------------------------------------------------------------

    // Loads the scene, its physics and the rules into `app`, and serves it. False if it cannot listen.
    inline bool installServer(projv::Application& app, ServerOptions options) {
        World& world = app.world;
        projv::Scene& loaded = world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(options.folder));
        if (loaded.components.empty()) {
            projv::core::error("playground: no field in '{}' (run from the binary's folder, or --write-assets)", options.folder);
            return false;
        }
        runtime::installSceneBridge(app);
        runtime::PhysicsConfig physics;
        physics.maxPieces = 1500;
        physics.dustVoxels = 4;
        physics.fracture.minImpactBreak = 2.0f;
        physics.settings.worldMin = vec3(-400.0f, -60.0f, -400.0f);
        physics.settings.worldMax = vec3(400.0f, 400.0f, 400.0f);
        runtime::installPhysics(app, physics);
        runtime::installImpactFracture(app);
        runtime::loadPhysicsMaterials(world, (std::filesystem::path(options.folder) / MATERIALS).string());
        for (const char* group : {"small debris", "player"}) runtime::definePhysicsGroup(world, group);
        runtime::spawnEntities(world);

        Game& game = world.ctx().emplace<Game>();
        game.options = options;
        const auto& scene = world.ctx().get<projv::Scene>();
        for (auto [e, collider] : world.view<projv::StaticCollider>().each()) {
            if (!collider.splits) continue;
            const projv::ComponentHandle h = runtime::voxelComponentOf(world, e);
            if (h != projv::INVALID_COMPONENT_HANDLE) game.structures.push_back({e, projv::utils::componentVoxels(scene, h)});
        }

        app.events().on<projv::BodyBroken>([&world](const projv::BodyBroken& e) { onBroken(world, e); });
        // A piece broken down to nothing is gone, everywhere.
        app.events().on<projv::BodyEmptied>([&world](const projv::BodyEmptied& e) {
            if (world.valid(e.entity) && world.any_of<projv::Piece, Built>(e.entity)) world.destroy(e.entity);
        });
        app.events().on<runtime::ClientJoined>([&world](const runtime::ClientJoined& e) {
            if (!world.valid(e.player) || !world.all_of<Player>(e.player)) return;
            Player& p = world.get<Player>(e.player);
            p.name = e.name.empty() ? "PLAYER" : e.name.substr(0, 10);
            banner(world, p.name + " JOINS", colourOf(p.slot), 2.0f);
        });

        runtime::ServerConfig config;
        config.port = options.port;
        config.sceneFolder = options.folder;
        // Four at most: a fifth is refused a body, and so a place.
        config.spawnPlayer = [](World& w, runtime::ClientId id) { return makePlayer(w, w.ctx().get<Game>(), id); };
        // A person's walking. It carries the knocks their client was told of, so it may be faster
        // than a walk.
        config.applyInput = [](World& w, runtime::ClientId, Entity player, vec3 move, bool jump) {
            if (!w.valid(player) || !w.all_of<Player>(player)) return;
            move.y = 0.0f;
            const float most = WALK_SPEED + BLAST_KNOCK + PUSH_SPEED;
            if (glm::length(move) > most) move = glm::normalize(move) * most;
            runtime::setCharacterInput(w, player, move, jump);
        };
        config.onMessage = [](World& w, runtime::ClientId, Entity player, std::span<const uint8_t> message) {
            onMessage(w, w.ctx().get<Game>(), player, message);
        };
        if (!runtime::installServer(app, projv::net::makeEnetTransport(), config)) return false;
        app.addSystem(projv::Stage::Update, "playground: rules", rules);
        app.addSystem(projv::Stage::FixedUpdate, "playground: after step", afterStep, {.after = "physics"});
        app.addSystem(projv::Stage::Startup, "playground: props", [](projv::Application& a) { makeProps(a.world, a.world.ctx().get<Game>()); });
        return true;
    }
}
