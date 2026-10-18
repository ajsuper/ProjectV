// The siege's rules: what only the server runs. Rounds, health and death, the three items, the
// cannons, the flags, who wins, and the bots that fill each team. siege_server runs it alone; a client
// started without --host runs it beside itself in the same process, and others can join that.
//
// Everything a player does reaches here as a request (runtime::sendToServer, read in onMessage) or as
// walking input (applyInput); a bot's brain makes the same requests by calling the same functions.
// What clients should know goes back with runtime::sendToClient(s): each player's state several times
// a second, and shots, blasts, deaths and banners as they happen.
#pragma once

#include <deque>
#include <map>
#include <random>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "core/application.h"
#include "core/log.h"
#include "net/transport.h"
#include "net/wire.h"
#include "runtime/entities.h"
#include "runtime/net.h"
#include "runtime/physics.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"
#include "siege.h"
#include "utils/compose_io.h"
#include "utils/scene_query.h"

namespace siege {
    using projv::Entity;
    using projv::NullEntity;
    using projv::World;
    namespace runtime = projv::runtime;

    struct ServerOptions {
        uint16_t    port = PORT;
        int         teamSize = TEAM_SIZE;
        float       roundSeconds = ROUND_SECONDS;
        std::string folder;   // the scene folder: "siege" beside the binary
    };

    // A soldier: a player's or a bot's, on the entity its character is.
    struct Soldier {
        std::string name;
        Team        team = Blue;
        runtime::ClientId client = 0;   // 0: a bot
        int         slot = 0;           // where in the line it spawns
        float       health = MAX_HEALTH;
        bool        alive = true, out = false;
        float       respawnIn = 0.0f;
        float       yaw = 0.0f, pitch = 0.0f;
        uint8_t     item = Bullet;
        float       cooldown[3] = {0, 0, 0};
        int         cannon = -1;
        std::deque<Entity> blocks;
        uint16_t    kills = 0, deaths = 0;
        float       shownYaw = 1e9f;    // the facing last written to its Transform
    };

    // A bot's brain: a path to walk, someone to shoot, something to do at the end.
    struct Bot {
        enum Role : uint8_t { Gunner, Raider, Defender };
        Role   role = Raider;
        int    cannon = -1;              // a gunner's
        std::vector<vec3> path;
        size_t next = 0;
        float  think = 0.0f, trigger = 0.0f, moved = 0.0f, sidestep = 0.0f, wander = 0.0f;
        int    stuck = 0;
        vec3   lastPosition{0.0f}, side{0.0f};
        Entity enemy = NullEntity;
        bool   aimed = false;
        int    shots = 0;                // a gunner's, this life: its aim improves
    };

    struct Thrown { Team team; Entity by; float age = 0.0f; };   // on a cannonball
    struct PlacedBlock {};

    struct Game {
        ServerOptions options;
        float    timeLeft = ROUND_SECONDS;
        uint16_t round = 1;
        bool     over = false;
        float    overFor = 0.0f;
        struct Cannon {
            Entity carriage = NullEntity, barrel = NullEntity, user = NullEntity;
            float  yaw = 0.0f, pitch = 0.1f, reload = 0.0f;
            float  shownYaw = 1e9f, shownPitch = 1e9f;
            bool   wrecked = false;
        };
        Cannon cannons[CANNONS];
        struct Flag { Entity entity = NullEntity; bool down = false; };
        Flag   flags[2];
        struct Shot { vec3 position, velocity; Team team; Entity by; float age = 0.0f; };
        std::vector<Shot> shots;
        struct Kick { vec3 at; float radius, speed; int steps; };
        std::vector<Kick> kicks;
        std::vector<std::pair<Entity, int>> newborn;   // shards still in their break's family
        // The fortresses and the cover as the files have them: put back each round.
        struct Original { Entity entity; std::vector<projv::utils::ComponentVoxel> voxels; };
        std::vector<Original> structures;
        float stateClock = 0.0f, wreckClock = 0.0f, nudgeClock = 0.0f;
        int   bots = 0;
        uint64_t cannonShots = 0, blasts = 0, breaks = 0;   // for the server's report
        std::mt19937 random{2026};

        float uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(random); }
        float normal(float spread) { return std::normal_distribution<float>(0.0f, spread)(random); }
        std::string prefab(const char* name) const { return (std::filesystem::path(options.folder) / name).string(); }
    };

    constexpr uint32_t SMALL_DEBRIS_VOXELS = 3;
    constexpr int SHARD_FAMILY_STEPS = 15;

    inline uint32_t solidLayers() {
        return runtime::layerBit(runtime::PhysicsLayer::Static) | runtime::layerBit(runtime::PhysicsLayer::Moving);
    }
    inline uint32_t debris(const World& world) { return runtime::physicsGroup(world, "small debris"); }

    // ---- Telling clients ----------------------------------------------------------------------------

    inline void tellAll(World& world, const projv::net::BitWriter& w, bool reliable = true) {
        runtime::sendToClients(world, w.data(), reliable);
    }

    inline void banner(World& world, const std::string& text, vec3 colour, float seconds = 3.0f) {
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Banner));
        w.bytes(std::vector<uint8_t>(text.begin(), text.end()));
        writeVec(w, colour);
        w.f32(seconds);
        tellAll(world, w);
        projv::core::info("siege: {}", text);
    }

    inline vec3 teamColour(Team t) { return t == Blue ? vec3(0.35f, 0.55f, 1.0f) : vec3(1.0f, 0.3f, 0.25f); }

    // ---- Soldiers -----------------------------------------------------------------------------------

    inline vec3 feet(const World& world, Entity e) {
        if (world.all_of<projv::PhysicsCharacter>(e)) return runtime::characterState(world, e).position;
        const projv::Transform* t = world.try_get<projv::Transform>(e);
        return t ? t->position : vec3(0.0f);
    }

    inline vec3 spawnPoint(Team team, int slot) {
        return Frame::of(team).at(-2.0f + float(slot / 4) * 1.2f, 0.05f, (float(slot % 4) - 1.5f) * 1.3f);
    }

    // Moves a character at once, still: a respawn, or off to the pen. The client that walks it is put
    // right by its next snapshot (prediction, runtime/replication.h).
    inline void moveCharacter(World& world, Entity e, vec3 to) {
        runtime::setCharacterInput(world, e, vec3(0.0f));
        if (const projv::PhysicsCharacter* c = world.try_get<projv::PhysicsCharacter>(e)) {
            runtime::physicsWorld(world).setCharacterPosition(c->id, to);
            runtime::physicsWorld(world).setCharacterVelocity(c->id, vec3(0.0f));
        } else {
            world.patch<projv::Transform>(e, [&](projv::Transform& t) { t.position = to; });
        }
    }

    inline vec3 penPoint(Entity e) {
        const uint32_t n = static_cast<uint32_t>(e) % 49u;
        return vec3(float(n % 7) - 3.0f, PEN_FLOOR + 0.05f, float(n / 7) - 3.0f);
    }

    inline void dismount(Game& game, Soldier& s) {
        if (s.cannon >= 0) game.cannons[s.cannon].user = NullEntity;
        s.cannon = -1;
    }

    // Players standing in a team: alive, or dead and waiting to come back.
    inline int standing(World& world, Team team) {
        int n = 0;
        for (auto [e, s] : world.view<Soldier>().each()) n += s.team == team && (s.alive || !s.out);
        return n;
    }

    inline void kill(World& world, Game& game, Entity victim, Entity killer) {
        Soldier& s = world.get<Soldier>(victim);
        if (!s.alive) return;
        const vec3 at = feet(world, victim);
        s.alive = false;
        s.health = 0.0f;
        s.deaths++;
        dismount(game, s);
        s.out = game.flags[s.team].down;
        s.respawnIn = RESPAWN_SECONDS;
        if (killer != NullEntity && world.valid(killer) && killer != victim)
            if (Soldier* k = world.try_get<Soldier>(killer)) k->kills++;
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Death));
        writeVec(w, at + vec3(0.0f, 1.0f, 0.0f));
        w.varint(s.team);
        tellAll(world, w);
        const Soldier* k = killer != NullEntity && world.valid(killer) ? world.try_get<Soldier>(killer) : nullptr;
        projv::core::info("siege: {} ({}) was killed{}{}", s.name, teamName(s.team), k ? " by " : "", k ? k->name : "");
        moveCharacter(world, victim, penPoint(victim));
    }

    inline void hurt(World& world, Game& game, Entity victim, float amount, Entity by) {
        Soldier* s = world.try_get<Soldier>(victim);
        if (!s || !s->alive || amount <= 0.0f || game.over) return;
        s->health -= amount;
        if (s->client) {
            projv::net::BitWriter w;
            w.varint(uint8_t(ToClient::Hurt));
            w.f32(amount);
            runtime::sendToClient(world, s->client, w.data());
        }
        if (s->health <= 0.0f) kill(world, game, victim, by);
    }

    inline void respawn(World& world, Entity e) {
        Soldier& s = world.get<Soldier>(e);
        s.alive = true;
        s.out = false;
        s.health = MAX_HEALTH;
        s.cooldown[0] = s.cooldown[1] = s.cooldown[2] = 0.0f;
        s.yaw = Frame::of(s.team).yaw();
        s.pitch = 0.0f;
        moveCharacter(world, e, spawnPoint(s.team, s.slot));
        if (Bot* bot = world.try_get<Bot>(e)) {
            bot->path.clear();
            bot->stuck = 0;
            bot->shots = 0;
        }
    }

    // A free slot in a team's line, so two never spawn on each other.
    inline int freeSlot(World& world, Team team) {
        for (int slot = 0;; slot++) {
            bool taken = false;
            for (auto [e, s] : world.view<Soldier>().each()) taken = taken || (s.team == team && s.slot == slot);
            if (!taken) return slot;
        }
    }

    inline Entity makeSoldier(World& world, Game& game, Team team, runtime::ClientId client, const std::string& name) {
        Soldier s;
        s.team = team;
        s.client = client;
        s.name = name;
        s.slot = freeSlot(world, team);
        s.yaw = Frame::of(team).yaw();
        Entity e = runtime::instantiatePrefab(world, game.prefab(SOLDIER[team]), spawnPoint(team, s.slot), facing(s.yaw));
        if (e == NullEntity) return e;
        world.emplace<projv::CharacterController>(e);
        world.emplace<projv::CollisionGroups>(e, projv::CollisionGroups{{"player"}, {"small debris"}, 0});
        world.emplace<projv::Replicated>(e, projv::Replicated{SOLDIER[team], {"physics.character"}});
        runtime::assignNetId(world, e);
        // A soldier who joins a round whose flag has fallen waits for the next.
        if (game.flags[team].down) {
            s.alive = false;
            s.out = true;
            s.health = 0.0f;
        }
        world.emplace<Soldier>(e, std::move(s));
        if (!world.get<Soldier>(e).alive) moveCharacter(world, e, penPoint(e));
        return e;
    }

    // Each team made up to its size with bots, and a bot stood down for each player over it.
    inline void balanceBots(World& world, Game& game) {
        for (Team team : {Blue, Red}) {
            int count = 0;
            Entity spareBot = NullEntity;
            for (auto [e, s] : world.view<Soldier>().each()) {
                if (s.team != team) continue;
                count++;
                if (!s.client) spareBot = e;
            }
            if (count > game.options.teamSize && spareBot != NullEntity) {
                dismount(game, world.get<Soldier>(spareBot));
                for (Entity b : world.get<Soldier>(spareBot).blocks) if (world.valid(b)) world.destroy(b);
                world.destroy(spareBot);
            } else if (count < game.options.teamSize) {
                static const char* names[] = {"ANNA", "BRAM", "CORA", "DREW", "EMIL", "FAYE", "GUS", "HANA", "IVO", "JUNE"};
                Entity e = makeSoldier(world, game, team, 0, std::string("BOT ") + names[game.bots++ % 10]);
                if (e == NullEntity) continue;
                Bot bot;
                // Each team's first bot mans a cannon, the next guards the courtyard, the rest attack.
                int gunners = 0, defenders = 0;
                for (auto [other, b, s] : world.view<Bot, Soldier>().each())
                    if (other != e && s.team == team) {
                        gunners += b.role == Bot::Gunner;
                        defenders += b.role == Bot::Defender;
                    }
                if (gunners == 0) {
                    bot.role = Bot::Gunner;
                    bot.cannon = team == Blue ? 0 : 2;
                } else if (defenders == 0) {
                    bot.role = Bot::Defender;
                }
                world.emplace<Bot>(e, bot);
            }
        }
    }

    // ---- Items --------------------------------------------------------------------------------------

    // The closest the segments p0-p1 and q0-q1 come.
    inline float segmentDistance(vec3 p0, vec3 p1, vec3 q0, vec3 q1) {
        const vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
        const float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
        float s = 0.0f, t = 0.0f;
        if (a < 1e-8f && e < 1e-8f) return glm::length(r);
        if (a < 1e-8f) t = std::clamp(f / e, 0.0f, 1.0f);
        else {
            const float c = glm::dot(d1, r);
            if (e < 1e-8f) s = std::clamp(-c / a, 0.0f, 1.0f);
            else {
                const float b = glm::dot(d1, d2), denom = a * e - b * b;
                s = denom > 1e-8f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
                t = (b * s + f) / e;
                if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
                else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
            }
        }
        return glm::length((p0 + d1 * s) - (q0 + d2 * t));
    }

    // How close a segment comes to a soldier's body (a capsule from shin to head).
    inline float distanceToBody(const World& world, Entity e, vec3 a, vec3 b) {
        const vec3 f = feet(world, e);
        return segmentDistance(a, b, f + vec3(0.0f, 0.3f, 0.0f), f + vec3(0.0f, 1.55f, 0.0f));
    }

    inline void fireBullet(World& world, Game& game, Entity shooter, vec3 eye, vec3 target) {
        Soldier& s = world.get<Soldier>(shooter);
        vec3 aim = target - eye;
        if (glm::length(aim) < 0.1f) return;
        aim = glm::normalize(aim);
        const vec3 velocity = aim * BULLET_SPEED;
        const vec3 from = eye + aim * 0.45f;
        game.shots.push_back({from, velocity, s.team, shooter});
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Shot));
        writeVec(w, from);
        writeVec(w, velocity);
        w.varint(s.team);
        tellAll(world, w, false);
    }

    inline bool placeBlock(World& world, Game& game, Entity placer, vec3 eye, vec3 target) {
        Soldier& s = world.get<Soldier>(placer);
        vec3 aim = target - eye;
        if (glm::length(aim) < 0.1f) return false;
        aim = glm::normalize(aim);
        auto hit = runtime::castRay(world, eye, aim, BLOCK_REACH, solidLayers(), placer, debris(world));
        if (!hit) return false;
        const vec3 cell = glm::floor(hit->position + hit->normal * 0.5f);
        const vec3 centre = cell + vec3(0.5f);
        if (!runtime::overlapBox(world, centre, vec3(0.45f), quat(1, 0, 0, 0), solidLayers()).empty()) return false;
        for (auto [e, other] : world.view<Soldier>().each())
            if (other.alive && distanceToBody(world, e, centre - vec3(0, 0.5f, 0), centre + vec3(0, 0.5f, 0)) < 0.85f) return false;
        Entity block = runtime::instantiatePrefab(world, game.prefab(BLOCK), vec3(centre.x, cell.y, centre.z));
        if (block == NullEntity) return false;
        projv::StaticCollider collider;
        collider.splits = true;
        collider.anchorBase = true;
        collider.breaksOnImpact = true;
        world.emplace<projv::StaticCollider>(block, collider);
        world.emplace<projv::Replicated>(block, projv::Replicated{BLOCK, {}});
        world.emplace<PlacedBlock>(block);
        runtime::assignNetId(world, block);
        s.blocks.push_back(block);
        while (s.blocks.size() > MAX_BLOCKS) {
            if (world.valid(s.blocks.front())) world.destroy(s.blocks.front());
            s.blocks.pop_front();
        }
        return true;
    }

    // A blast at `at`: what a ray from it reaches within the radius is struck, with the energy falling
    // off with the square of the distance; two steps later everything near (the shards too) is thrown
    // outward; soldiers near are hurt -- enemies fully, the one who set it off a little.
    inline void explode(World& world, Game& game, vec3 at, Entity by) {
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
        game.kicks.push_back({at, BLAST_RADIUS * 1.8f, 18.0f, 2});
        game.blasts++;
        const Team team = by != NullEntity && world.valid(by) ? world.get<Soldier>(by).team : Blue;
        std::vector<std::pair<Entity, float>> hurtBy;
        for (auto [e, s] : world.view<Soldier>().each()) {
            if (!s.alive) continue;
            const float d = glm::distance(at, feet(world, e) + vec3(0.0f, 0.9f, 0.0f));
            if (d >= BLAST_RADIUS) continue;
            const float share = e == by ? 0.35f : (s.team == team ? 0.0f : 1.0f);
            hurtBy.push_back({e, BLAST_DAMAGE * (1.0f - d / BLAST_RADIUS) * share});
        }
        for (auto [e, amount] : hurtBy) hurt(world, game, e, amount, by);
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Blast));
        writeVec(w, at);
        tellAll(world, w);
    }

    inline bool blast(World& world, Game& game, Entity by, vec3 eye, vec3 target) {
        vec3 aim = target - eye;
        if (glm::length(aim) < 0.1f) return false;
        aim = glm::normalize(aim);
        auto hit = runtime::castRay(world, eye, aim, BLAST_RANGE, solidLayers(), by, debris(world));
        if (!hit) return false;
        explode(world, game, hit->position + hit->normal * 0.3f, by);
        return true;
    }

    inline void fireCannon(World& world, Game& game, Entity by, int index) {
        Game::Cannon& cannon = game.cannons[index];
        if (cannon.reload > 0.0f || cannon.wrecked) return;
        const vec3 aim = direction(cannon.yaw, cannon.pitch);
        const vec3 from = muzzle(index, cannon.yaw, cannon.pitch);
        Entity ball = runtime::instantiatePrefab(world, game.prefab(CANNONBALL), from);
        if (ball == NullEntity) return;
        projv::RigidBody body;
        body.shape = projv::RigidBody::Shape::Sphere;
        body.material = "iron";
        body.quality = projv::RigidBody::Quality::Continuous;
        world.emplace<projv::RigidBody>(ball, body);
        world.emplace<projv::Replicated>(ball, projv::Replicated{CANNONBALL, {}});
        world.emplace<Thrown>(ball, Thrown{cannonTeam(index), by});
        runtime::assignNetId(world, ball);
        runtime::setVelocity(world, ball, aim * CANNON_SPEED);
        cannon.reload = CANNON_RELOAD;
        game.cannonShots++;
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Muzzle));
        writeVec(w, from);
        writeVec(w, aim);
        tellAll(world, w);
    }

    // What a soldier's click does: the cannon it is on, or the item in its hand, aimed from its eye
    // toward `target`.
    inline void use(World& world, Game& game, Entity e, vec3 eye, vec3 target) {
        Soldier& s = world.get<Soldier>(e);
        if (!s.alive || game.over) return;
        if (s.cannon >= 0) return fireCannon(world, game, e, s.cannon);
        if (s.cooldown[s.item] > 0.0f) return;
        bool used = false;
        switch (s.item) {
            case Bullet: fireBullet(world, game, e, eye, target); used = true; break;
            case Blocks: used = placeBlock(world, game, e, eye, target); break;
            case Blast:  used = blast(world, game, e, eye, target); break;
        }
        if (used) s.cooldown[s.item] = COOLDOWN[s.item];
    }

    inline void toggleMount(World& world, Game& game, Entity e) {
        Soldier& s = world.get<Soldier>(e);
        if (s.cannon >= 0) return dismount(game, s);
        if (!s.alive) return;
        const vec3 at = feet(world, e);
        for (int i = 0; i < CANNONS; i++) {
            Game::Cannon& c = game.cannons[i];
            if (cannonTeam(i) != s.team || c.wrecked) continue;
            if (glm::distance(at, Frame::of(s.team).cannonSeat(cannonSide(i))) > CANNON_REACH) continue;
            if (c.user != NullEntity && world.valid(c.user)) {
                // A bot makes way for a player, and goes to fight instead.
                Bot* bot = world.try_get<Bot>(c.user);
                if (!s.client || !bot) continue;
                dismount(game, world.get<Soldier>(c.user));
                bot->role = Bot::Raider;
                bot->path.clear();
            }
            c.user = e;
            s.cannon = i;
            s.yaw = c.yaw;
            s.pitch = c.pitch;
            return;
        }
    }

    // ---- Requests from players ----------------------------------------------------------------------

    inline void onMessage(World& world, Game& game, Entity player, std::span<const uint8_t> message) {
        if (player == NullEntity || !world.valid(player) || !world.all_of<Soldier>(player)) return;
        Soldier& s = world.get<Soldier>(player);
        projv::net::BitReader r(message);
        const auto kind = ToServer(r.varint());
        if (kind == ToServer::Aim) {
            const float yaw = r.f32(), pitch = r.f32();
            if (r.failed || !std::isfinite(yaw) || !std::isfinite(pitch)) return;
            s.yaw = wrapAngle(yaw);
            s.pitch = std::clamp(pitch, -1.5f, 1.5f);
        } else if (kind == ToServer::Select) {
            const uint64_t item = r.varint();
            if (!r.failed && item <= Blast) s.item = uint8_t(item);
        } else if (kind == ToServer::Mount) {
            toggleMount(world, game, player);
        } else if (kind == ToServer::Use) {
            const vec3 eye = readVec(r), target = readVec(r);
            // From its own eyes: a client cannot fire from across the field.
            if (r.failed || !finite(eye) || !finite(target)) return;
            if (glm::distance(eye, feet(world, player) + vec3(0.0f, EYE_HEIGHT, 0.0f)) > 3.0f) return;
            use(world, game, player, eye, target);
        }
    }

    // ---- Bots ---------------------------------------------------------------------------------------

    // Where a cannonball fired from `from` at `velocity` first hits something.
    inline std::optional<vec3> landing(const World& world, vec3 from, vec3 velocity) {
        vec3 p = from;
        for (float t = 0.0f; t < 6.0f; t += 0.05f) {
            const vec3 next = p + velocity * 0.05f;
            velocity.y -= GRAVITY * 0.05f;
            const vec3 step = next - p;
            if (auto hit = runtime::castRay(world, p, glm::normalize(step), glm::length(step), solidLayers(), NullEntity, debris(world)))
                return hit->position;
            p = next;
            if (p.y < -5.0f) return p;
        }
        return std::nullopt;
    }

    inline void planPath(World& world, Game& game, Entity e, Bot& bot) {
        const Soldier& s = world.get<Soldier>(e);
        const Frame home = Frame::of(s.team), enemy = Frame::of(other(s.team));
        bot.path.clear();
        bot.next = 0;
        if (bot.role == Bot::Gunner && (bot.cannon < 0 || game.cannons[bot.cannon].wrecked)) bot.role = Bot::Raider;
        if (bot.role == Bot::Gunner) {
            const float side = cannonSide(bot.cannon) ? 1.0f : -1.0f;
            bot.path = {home.at(6.0f, 0, side * 2.3f), home.at(8.25f, 0, side * 2.3f), home.at(8.25f, WALL_HEIGHT, side * 8.1f),
                        home.at(9.6f, WALL_HEIGHT, side * 8.1f), home.cannonSeat(cannonSide(bot.cannon))};
        } else if (bot.role == Bot::Raider) {
            bot.path = {home.gateInside() + vec3(0, 0, game.uniform(-0.8f, 0.8f)), home.gateOutside(),
                        vec3(game.uniform(-6.0f, 6.0f), 0.0f, game.uniform(-14.0f, 14.0f)), enemy.gateOutside(),
                        enemy.gateInside() + vec3(0, 0, game.uniform(-0.8f, 0.8f)),
                        enemy.at(1.0f, 0.0f, game.uniform(-4.0f, 4.0f))};
        } else {
            bot.path = {home.at(game.uniform(0.0f, 5.0f), 0.0f, game.uniform(-5.0f, 5.0f))};
        }
    }

    inline bool canSee(const World& world, vec3 from, vec3 to) {
        const vec3 d = to - from;
        const float distance = glm::length(d);
        if (distance < 0.5f) return true;
        return !runtime::castRay(world, from, d / distance, distance - 0.6f, solidLayers(), NullEntity, debris(world));
    }

    // The nearest enemy standing within `range` that can be seen.
    inline Entity spot(World& world, Entity e, float range) {
        const Soldier& s = world.get<Soldier>(e);
        const vec3 eye = feet(world, e) + vec3(0, EYE_HEIGHT, 0);
        Entity best = NullEntity;
        float bestDistance = range;
        for (auto [other, o] : world.view<Soldier>().each()) {
            if (o.team == s.team || !o.alive) continue;
            const vec3 chest = feet(world, other) + vec3(0, 1.2f, 0);
            const float d = glm::distance(eye, chest);
            if (d < bestDistance && canSee(world, eye, chest)) {
                best = other;
                bestDistance = d;
            }
        }
        return best;
    }

    // Looks at `point` from `eye`.
    inline void lookAt(Soldier& s, vec3 eye, vec3 point) {
        const vec3 d = point - eye;
        s.yaw = std::atan2(d.z, d.x);
        s.pitch = std::atan2(d.y, std::max(glm::length(vec3(d.x, 0, d.z)), 0.01f));
    }

    // A gunner's shot: at the enemy flag while it stands, then at whoever is nearest, then the keep.
    inline void aimCannon(World& world, Game& game, Entity e, Bot& bot) {
        Soldier& s = world.get<Soldier>(e);
        const Team enemy = other(s.team);
        vec3 target = Frame::of(enemy).at(-6.5f, KEEP_HEIGHT - 3.0f, 0.0f);
        const Game::Flag& flag = game.flags[enemy];
        if (!flag.down && world.valid(flag.entity) && runtime::hasBody(world, flag.entity)) {
            target = runtime::bodyPose(world, flag.entity).position + vec3(0, 1.2f, 0);
        } else {
            float best = 110.0f;
            for (auto [other, o] : world.view<Soldier>().each())
                if (o.team == enemy && o.alive) {
                    const float d = glm::distance(feet(world, other), cannonPivot(s.cannon));
                    if (d < best) { best = d; target = feet(world, other) + vec3(0, 0.8f, 0); }
                }
        }
        const vec3 d = target - cannonPivot(s.cannon);
        float yaw = std::atan2(d.z, d.x);
        const float across = glm::length(vec3(d.x, 0, d.z));
        // The low arc if it gets there, else the high; if neither, the low anyway, and the wall in the
        // way goes.
        const float low = ballisticPitch(across, d.y, CANNON_SPEED, false).value_or(0.6f);
        float pitch = low;
        bool found = false;
        for (bool high : {false, true}) {
            auto p = ballisticPitch(across, d.y, CANNON_SPEED, high);
            if (!p) continue;
            float clampedYaw = yaw, clampedPitch = *p;
            clampCannon(s.cannon, clampedYaw, clampedPitch);
            auto land = landing(world, muzzle(s.cannon, clampedYaw, clampedPitch), direction(clampedYaw, clampedPitch) * CANNON_SPEED);
            if (land && glm::distance(*land, target) < 4.0f) {
                pitch = *p;
                found = true;
                break;
            }
        }
        if (std::getenv("SIEGE_BOTS")) {
            float y = yaw, p = pitch;
            clampCannon(s.cannon, y, p);
            auto land = landing(world, muzzle(s.cannon, y, p), direction(y, p) * CANNON_SPEED);
            projv::core::info("SIEGEAIM: {} target ({:.1f}, {:.1f}, {:.1f}) pitch {:.3f} {} lands ({:.1f}, {:.1f}, {:.1f})", s.name, target.x,
                              target.y, target.z, p, found ? "clear" : "blocked", land ? land->x : 0.0f, land ? land->y : 0.0f,
                              land ? land->z : 0.0f);
        }
        // Not a marksman: well off at first, closer with each shot, never dead on.
        const float spread = std::max(0.022f, 0.07f * std::pow(0.82f, float(bot.shots)));
        s.yaw = yaw + game.normal(spread);
        s.pitch = pitch + game.normal(spread * 0.8f);
        bot.aimed = true;
    }

    inline void botThink(World& world, Game& game, Entity e, Bot& bot, float dt) {
        Soldier& s = world.get<Soldier>(e);
        if (!s.alive || game.over) {
            runtime::setCharacterInput(world, e, vec3(0.0f));
            return;
        }
        if (bot.path.empty()) planPath(world, game, e, bot);
        const vec3 at = feet(world, e), eye = at + vec3(0, EYE_HEIGHT, 0);
        bot.think -= dt;
        bot.trigger -= dt;
        bot.sidestep -= dt;

        // On a cannon: aim, and fire when loaded.
        if (s.cannon >= 0) {
            if (game.cannons[s.cannon].wrecked) { dismount(game, s); return; }
            runtime::setCharacterInput(world, e, vec3(0.0f));
            if (bot.think <= 0.0f && game.cannons[s.cannon].reload < 0.4f) {
                bot.think = 0.5f;
                aimCannon(world, game, e, bot);
            }
            if (bot.aimed && game.cannons[s.cannon].reload <= 0.0f && bot.think < 0.3f) {
                // A moment after aiming: the barrel follows the aim in the rules below, not at once.
                fireCannon(world, game, e, s.cannon);
                bot.aimed = false;
                bot.shots++;
                bot.think = game.uniform(0.5f, 2.5f);   // and a breath before the next
            }
            return;
        }

        if (bot.think <= 0.0f) {
            bot.think = 0.25f;
            bot.enemy = spot(world, e, bot.role == Bot::Defender ? 35.0f : 45.0f);
        }
        const bool engaging = bot.enemy != NullEntity && world.valid(bot.enemy) && world.get<Soldier>(bot.enemy).alive;

        // Walking the path.
        vec3 walk(0.0f);
        const bool arrived = bot.next >= bot.path.size();
        if (!arrived) {
            vec3 to = bot.path[bot.next] - at;
            to.y = 0.0f;
            if (glm::length(to) < 0.7f) bot.next++;
            else walk = glm::normalize(to) * (bot.role == Bot::Defender ? WALK_SPEED : SPRINT_SPEED * 0.85f);
        } else if (bot.role == Bot::Gunner) {
            toggleMount(world, game, e);
            if (s.cannon < 0) bot.role = Bot::Raider, bot.path.clear();
        } else if (bot.role == Bot::Defender) {
            bot.wander -= dt;
            if (bot.wander <= 0.0f) {
                bot.wander = game.uniform(3.0f, 7.0f);
                bot.path = {Frame::of(s.team).at(game.uniform(-1.0f, 6.0f), 0.0f, game.uniform(-6.0f, 6.0f))};
                bot.next = 0;
            }
        }
        if (engaging && bot.role == Bot::Defender) walk *= 0.3f;
        if (bot.sidestep > 0.0f) walk = bot.side * WALK_SPEED;

        // Stuck: jump, then step aside, then blast through.
        bool jump = false;
        bot.moved += dt;
        if (bot.moved >= 1.0f) {
            const bool still = glm::length(vec3(at.x - bot.lastPosition.x, 0, at.z - bot.lastPosition.z)) < 0.35f;
            bot.stuck = still && glm::length(walk) > 0.1f ? bot.stuck + 1 : 0;
            bot.lastPosition = at;
            bot.moved = 0.0f;
            if (bot.stuck >= 1) jump = true;
            if (bot.stuck >= 3) {
                const vec3 ahead = glm::length(walk) > 0.1f ? glm::normalize(walk) : direction(s.yaw, 0.0f);
                auto wall = runtime::castRay(world, eye - vec3(0, 0.6f, 0), ahead, 5.0f, solidLayers(), e, debris(world));
                if (bot.role == Bot::Raider && wall && wall->distance > 2.5f && s.cooldown[Blast] <= 0.0f) {
                    s.item = Blast;
                    use(world, game, e, eye, wall->position);
                } else {
                    bot.side = vec3(-ahead.z, 0, ahead.x) * (game.uniform(0, 1) < 0.5f ? -1.0f : 1.0f);
                    bot.sidestep = 0.8f;
                }
            }
            if (bot.stuck >= 8) {
                bot.next = std::min(bot.next + 1, bot.path.size());
                bot.stuck = 0;
            }
        }
        runtime::setCharacterInput(world, e, walk, jump);

        // Fighting: bullets at whoever it sees, a blast now and then; at the end of a raid, blasts at
        // the keep's foot to bring the flag down.
        if (engaging) {
            const vec3 chest = feet(world, bot.enemy) + vec3(0, 1.2f, 0);
            const float d = glm::distance(eye, chest);
            lookAt(s, eye, chest);
            if (bot.trigger <= 0.0f) {
                bot.trigger = game.uniform(0.3f, 0.9f);
                const vec3 miss = vec3(game.normal(1.0f), game.normal(1.0f), game.normal(1.0f)) * (0.03f * d + 0.15f);
                if (bot.role == Bot::Raider && d > 7.0f && d < 22.0f && s.cooldown[Blast] <= 0.0f && game.uniform(0, 1) < 0.15f) {
                    s.item = Blast;
                    use(world, game, e, eye, feet(world, bot.enemy) + miss);
                } else {
                    s.item = Bullet;
                    use(world, game, e, eye, chest + miss);
                }
            }
        } else if (glm::length(walk) > 0.1f) {
            s.yaw = std::atan2(walk.z, walk.x);
            s.pitch = 0.0f;
        }
        if (!engaging && arrived && bot.role == Bot::Raider && !game.flags[other(s.team)].down && s.cooldown[Blast] <= 0.0f) {
            const vec3 keep = Frame::of(other(s.team)).at(-4.0f, 1.0f, game.uniform(-1.8f, 1.8f));
            if (glm::distance(eye, keep) < BLAST_RANGE - 2.0f) {
                lookAt(s, eye, keep);
                s.item = Blast;
                use(world, game, e, eye, keep);
            }
        }
    }

    // ---- The round ----------------------------------------------------------------------------------

    inline void makeCannonsAndFlags(World& world, Game& game) {
        for (int i = 0; i < CANNONS; i++) {
            Game::Cannon& c = game.cannons[i];
            for (Entity e : {c.carriage, c.barrel}) if (e != NullEntity && world.valid(e)) world.destroy(e);
            c = Game::Cannon{};
            c.yaw = Frame::of(cannonTeam(i)).yaw();
            const vec3 pivot = cannonPivot(i);
            c.carriage = runtime::instantiatePrefab(world, game.prefab(CARRIAGE), pivot - vec3(0, 0.25f, 0), barrelRotation(c.yaw, 0.0f));
            c.barrel = runtime::instantiatePrefab(world, game.prefab(BARREL), pivot, barrelRotation(c.yaw, c.pitch));
            for (auto [e, name] : {std::pair{c.carriage, CARRIAGE}, std::pair{c.barrel, BARREL}}) {
                if (e == NullEntity) continue;
                world.emplace<projv::Replicated>(e, projv::Replicated{name, {}});
                runtime::assignNetId(world, e);
            }
        }
        for (Team team : {Blue, Red}) {
            Game::Flag& f = game.flags[team];
            if (f.entity != NullEntity && world.valid(f.entity)) world.destroy(f.entity);
            f = Game::Flag{};
            f.entity = runtime::instantiatePrefab(world, game.prefab(FLAG[team]), Frame::of(team).flagHome());
            if (f.entity == NullEntity) continue;
            world.emplace<projv::RigidBody>(f.entity);
            world.emplace<projv::Replicated>(f.entity, projv::Replicated{FLAG[team], {}});
            runtime::assignNetId(world, f.entity);
        }
    }

    // A new round: the rubble cleared, every wall put back as the files have it, new cannons and flags,
    // everyone home.
    inline void startRound(World& world, Game& game, bool first) {
        std::vector<Entity> doomed;
        for (Entity e : world.view<projv::Piece>()) doomed.push_back(e);
        for (Entity e : world.view<PlacedBlock>()) doomed.push_back(e);
        for (Entity e : world.view<Thrown>()) doomed.push_back(e);
        for (Entity e : doomed) if (world.valid(e)) world.destroy(e);
        game.shots.clear();
        game.kicks.clear();
        game.newborn.clear();
        auto& scene = world.ctx().get<projv::Scene>();
        for (const Game::Original& o : game.structures) {
            if (!world.valid(o.entity)) continue;
            const projv::ComponentHandle h = runtime::voxelComponentOf(world, o.entity);
            if (h == projv::INVALID_COMPONENT_HANDLE) continue;
            std::set<std::tuple<int, int, int>> present;
            for (const auto& v : projv::utils::componentVoxels(scene, h)) present.insert({v.position.x, v.position.y, v.position.z});
            std::vector<projv::PendingVoxelOp> ops;
            const auto& palette = scene.components[h].materialPalette;
            for (const auto& v : o.voxels)
                if (!present.count({v.position.x, v.position.y, v.position.z})) {
                    projv::PendingVoxelOp op{true, v.position, v.slot < palette.size() ? palette[v.slot].packedColor : 0u};
                    op.slot = v.slot;
                    ops.push_back(op);
                }
            if (!ops.empty()) runtime::editVoxels(world, o.entity, ops);
        }
        makeCannonsAndFlags(world, game);
        // Not until the walls under the cannons are solid: bodies are made at the next step, and an
        // edit putting a wall back lands then too.
        game.wreckClock = 1.0f;
        for (auto [e, s] : world.view<Soldier>().each()) {
            s.kills = first ? 0 : s.kills;
            s.blocks.clear();
            s.cannon = -1;
        }
        std::vector<Entity> soldiers;
        for (Entity e : world.view<Soldier>()) soldiers.push_back(e);
        for (Entity e : soldiers) respawn(world, e);
        game.timeLeft = game.options.roundSeconds;
        game.over = false;
        if (!first) game.round++;
        banner(world, "ROUND " + std::to_string(game.round), vec3(1.0f, 0.95f, 0.8f), 2.5f);
    }

    inline void endRound(World& world, Game& game, const std::string& text, vec3 colour) {
        game.over = true;
        game.overFor = ROUND_OVER_SECONDS;
        for (auto [e, s] : world.view<Soldier>().each()) dismount(game, s);
        banner(world, text, colour, ROUND_OVER_SECONDS);
    }

    // ---- Each frame ---------------------------------------------------------------------------------

    inline void rules(projv::Application& app) {
        World& world = app.world;
        Game& game = world.ctx().get<Game>();
        const float dt = app.time().delta;
        if (dt <= 0.0f) return;

        if (game.over) {
            game.overFor -= dt;
            if (game.overFor <= 0.0f) startRound(world, game, false);
        }
        balanceBots(world, game);

        for (auto [e, bot] : world.view<Bot>().each()) botThink(world, game, e, bot, dt);

        // Soldiers: cooldowns, facing, the dead coming back, and anyone who fell off the world.
        std::vector<Entity> fell, back;
        for (auto [e, s] : world.view<Soldier>().each()) {
            for (float& c : s.cooldown) c = std::max(0.0f, c - dt);
            if (s.cannon >= 0 && (game.cannons[s.cannon].user != e || game.cannons[s.cannon].wrecked)) s.cannon = -1;
            if (s.alive) {
                if (std::abs(s.yaw - s.shownYaw) > 0.01f) {
                    s.shownYaw = s.yaw;
                    world.patch<projv::Transform>(e, [&](projv::Transform& t) { t.rotation = facing(s.yaw); });
                }
                if (feet(world, e).y < -4.0f) fell.push_back(e);
            } else if (!s.out) {
                if (game.flags[s.team].down) s.out = true;
                else if ((s.respawnIn -= dt) <= 0.0f && !game.over) back.push_back(e);
            }
        }
        for (Entity e : fell) kill(world, game, e, NullEntity);
        for (Entity e : back) respawn(world, e);

        // Cannons: pointing where their gunner looks, reloading, and falling when the wall under them
        // goes.
        game.wreckClock -= dt;
        const bool checkWrecks = game.wreckClock <= 0.0f;
        if (checkWrecks) game.wreckClock = 0.5f;
        for (int i = 0; i < CANNONS; i++) {
            Game::Cannon& c = game.cannons[i];
            c.reload = std::max(0.0f, c.reload - dt);
            if (c.wrecked || !world.valid(c.barrel) || !world.valid(c.carriage)) continue;
            if (c.user != NullEntity && world.valid(c.user)) {
                const Soldier& s = world.get<Soldier>(c.user);
                c.yaw = s.yaw;
                c.pitch = s.pitch;
                clampCannon(i, c.yaw, c.pitch);
            } else {
                c.user = NullEntity;
            }
            if (std::abs(c.yaw - c.shownYaw) > 1e-3f || std::abs(c.pitch - c.shownPitch) > 1e-3f) {
                c.shownYaw = c.yaw;
                c.shownPitch = c.pitch;
                world.patch<projv::Transform>(c.barrel, [&](projv::Transform& t) { t.rotation = barrelRotation(c.yaw, c.pitch); });
                world.patch<projv::Transform>(c.carriage, [&](projv::Transform& t) { t.rotation = barrelRotation(c.yaw, 0.0f); });
            }
            if (checkWrecks && !runtime::castRay(world, cannonPivot(i), vec3(0, -1, 0), 1.5f,
                                                 runtime::layerBit(runtime::PhysicsLayer::Static))) {
                c.wrecked = true;
                if (c.user != NullEntity && world.valid(c.user)) dismount(game, world.get<Soldier>(c.user));
                for (auto [e, material] : {std::pair{c.carriage, "planks"}, std::pair{c.barrel, "iron"}}) {
                    projv::RigidBody body;
                    body.material = material;
                    world.emplace<projv::RigidBody>(e, body);
                }
            }
        }

        // Bullets: flown here, a ray each frame; what they hit first is hurt or chipped.
        std::vector<std::pair<Entity, Entity>> shot;   // victim, shooter
        for (Game::Shot& b : game.shots) {
            b.age += dt;
            const vec3 next = b.position + b.velocity * dt;
            b.velocity.y -= BULLET_DROP * dt;
            const vec3 step = next - b.position;
            const float length = glm::length(step);
            auto hit = runtime::castRay(world, b.position, step / length, length, solidLayers(), NullEntity, debris(world));
            const float reach = hit ? hit->distance : length;
            Entity victim = NullEntity;
            float nearest = 1e9f;
            for (auto [e, s] : world.view<Soldier>().each()) {
                if (!s.alive || s.team == b.team || e == b.by) continue;
                const vec3 end = b.position + step / length * reach;
                if (distanceToBody(world, e, b.position, end) < 0.4f) {
                    const float d = glm::distance(b.position, feet(world, e));
                    if (d < nearest) { nearest = d; victim = e; }
                }
            }
            if (victim != NullEntity) {
                shot.push_back({victim, b.by});
                b.age = 1e9f;
            } else if (hit) {
                if (runtime::canBreak(world, hit->entity)) {
                    runtime::Strike strike;
                    strike.at = hit->position;
                    strike.energy = BULLET_ENERGY;
                    runtime::fracture(world, hit->entity, strike);
                }
                b.age = 1e9f;
            }
            b.position = next;
        }
        std::erase_if(game.shots, [](const Game::Shot& b) { return b.age > 2.0f; });
        for (auto [victim, by] : shot) hurt(world, game, victim, BULLET_DAMAGE, by);

        // Cannonballs: deadly while they fly fast, gone after a while.
        std::vector<Entity> old;
        std::vector<std::pair<Entity, Entity>> struck;
        for (auto [ball, thrown] : world.view<Thrown>().each()) {
            thrown.age += dt;
            if (thrown.age > CANNONBALL_SECONDS) { old.push_back(ball); continue; }
            if (!runtime::hasBody(world, ball)) continue;
            const runtime::BodyState pose = runtime::bodyPose(world, ball);
            if (glm::length(pose.linearVelocity) < 8.0f) continue;
            for (auto [e, s] : world.view<Soldier>().each())
                if (s.alive && s.team != thrown.team && distanceToBody(world, e, pose.position, pose.position) < 0.9f)
                    struck.push_back({e, thrown.by});
        }
        for (Entity e : old) world.destroy(e);
        for (auto [victim, by] : struck) hurt(world, game, victim, CANNONBALL_DAMAGE, by);

        // The flags: down once fallen from their keep. One at rest is woken now and then, so it notices
        // the roof going from under it.
        game.nudgeClock -= dt;
        for (Team team : {Blue, Red}) {
            Game::Flag& f = game.flags[team];
            if (f.down || game.over) continue;
            const bool gone = !world.valid(f.entity);
            if (gone || (runtime::hasBody(world, f.entity) && runtime::bodyPose(world, f.entity).position.y < FLAG_DOWN_BELOW)) {
                f.down = true;
                banner(world, std::string(teamName(team)) + " FLAG IS DOWN!", teamColour(other(team)), 3.5f);
                for (auto [e, s] : world.view<Soldier>().each())
                    if (s.team == team && !s.alive) s.out = true;
            } else if (game.nudgeClock <= 0.0f && runtime::hasBody(world, f.entity) && !runtime::bodyPose(world, f.entity).awake) {
                runtime::addVelocity(world, f.entity, vec3(0.0f, -0.05f, 0.0f));
            }
        }
        if (game.nudgeClock <= 0.0f) game.nudgeClock = 0.5f;

        // Who has won: a team whose flag is down and whose last soldier has fallen has lost.
        if (!game.over) {
            game.timeLeft -= dt;
            const bool blueGone = game.flags[Blue].down && standing(world, Blue) == 0;
            const bool redGone = game.flags[Red].down && standing(world, Red) == 0;
            if (blueGone && redGone) endRound(world, game, "DRAW!", vec3(1.0f, 0.9f, 0.6f));
            else if (redGone) endRound(world, game, "BLUE WINS!", teamColour(Blue));
            else if (blueGone) endRound(world, game, "RED WINS!", teamColour(Red));
            else if (game.timeLeft <= 0.0f) endRound(world, game, "TIME UP - DRAW", vec3(1.0f, 0.9f, 0.6f));
        }

        // Each player told how it and the round stand, fifteen times a second.
        game.stateClock -= dt;
        if (game.stateClock <= 0.0f) {
            game.stateClock = 1.0f / 15.0f;
            State state;
            state.timeLeft = std::max(0.0f, game.timeLeft);
            state.round = game.round;
            state.over = game.over;
            for (Team team : {Blue, Red}) {
                state.flagUp[team] = !game.flags[team].down;
                state.standing[team] = uint8_t(standing(world, team));
            }
            for (auto [e, s] : world.view<Soldier>().each()) {
                if (!s.client) continue;
                state.team = s.team;
                state.health = std::max(0.0f, s.health);
                state.alive = s.alive;
                state.out = s.out;
                state.respawnIn = s.respawnIn;
                state.item = s.item;
                for (int i = 0; i < 3; i++) state.cooldown[i] = s.cooldown[i] / COOLDOWN[i];
                state.cannon = int8_t(s.cannon);
                state.reload = s.cannon >= 0 ? game.cannons[s.cannon].reload / CANNON_RELOAD : 0.0f;
                state.kills = s.kills;
                state.deaths = s.deaths;
                projv::net::BitWriter w;
                state.write(w);
                runtime::sendToClient(world, s.client, w.data(), false);
            }
        }
    }

    // FixedUpdate, after the physics: a blast's push, two steps on (so its shards exist), and a break's
    // shards let collide with each other again.
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
        for (auto& [e, steps] : game.newborn)
            if (--steps == 0 && world.valid(e) && world.all_of<projv::CollisionGroups>(e))
                world.patch<projv::CollisionGroups>(e, [](projv::CollisionGroups& g) { g.family = 0; });
        std::erase_if(game.newborn, [](const auto& n) { return n.second <= 0; });
    }

    // A break's pieces: crumbs walked and shot through (the "small debris" group), and a strike's
    // shards in one family for their first quarter second, so the solver does not spend the break's
    // first steps on every shard against its neighbours.
    inline void onBroken(World& world, const projv::BodyBroken& e) {
        Game& game = world.ctx().get<Game>();
        game.breaks++;
        const auto& scene = world.ctx().get<projv::Scene>();
        static uint32_t nextFamily = 1000;
        const uint32_t family = e.shards > 1 ? nextFamily++ : 0;
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
        projv::Scene& loaded = app.world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(options.folder));
        if (loaded.components.empty()) {
            projv::core::error("siege: no field in '{}' (run from the binary's folder, or --write-assets)", options.folder);
            return false;
        }
        runtime::installSceneBridge(app);
        runtime::PhysicsConfig physics;
        physics.maxPieces = 1500;
        physics.dustVoxels = 4;
        physics.fracture.minImpactBreak = 2.0f;
        runtime::installPhysics(app, physics);
        runtime::installImpactFracture(app);
        runtime::loadPhysicsMaterials(world, (std::filesystem::path(options.folder) / MATERIALS).string());
        for (const char* group : {"small debris", "player"}) runtime::definePhysicsGroup(world, group);
        runtime::spawnEntities(world);

        Game& game = world.ctx().emplace<Game>();
        game.options = options;
        game.timeLeft = options.roundSeconds;
        // What is breakable, as the files have it: what each round puts back.
        const auto& scene = world.ctx().get<projv::Scene>();
        for (auto [e, collider] : world.view<projv::StaticCollider>().each()) {
            if (!collider.splits) continue;
            const projv::ComponentHandle h = runtime::voxelComponentOf(world, e);
            if (h != projv::INVALID_COMPONENT_HANDLE) game.structures.push_back({e, projv::utils::componentVoxels(scene, h)});
        }

        app.events().on<projv::BodyBroken>([&world](const projv::BodyBroken& e) { onBroken(world, e); });
        // A piece broken down to nothing is gone, everywhere.
        app.events().on<projv::BodyEmptied>([&world](const projv::BodyEmptied& e) {
            if (world.valid(e.entity) && world.all_of<projv::Piece>(e.entity)) world.destroy(e.entity);
        });
        app.events().on<runtime::ClientJoined>([&world](const runtime::ClientJoined& e) {
            if (!world.valid(e.player) || !world.all_of<Soldier>(e.player)) return;
            Soldier& s = world.get<Soldier>(e.player);
            s.name = e.name.empty() ? "PLAYER" : e.name;
            banner(world, s.name + " JOINS " + teamName(s.team), teamColour(s.team), 2.0f);
        });

        runtime::ServerConfig config;
        config.port = options.port;
        config.sceneFolder = options.folder;
        // A player: on the team with fewer players, a bot making way for them.
        config.spawnPlayer = [](World& w, runtime::ClientId id) {
            Game& g = w.ctx().get<Game>();
            int humans[2] = {0, 0};
            for (auto [e, s] : w.view<Soldier>().each()) humans[s.team] += s.client != 0;
            const Team team = humans[Red] < humans[Blue] ? Red : Blue;
            return makeSoldier(w, g, team, id, "PLAYER");
        };
        config.applyInput = [](World& w, runtime::ClientId, Entity player, vec3 move, bool jump) {
            const Soldier* s = w.valid(player) ? w.try_get<Soldier>(player) : nullptr;
            if (!s || !s->alive || s->cannon >= 0 || w.ctx().get<Game>().over) {
                runtime::setCharacterInput(w, player, vec3(0.0f));
                return;
            }
            move.y = 0.0f;
            if (glm::length(move) > SPRINT_SPEED * 1.05f) move = glm::normalize(move) * SPRINT_SPEED;
            runtime::setCharacterInput(w, player, move, jump);
        };
        config.onMessage = [](World& w, runtime::ClientId, Entity player, std::span<const uint8_t> message) {
            onMessage(w, w.ctx().get<Game>(), player, message);
        };
        if (!runtime::installServer(app, projv::net::makeEnetTransport(), config)) return false;
        app.addSystem(projv::Stage::Update, "siege: rules", rules);
        app.addSystem(projv::Stage::FixedUpdate, "siege: after step", afterStep, {.after = "physics"});
        app.addSystem(projv::Stage::Startup, "siege: first round", [](projv::Application& a) {
            startRound(a.world, a.world.ctx().get<Game>(), true);
        });
        return true;
    }
}
