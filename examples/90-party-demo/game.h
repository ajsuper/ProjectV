// The party's rules: what only the server runs. The match and its rounds, the five games, the shove,
// the blocks and the bombs, the tiles that drop, and the bots that fill the seats nobody has.
// party_demo_server runs it alone; a client started without --host runs it beside itself in the same
// process, and others can join that.
//
// Everything a player does reaches here as a request (runtime::sendToServer, read in onMessage) or as
// walking input (applyInput); a bot's brain makes the same requests by calling the same functions.
//
// A shove's push is carried by the shoved player's own walking: the server tells that player's
// client (ToClient::Knock), which adds the push to the input it predicts with and sends -- so its
// prediction and the server agree, and nothing has to be corrected. A bot's push is added here.
#pragma once

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
#include "party.h"
#include "runtime/entities.h"
#include "runtime/net.h"
#include "runtime/physics.h"
#include "runtime/replication.h"
#include "runtime/scene_bridge.h"
#include "utils/compose_io.h"
#include "utils/scene_query.h"

namespace party {
    using projv::Entity;
    using projv::NullEntity;
    using projv::World;
    namespace runtime = projv::runtime;

    struct ServerOptions {
        uint16_t    port = PORT;
        int         players = MAX_PLAYERS;   // bots fill the seats up to this
        int         rounds = ROUNDS;
        int         only = -1;               // a Mode: play just that game (for trying one out)
        std::string folder;                  // the scene folder: "party" beside the binary
    };

    // A player: a person's or a bot's, on the entity its character is.
    struct Player {
        std::string name;
        runtime::ClientId client = 0;   // 0: a bot
        int     slot = 0;               // its colour, where it starts, its pad
        bool    alive = false;          // on the floor, in this round
        bool    out = true;             // out of this round, waiting for the next
        float   yaw = 0.0f, shownYaw = 1e9f;
        float   shove = 0.0f, toss = 0.0f, block = 0.0f;   // cooldowns
        int     blocks = 0;             // left to lay
        float   regrow = 0.0f;          // till another block
        float   respawnIn = 0.0f;       // back after a fall (Sky Dash, Sky Climb, Blueprint)
        float   hazardWait = 0.0f;      // Sky Dash: not thrown again by a sweeper or pusher till this is 0
        int     checkpoint = 0;         // Sky Dash: the last passed
        float   furthest = DASH_CHECKPOINTS[0];   // Sky Dash: z reached
        int     place = 0;              // Sky Dash: where it finished; Sky Climb, Blueprint: 1 if it won; 0 not yet
        float   best = 0.0f;            // Sky Climb: the highest it has stood; Blueprint: how much of the shape it has
        uint8_t points = 0;
        vec3    knock{0.0f};            // a bot's: the shove it is still carried by
        bool    pop = false;            // a bot's: hop with it
    };

    // A bot's brain: somewhere to go, and when to think again.
    struct Bot {
        vec3  goal{0.0f};
        vec3  way{0.0f};                // Sky Dash, Sky Climb, Blueprint: the way it decided to walk
        float wait = 0.0f;              // Sky Climb, Blueprint: till its next jump or cube
        float think = 0.0f, moved = 0.0f;
        vec3  last{0.0f};
        int   stuck = 0;
        bool  jump = false;
    };

    struct Thrown { Entity by; float fuse = THROW_FUSE, age = 0.0f; vec3 velocity{0.0f}; };   // on a lobbed bomb

    struct Game {
        ServerOptions options;
        Mode     mode = Crumble;
        Phase    phase = Intro;
        int      round = 1;
        float    clock = INTRO_SECONDS;
        // Everything that breaks, as the files have it: put back each round.
        struct Structure { Entity entity = NullEntity; std::vector<projv::utils::ComponentVoxel> original; };
        std::vector<Structure> structures;
        // A floor, and the palette slot of its anchor voxels: a tile laid names it outright, as a colour
        // matched to a slot by its colour alone would not carry the anchor's material.
        struct Floor { Entity entity = NullEntity; FloorSpec spec; uint8_t anchorSlot = projv::INVALID_MATERIAL; };
        std::vector<Floor> floors;
        struct Tile {
            size_t   floor = 0;
            TileSpec spec;
            float    crack = -1.0f;      // seconds since it was first stood on; < 0: not cracking
            float    regrow = -1.0f;     // Sky Dash's crumbling strip: seconds till it is back
            int      stage = -1;         // which crack colour its top is
            bool     dropped = false, fixed = false, crumbles = false;
            // Its voxels as the files have them, by palette slot: what it grows back as. (Laid by
            // colour, a voxel would take a new palette slot each time -- and a palette holds 255.)
            std::vector<projv::PendingVoxelOp> original;
        };
        std::vector<Tile> tiles;
        std::map<std::tuple<size_t, int, int>, size_t> tileAt;   // floor, i, j -> tile
        // Where cubes are built: Sky Climb's ground, Blueprint's pads and display.
        struct Plot {
            Entity   entity = NullEntity;
            PlotSpec spec;
            std::set<std::tuple<int, int, int>> cubes;   // cells with a cube
        };
        std::vector<Plot> plots;                          // in plotSpecs' order
        std::set<std::tuple<int, int, int>> shape;        // Blueprint: the cells to copy
        struct Bomb {
            Entity marker = NullEntity, carrier = NullEntity, from = NullEntity;   // from: who passed it last
            float  fuse = 0.0f, fuseMax = 1.0f, passWait = 0.0f, nextIn = 0.0f, fromWait = 0.0f;
        };
        Bomb   bomb;
        Entity sweepers[2] = {NullEntity, NullEntity}, pushers[3] = {NullEntity, NullEntity, NullEntity};
        float  hazardClock = 0.0f;
        std::vector<Entity> fallen;      // this round, in the order they went out
        int    finished = 0;             // Sky Dash: home so far
        int    bots = 0;
        float  stateClock = 0.0f, sweepClock = 0.0f, surveyClock = 0.0f;
        std::mt19937 random{2026};

        float uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(random); }
        std::string prefab(const std::string& name) const { return (std::filesystem::path(options.folder) / name).string(); }
    };

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
        projv::core::info("party: {}", text);
    }

    // Something to draw at `at`: a blast `radius` across, or (0) a shove's puff, or (-1) the bomb
    // changing hands, or (-2) a block laid.
    inline void boom(World& world, vec3 at, float radius) {
        projv::net::BitWriter w;
        w.varint(uint8_t(ToClient::Boom));
        writeVec(w, at);
        w.f32(radius);
        tellAll(world, w);
    }

    inline vec3 colourOf(int slot) { return PLAYER_COLOURS[std::clamp(slot, 0, MAX_PLAYERS - 1)]; }

    // ---- Players ------------------------------------------------------------------------------------

    inline vec3 feet(const World& world, Entity e) {
        if (world.all_of<projv::PhysicsCharacter>(e)) return runtime::characterState(world, e).position;
        const projv::Transform* t = world.try_get<projv::Transform>(e);
        return t ? t->position : vec3(0.0f);
    }

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

    inline vec3 penPoint(int slot) { return penCentre() + vec3(float(slot) * 1.5f - 2.25f, 0.05f, 0.0f); }

    inline int playersIn(World& world) { return int(world.view<Player>().size()); }
    inline int aliveCount(World& world) {
        int n = 0;
        for (auto [e, p] : world.view<Player>().each()) n += p.alive;
        return n;
    }
    // Still in the round: up, or down for a moment and coming back.
    inline int standingCount(World& world) {
        int n = 0;
        for (auto [e, p] : world.view<Player>().each()) n += !p.out;
        return n;
    }

    // A free colour, so no two players share one.
    inline int freeSlot(World& world) {
        for (int slot = 0; slot < MAX_PLAYERS; slot++) {
            bool taken = false;
            for (auto [e, p] : world.view<Player>().each()) taken = taken || p.slot == slot;
            if (!taken) return slot;
        }
        return -1;
    }

    // Ready for a round: on its start, with its blocks and nothing cooling down.
    inline void ready(World& world, Game& game, Entity e, Player& p) {
        p.alive = true;
        p.out = false;
        p.shove = p.toss = p.block = p.respawnIn = 0.0f;
        p.blocks = BLOCKS[game.mode];
        p.regrow = BLOCK_REGROW[game.mode];
        p.checkpoint = 0;
        p.furthest = DASH_CHECKPOINTS[0];
        p.place = 0;
        p.best = 0.0f;
        p.knock = vec3(0.0f);
        const vec3 start = startPoint(game.mode, p.slot);
        const vec3 ahead = game.mode == Dash ? vec3(0, 0, 1) : arenaCentre(game.mode) - start;
        p.yaw = std::atan2(ahead.z, ahead.x);
        moveCharacter(world, e, start);
    }

    inline Entity makePlayer(World& world, Game& game, runtime::ClientId client, const std::string& name) {
        const int slot = freeSlot(world);
        if (slot < 0) return NullEntity;
        Entity e = runtime::instantiatePrefab(world, game.prefab(playerPrefab(slot)), penPoint(slot), facing(0.0f));
        if (e == NullEntity) return e;
        world.emplace<projv::CharacterController>(e);
        world.emplace<projv::CollisionGroups>(e, projv::CollisionGroups{{"player"}, {}, 0});
        world.emplace<projv::Replicated>(e, projv::Replicated{playerPrefab(slot), {"physics.character"}});
        runtime::assignNetId(world, e);
        Player p;
        p.name = name;
        p.client = client;
        p.slot = slot;
        world.emplace<Player>(e, std::move(p));
        // In at once while the round is still being introduced; otherwise out until the next.
        if (game.phase == Intro) ready(world, game, e, world.get<Player>(e));
        return e;
    }

    // The seats made up to the number asked with bots, and a bot stood down for each person over it.
    inline void balanceBots(World& world, Game& game) {
        int count = 0;
        Entity spare = NullEntity;
        for (auto [e, p] : world.view<Player>().each()) {
            count++;
            if (!p.client) spare = e;
        }
        if (count > game.options.players && spare != NullEntity) {
            if (game.bomb.carrier == spare) game.bomb.carrier = NullEntity;
            world.destroy(spare);
        } else if (count < game.options.players) {
            static const char* names[] = {"BOT ANNA", "BOT BRAM", "BOT CORA", "BOT DREW", "BOT EMIL", "BOT FAYE"};
            Entity e = makePlayer(world, game, 0, names[game.bots++ % 6]);
            if (e != NullEntity) world.emplace<Bot>(e);
        }
    }

    // Out of the round: off to the pen to watch.
    inline void eliminate(World& world, Game& game, Entity e, const char* how) {
        Player& p = world.get<Player>(e);
        if (p.out) return;
        p.alive = false;
        p.out = true;
        game.fallen.push_back(e);
        moveCharacter(world, e, penPoint(p.slot));
        if (standingCount(world) >= 1) banner(world, p.name + " " + how, colourOf(p.slot), 1.8f);
    }

    // A push: a bot's carried here, a person's told to its client.
    inline void knock(World& world, Entity e, vec3 velocity) {
        Player& p = world.get<Player>(e);
        velocity.y = 0.0f;
        if (p.client) {
            projv::net::BitWriter w;
            w.varint(uint8_t(ToClient::Knock));
            writeVec(w, velocity);
            runtime::sendToClient(world, p.client, w.data());
        } else {
            p.knock = velocity;
            p.pop = true;
        }
    }

    // ---- Tiles --------------------------------------------------------------------------------------

    // The tile whose cell a point on the current game's floors is over, if any (gaps included).
    inline Game::Tile* tileUnder(Game& game, vec3 at) {
        for (size_t f = 0; f < game.floors.size(); f++) {
            const FloorSpec& spec = game.floors[f].spec;
            if (spec.mode != game.mode || std::abs(at.y - spec.top) > 0.4f) continue;
            const vec3 local = at - floorCorner(spec);
            const int i = int(std::floor(local.x / PITCH)), j = int(std::floor(local.z / PITCH));
            auto it = game.tileAt.find({f, i, j});
            if (it != game.tileAt.end()) return &game.tiles[it->second];
        }
        return nullptr;
    }

    struct Edits {
        std::map<Entity, std::vector<projv::PendingVoxelOp>> ops;
        std::vector<projv::PendingVoxelOp>& of(Entity e) { return ops[e]; }
        void flush(World& world) {
            for (auto& [e, list] : ops)
                if (!list.empty()) runtime::editVoxels(world, e, list);
            ops.clear();
        }
    };

    inline Entity floorOf(Game& game, const Game::Tile& t) { return game.floors[t.floor].entity; }

    inline void recolour(Game& game, Game::Tile& t, int stage, Edits& edits) {
        if (t.stage == stage) return;
        t.stage = stage;
        auto& list = edits.of(floorOf(game, t));
        for (ivec3 v : topVoxels(game.floors[t.floor].spec, t.spec)) {
            if (v == anchorVoxel(t.spec)) continue;
            list.push_back({true, v, crackColour(stage)});
        }
    }

    // Away: its anchor taken out, and it falls.
    inline void drop(Game& game, Game::Tile& t, Edits& edits) {
        if (t.dropped || t.fixed) return;
        t.dropped = true;
        // Sky Dash's crumbling strip comes back, for whoever is behind.
        if (game.mode == Dash && t.crumbles) t.regrow = TILE_REGROW;
        edits.of(floorOf(game, t)).push_back({false, anchorVoxel(t.spec), 0});
    }

    // Laid: a whole tile where there was none, in its layer's colour -- or, `colour` null, as the files
    // have it.
    inline void lay(Game& game, Game::Tile& t, const vec3* colour, Edits& edits) {
        t.dropped = false;
        t.crack = -1.0f;
        t.regrow = -1.0f;
        t.stage = -1;
        auto& list = edits.of(floorOf(game, t));
        if (!colour && !t.original.empty()) {
            list.insert(list.end(), t.original.begin(), t.original.end());
            return;
        }
        for (projv::PendingVoxelOp op : tileVoxels(game.floors[t.floor].spec, t.spec, colour)) {
            if (op.position == anchorVoxel(t.spec)) op.slot = game.floors[t.floor].anchorSlot;
            list.push_back(op);
        }
    }

    // Every tile of the current game within `radius` of `at`.
    inline void dropAround(Game& game, vec3 at, float radius, Edits& edits) {
        for (Game::Tile& t : game.tiles) {
            if (game.floors[t.floor].spec.mode != game.mode) continue;
            const vec3 d = t.spec.centre - at;
            if (glm::length(vec3(d.x, d.y * 2.0f, d.z)) < radius) drop(game, t, edits);
        }
    }

    // ---- Plots --------------------------------------------------------------------------------------

    // The plot of the current game a point is over, and the cell of it: -1 if none.
    // (Game::plots is in plotSpecs' order.)
    inline int plotAt(const Game& game, vec3 at, ivec3& cell) { return plotCellAt(game.mode, at, cell); }

    inline bool inPlot(const PlotSpec& spec, ivec3 cell) {
        return cell.x >= 0 && cell.z >= 0 && cell.x < spec.cells && cell.z < spec.cells && cell.y >= 0 && cell.y < spec.layers;
    }

    // Whether a cube may go there: on the plot, empty, and against something -- the platform, or a cube
    // below (`stacked`) or beside.
    inline bool cubeFits(const Game& game, int plot, ivec3 cell, bool stacked) {
        const Game::Plot& p = game.plots[size_t(plot)];
        if (p.spec.locked || !inPlot(p.spec, cell)) return false;
        const auto has = [&](ivec3 c) { return p.cubes.count({c.x, c.y, c.z}) > 0; };
        if (has(cell)) return false;
        if (cell.y == 0 || has(cell - ivec3(0, 1, 0))) return true;
        if (stacked) return false;
        for (ivec3 d : {ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1)})
            if (has(cell + d)) return true;
        return false;
    }

    // What the crosshair is on, `REACH` from `eye` along `dir`, in game `mode`: the plot and the cell a
    // cube would go in (`place`) or the cell of the cube it is on (not `place`). -1 if nothing. The
    // client asks it too, to mark where a cube would go.
    inline int aimedCell(const World& world, Mode mode, Entity ignore, vec3 eye, vec3 dir, bool place, ivec3& cell) {
        auto hit = runtime::castRay(world, eye, dir, REACH, runtime::layerBit(runtime::PhysicsLayer::Static), ignore);
        if (!hit) return -1;
        // Half a cube off the face hit: outside it to place, just inside it to break.
        const vec3 point = hit->position + hit->normal * (place ? 0.5f : -0.1f);
        return plotCellAt(mode, point, cell);
    }

    // Reads back which cells of a plot still hold most of a cube.
    inline void surveyPlot(World& world, Game& game, size_t index) {
        Game::Plot& plot = game.plots[index];
        const projv::ComponentHandle h = world.valid(plot.entity) ? runtime::voxelComponentOf(world, plot.entity) : projv::INVALID_COMPONENT_HANDLE;
        if (h == projv::INVALID_COMPONENT_HANDLE) return;
        std::map<std::tuple<int, int, int>, int> count;
        for (const auto& v : projv::utils::componentVoxels(world.ctx().get<projv::Scene>(), h)) {
            if (v.position.y < PLATFORM) continue;
            count[{v.position.x / CUBE, (v.position.y - PLATFORM) / CUBE, v.position.z / CUBE}]++;
        }
        plot.cubes.clear();
        for (auto [cell, n] : count)
            if (n >= CUBE * CUBE * CUBE / 2) plot.cubes.insert(cell);
    }

    // A cube into a plot's cell, in a player's colour.
    inline void addCube(World& world, Game& game, int plot, ivec3 cell, int slot, Edits& edits) {
        Game::Plot& p = game.plots[size_t(plot)];
        auto& list = edits.of(p.entity);
        for (const projv::PendingVoxelOp& op : cubeVoxels(cell, colourOf(slot))) list.push_back(op);
        p.cubes.insert({cell.x, cell.y, cell.z});
        boom(world, plotCorner(p.spec) + vec3(float(cell.x), float(PLATFORM) * VOXEL + float(cell.y), float(cell.z)) + vec3(0.5f), -2.0f);
    }

    // A cube taken out of a plot's cell; what it held up falls.
    inline bool removeCube(World& world, Game& game, int plot, ivec3 cell, Edits& edits) {
        Game::Plot& p = game.plots[size_t(plot)];
        if (p.spec.locked || !p.cubes.count({cell.x, cell.y, cell.z})) return false;
        auto& list = edits.of(p.entity);
        const ivec3 o = cubeOrigin(cell);
        for (int y = 0; y < CUBE; y++)
            for (int z = 0; z < CUBE; z++)
                for (int x = 0; x < CUBE; x++) list.push_back({false, o + ivec3(x, y, z), 0});
        p.cubes.erase({cell.x, cell.y, cell.z});
        boom(world, plotCorner(p.spec) + vec3(float(cell.x), float(PLATFORM) * VOXEL + float(cell.y), float(cell.z)) + vec3(0.5f), 0.0f);
        return true;
    }

    // ---- Things players do --------------------------------------------------------------------------

    inline void shove(World& world, Game& game, Entity e) {
        Player& p = world.get<Player>(e);
        if (!p.alive || p.shove > 0.0f || game.phase != Playing) return;
        p.shove = SHOVE_COOLDOWN;
        const vec3 at = feet(world, e), forward = forwardOf(p.yaw);
        std::vector<std::pair<Entity, vec3>> pushed;
        for (auto [other, o] : world.view<Player>().each()) {
            if (other == e || !o.alive) continue;
            vec3 d = feet(world, other) - at;
            if (std::abs(d.y) > 1.6f) continue;
            d.y = 0.0f;
            const float distance = glm::length(d);
            if (distance > SHOVE_REACH) continue;
            const vec3 dir = distance > 0.05f ? d / distance : forward;
            if (glm::dot(dir, forward) < SHOVE_CONE && distance > 0.7f) continue;
            pushed.push_back({other, glm::normalize(dir + forward * 0.5f) * SHOVE_SPEED});
        }
        for (auto [other, velocity] : pushed) knock(world, other, velocity);
        boom(world, at + forward * 0.9f + vec3(0.0f, 1.0f, 0.0f), 0.0f);
    }

    // A bomb lobbed ahead, further the longer it was held; it bursts where it lands.
    inline void throwBomb(World& world, Game& game, Entity e, float charge) {
        Player& p = world.get<Player>(e);
        if (!p.alive || p.toss > 0.0f || game.phase != Playing || THROW_COOLDOWN[game.mode] <= 0.0f) return;
        p.toss = THROW_COOLDOWN[game.mode];
        Entity bomb = runtime::instantiatePrefab(world, game.prefab(BOMB), throwOrigin(feet(world, e), p.yaw));
        if (bomb == NullEntity) return;
        projv::RigidBody body;
        body.shape = projv::RigidBody::Shape::Sphere;
        body.material = "iron";
        body.quality = projv::RigidBody::Quality::Continuous;
        world.emplace<projv::RigidBody>(bomb, body);
        world.emplace<projv::CollisionGroups>(bomb, projv::CollisionGroups{{"falling"}, {"player", "falling"}, 0});
        world.emplace<projv::Replicated>(bomb, projv::Replicated{BOMB, {}});
        const vec3 velocity = throwVelocity(p.yaw, charge);
        world.emplace<Thrown>(bomb, Thrown{e, THROW_FUSE, 0.0f, velocity});
        runtime::assignNetId(world, bomb);
        runtime::setVelocity(world, bomb, velocity);
    }

    // A bomb bursting: tiles near it gone, a bite out of any tower, everyone near thrown.
    inline void burst(World& world, Game& game, vec3 at, Edits& edits) {
        dropAround(game, at, BOMB_RADIUS, edits);
        for (const Game::Plot& plot : game.plots) {
            if (plot.spec.mode != game.mode || plot.spec.locked || !world.valid(plot.entity)) continue;
            if (glm::distance(vec3(at.x, 0, at.z), vec3(plot.spec.centre.x, 0, plot.spec.centre.z)) > float(plot.spec.cells) * 0.75f + BOMB_BITE) continue;
            auto& list = edits.of(plot.entity);
            // Not the platform: what stands on it.
            for (const projv::PendingVoxelOp& op : runtime::voxelsInSphere(world, plot.entity, at, BOMB_BITE))
                if (op.position.y >= PLATFORM) list.push_back(op);
        }
        std::vector<std::pair<Entity, vec3>> pushed;
        for (auto [e, p] : world.view<Player>().each()) {
            if (!p.alive) continue;
            vec3 d = feet(world, e) - at;
            d.y = 0.0f;
            const float distance = glm::length(d);
            if (distance > BOMB_KNOCK_RADIUS) continue;
            const vec3 dir = distance > 0.05f ? d / distance : vec3(1, 0, 0);
            pushed.push_back({e, dir * BOMB_KNOCK * (1.0f - 0.5f * distance / BOMB_KNOCK_RADIUS)});
        }
        for (auto [e, v] : pushed) knock(world, e, v);
        boom(world, at, BOMB_RADIUS);
    }

    // Whether a player stands where a block would go.
    inline bool someoneIn(World& world, vec3 lo, vec3 hi) {
        for (auto [e, p] : world.view<Player>().each()) {
            if (!p.alive) continue;
            const vec3 f = feet(world, e);
            if (f.x + 0.3f > lo.x && f.x - 0.3f < hi.x && f.z + 0.3f > lo.z && f.z - 0.3f < hi.z && f.y + 1.8f > lo.y && f.y < hi.y) return true;
        }
        return false;
    }

    // Where a player's first-person eye is (just in front of its face, as its client has it), and
    // where it looks (yaw, pitch).
    inline vec3 eyeOf(World& world, Entity e) {
        return feet(world, e) + vec3(0.0f, EYE_HEIGHT, 0.0f) + forwardOf(world.get<Player>(e).yaw) * EYE_AHEAD;
    }
    inline vec3 lookDir(float yaw, float pitch) { return vec3(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)); }

    // Whether a game builds in cubes on plots (rather than tiles into gaps).
    inline bool cubes(Mode mode) { return mode == Climb || mode == Blueprint; }

    // A block: in a game of plots, a cube where the crosshair points (`aimed`, along yaw and pitch from
    // the eye), or in front of the player on top of whatever is there; elsewhere a tile into the gap in
    // front.
    inline bool placeBlock(World& world, Game& game, Entity e, Edits& edits, bool aimed = false, float pitch = 0.0f) {
        Player& p = world.get<Player>(e);
        if (!p.alive || p.blocks <= 0 || p.block > 0.0f || game.phase != Playing) return false;
        const vec3 at = feet(world, e), forward = forwardOf(p.yaw);
        if (cubes(game.mode)) {
            ivec3 cell;
            int plot = -1;
            if (aimed) {
                plot = aimedCell(world, game.mode, e, eyeOf(world, e), lookDir(p.yaw, pitch), true, cell);
                if (plot < 0 || !cubeFits(game, plot, cell, false)) return false;
            } else {
                plot = plotAt(game, at + forward * 1.2f + vec3(0.0f, 0.2f, 0.0f), cell);
                if (plot < 0) return false;
                // Stack on what is there.
                cell.y = 0;
                while (cell.y < game.plots[size_t(plot)].spec.layers && !cubeFits(game, plot, cell, true)) cell.y++;
                if (!cubeFits(game, plot, cell, true)) return false;
            }
            const vec3 lo = plotCorner(game.plots[size_t(plot)].spec) + vec3(float(cell.x), float(PLATFORM) * VOXEL + float(cell.y), float(cell.z));
            if (someoneIn(world, lo, lo + vec3(1.0f))) return false;
            addCube(world, game, plot, cell, p.slot, edits);
        } else {
            // The first gap ahead, a tile or two out.
            Game::Tile* t = nullptr;
            for (float ahead : {PITCH, PITCH * 1.6f}) {
                Game::Tile* there = tileUnder(game, vec3(at.x, at.y, at.z) + forward * ahead);
                if (there && there->dropped) { t = there; break; }
            }
            if (!t) return false;
            const vec3 colour = colourOf(p.slot);
            lay(game, *t, &colour, edits);
            boom(world, t->spec.centre, -2.0f);
        }
        p.blocks--;
        p.block = BLOCK_COOLDOWN;
        return true;
    }

    // X: the cube the crosshair is on (`aimed`), or the one in front of the player at chest height.
    inline bool breakBlock(World& world, Game& game, Entity e, Edits& edits, bool aimed = false, float pitch = 0.0f) {
        Player& p = world.get<Player>(e);
        if (!p.alive || p.block > 0.0f || game.phase != Playing || !BREAKS[game.mode]) return false;
        ivec3 cell;
        const int plot = aimedCell(world, game.mode, e, eyeOf(world, e), lookDir(p.yaw, aimed ? pitch : -0.35f), false, cell);
        if (plot < 0 || !removeCube(world, game, plot, cell, edits)) return false;
        p.block = BLOCK_COOLDOWN;
        // A cube taken back is a cube to lay again.
        p.blocks = std::min(p.blocks + 1, BLOCKS[game.mode]);
        return true;
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
        } else if (kind == ToServer::Shove) {
            shove(world, game, player);
        } else if (kind == ToServer::Throw) {
            const float charge = r.f32();
            if (!r.failed && std::isfinite(charge)) throwBomb(world, game, player, std::clamp(charge, 0.0f, 1.0f));
        } else if (kind == ToServer::Place || kind == ToServer::Break) {
            const bool aimed = r.boolean();
            const float yaw = r.f32(), pitch = r.f32();
            if (r.failed || !std::isfinite(yaw) || !std::isfinite(pitch)) return;
            if (aimed) p.yaw = wrapAngle(yaw);
            Edits edits;
            if (kind == ToServer::Place) placeBlock(world, game, player, edits, aimed, std::clamp(pitch, -1.55f, 1.55f));
            else breakBlock(world, game, player, edits, aimed, std::clamp(pitch, -1.55f, 1.55f));
            edits.flush(world);
        }
    }

    // ---- Bots ---------------------------------------------------------------------------------------

    // What is underfoot `ahead` metres from `at` along `dir`: 0 sound floor (or the tile it stands on
    // already), 1 a tile cracking -- passable, quickly -- 2 nothing. Read from the tiles, not a ray: a
    // ray down a gap between two tiles would find nothing.
    inline int footing(Game& game, vec3 at, vec3 dir, float ahead) {
        const vec3 probe = at + dir * ahead;
        const Game::Tile* t = tileUnder(game, vec3(probe.x, at.y, probe.z));
        if (!t || t->dropped) return 2;
        if (t == tileUnder(game, at)) return 0;
        return t->crack >= 0.0f ? 1 : 0;
    }

    // The way nearest `want` that keeps floor underfoot.
    inline vec3 safeWay(Game& game, vec3 at, vec3 want) {
        if (glm::length(want) < 1e-3f) return vec3(0.0f);
        want = glm::normalize(want);
        static const float PENALTY[3] = {0.0f, 1.2f, 4.0f};
        vec3 best(0.0f);
        float bestScore = -1e9f;
        for (int k = 0; k < 16; k++) {
            const float angle = float(k) * 0.3926991f;
            const vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
            const float score = glm::dot(dir, want) - PENALTY[footing(game, at, dir, 1.2f)] - 0.5f * PENALTY[footing(game, at, dir, 2.4f)];
            if (score > bestScore) { bestScore = score; best = dir; }
        }
        return best;
    }

    inline Entity nearestOther(World& world, Entity e, float range, Entity except = NullEntity) {
        const vec3 at = feet(world, e);
        Entity best = NullEntity;
        float bestDistance = range;
        for (auto [other, o] : world.view<Player>().each()) {
            if (other == e || other == except || !o.alive) continue;
            const float d = glm::distance(feet(world, other), at);
            if (d < bestDistance) { bestDistance = d; best = other; }
        }
        return best;
    }

    // A shove at whoever is close in front, now and then.
    inline void maybeShove(World& world, Game& game, Entity e, Entity target, float chance) {
        Player& p = world.get<Player>(e);
        if (target == NullEntity || p.shove > 0.0f) return;
        const vec3 d = feet(world, target) - feet(world, e);
        if (glm::length(vec3(d.x, 0, d.z)) > SHOVE_REACH - 0.4f || game.uniform(0, 1) > chance) return;
        p.yaw = std::atan2(d.z, d.x);
        shove(world, game, e);
    }

    // Where a bomb thrown from `from` at `velocity` first lands.
    inline vec3 landing(const World& world, vec3 from, vec3 velocity) {
        vec3 p = from;
        for (float t = 0.0f; t < 4.0f; t += 0.04f) {
            const vec3 next = p + velocity * 0.04f;
            velocity.y -= GRAVITY * 0.04f;
            const vec3 step = next - p;
            if (auto hit = runtime::castRay(world, p, glm::normalize(step), glm::length(step), runtime::layerBit(runtime::PhysicsLayer::Static)))
                return hit->position;
            p = next;
            if (p.y < FALLEN_BELOW) break;
        }
        return p;
    }

    // A throw at `target`: faced, and the charge that lands nearest it.
    inline void throwAt(World& world, Game& game, Entity e, vec3 target, float error) {
        Player& p = world.get<Player>(e);
        const vec3 at = feet(world, e), d = target - at;
        p.yaw = std::atan2(d.z, d.x) + game.uniform(-error, error) * 0.1f;
        float best = 0.5f, bestMiss = 1e9f;
        for (float c = 0.0f; c <= 1.0f; c += 0.05f) {
            const float miss = glm::distance(landing(world, throwOrigin(at, p.yaw), throwVelocity(p.yaw, c)), target);
            if (miss < bestMiss) { bestMiss = miss; best = c; }
        }
        throwBomb(world, game, e, std::clamp(best + game.uniform(-error, error) * 0.08f, 0.0f, 1.0f));
    }

    // A racer: up the course, laying tiles over the gaps, jumping the sweepers, bombing whoever is just
    // ahead now and then.
    inline vec3 dashBot(World& world, Game& game, Entity e, Bot& bot, Edits& edits) {
        Player& p = world.get<Player>(e);
        const vec3 at = feet(world, e);
        const float lane = DASH_X + (float(p.slot) - 1.5f) * 1.6f;
        // Toward the finish, keeping to its lane; in the narrow way, the middle.
        const bool narrow = at.z > DASH_CHECKPOINTS[2] + 1.5f && at.z < DASH_FINISH;
        bot.goal = vec3(narrow ? DASH_X : lane, 0.0f, std::min(at.z + 6.0f, DASH_FINISH + 3.0f));
        // A gap ahead, from firm ground: lay a tile into it and wait for it. (On the crumbling strip,
        // never wait: run round what has gone.)
        const vec3 ahead = vec3(0.0f, 0.0f, 1.0f);
        const Game::Tile* under = tileUnder(game, at);
        const Game::Tile* next = tileUnder(game, at + ahead * PITCH);
        if (under && !under->crumbles && next && next->dropped && !next->crumbles && at.z < DASH_FINISH) {
            p.yaw = 1.5707963f;
            placeBlock(world, game, e, edits);
            return vec3(0.0f);
        }
        // A sweeper coming round: hop it.
        for (Entity s : game.sweepers) {
            if (!world.valid(s)) continue;
            const projv::Transform& t = world.get<projv::Transform>(s);
            const vec3 along = t.rotation * vec3(1, 0, 0), d = at - t.position;
            const float off = std::abs(glm::dot(d, vec3(-along.z, 0, along.x)));
            if (off < 1.4f && std::abs(glm::dot(d, along)) < SWEEPER_LENGTH * 0.5f && at.y < 0.5f) bot.jump = true;
        }
        // Whoever is a little ahead, in range: a bomb at their feet.
        if (p.toss <= 0.0f && game.uniform(0, 1) < 0.08f)
            for (auto [other, o] : world.view<Player>().each()) {
                if (other == e || !o.alive) continue;
                const vec3 there = feet(world, other);
                if (there.z > at.z + 4.0f && there.z < at.z + 13.0f) {
                    throwAt(world, game, e, there + vec3(0, 0, 1.0f), 2.0f);
                    break;
                }
            }
        maybeShove(world, game, e, nearestOther(world, e, 2.0f), 0.25f);
        vec3 want = bot.goal - at;
        want.y = 0.0f;
        if (glm::length(want) < 0.3f) return vec3(0.0f);
        const vec3 way = safeWay(game, at, want);
        // On firm ground with only holes ahead: wait for the strip to grow back.
        if (under && !under->crumbles && footing(game, at, way, 1.2f) == 2) return vec3(0.0f);
        return way;
    }

    // Sky Climb: pillar up where it stands -- jump, and at the top of the jump lay a cube under its
    // feet -- with a breath between; bomb the foot of whoever is higher.
    inline vec3 climbBot(World& world, Game& game, Entity e, Bot& bot, Edits& edits, float dt) {
        Player& p = world.get<Player>(e);
        const vec3 at = feet(world, e);
        const runtime::CharacterState state = runtime::characterState(world, e);
        bot.wait -= dt;
        if (state.onGround && bot.wait <= 0.0f && p.blocks > 0) {
            bot.jump = true;
            bot.wait = game.uniform(0.5f, 1.4f);
        }
        if (!state.onGround && state.velocity.y < 1.2f && p.block <= 0.0f && p.blocks > 0) {
            ivec3 cell;
            const int plot = plotAt(game, at - vec3(0.0f, 0.9f, 0.0f), cell);
            if (plot >= 0 && cubeFits(game, plot, cell, true)) {
                const vec3 lo = plotCorner(game.plots[size_t(plot)].spec) + vec3(float(cell.x), float(PLATFORM) * VOXEL + float(cell.y), float(cell.z));
                if (!someoneIn(world, lo, lo + vec3(1.0f))) {
                    addCube(world, game, plot, cell, p.slot, edits);
                    p.blocks--;
                    p.block = BLOCK_COOLDOWN;
                }
            }
        }
        if (p.toss <= 0.0f && game.uniform(0, 1) < 0.05f)
            for (auto [other, o] : world.view<Player>().each()) {
                if (other == e || !o.alive) continue;
                const vec3 there = feet(world, other);
                if (there.y > at.y + 1.0f && glm::length(vec3(there.x - at.x, 0, there.z - at.z)) > 3.0f) {
                    throwAt(world, game, e, vec3(there.x, 0.6f, there.z), 2.0f);
                    break;
                }
            }
        // Back toward where it started, when thrown off its tower.
        const vec3 spot = startPoint(Climb, p.slot) - at;
        return at.y < 0.6f && glm::length(vec3(spot.x, 0, spot.z)) > 1.5f ? glm::normalize(vec3(spot.x, 0, spot.z)) : vec3(0.0f);
    }

    // Blueprint: a cube every couple of seconds -- a wrong one taken away first, else the lowest
    // missing one -- and now and then a mistake.
    inline vec3 blueprintBot(World& world, Game& game, Entity e, Bot& bot, Edits& edits, float dt) {
        Player& p = world.get<Player>(e);
        const int own = ownPlot(Blueprint, p.slot);
        bot.wait -= dt;
        if (bot.wait <= 0.0f && own >= 0) {
            bot.wait = game.uniform(2.4f, 3.8f);
            const int off = (PAD_CELLS - SHAPE_CELLS) / 2;
            const auto& built = game.plots[size_t(own)].cubes;
            bool done = false;
            for (auto [x, y, z] : built) {
                if (game.shape.count({x - off, y, z - off})) continue;
                removeCube(world, game, own, ivec3(x, y, z), edits);
                p.blocks = std::min(p.blocks + 1, BLOCKS[Blueprint]);
                done = true;
                break;
            }
            if (!done && p.blocks > 0) {
                ivec3 next(-1);
                if (game.uniform(0, 1) < 0.15f) next = ivec3(int(game.uniform(0.0f, 4.99f)), 0, int(game.uniform(0.0f, 4.99f)));
                else
                    for (int y = 0; y < SHAPE_LAYERS && next.x < 0; y++)
                        for (auto [sx, sy, sz] : game.shape)
                            if (sy == y && !built.count({sx + off, sy, sz + off}) && cubeFits(game, own, ivec3(sx + off, sy, sz + off), true)) {
                                next = ivec3(sx + off, sy, sz + off);
                                break;
                            }
                if (next.x >= 0 && cubeFits(game, own, next, true)) {
                    addCube(world, game, own, next, p.slot, edits);
                    p.blocks--;
                }
            }
        }
        const vec3 spot = startPoint(Blueprint, p.slot) - feet(world, e);
        return glm::length(vec3(spot.x, 0, spot.z)) > 0.8f ? glm::normalize(vec3(spot.x, 0, spot.z)) : vec3(0.0f);
    }

    inline void botThink(World& world, Game& game, Entity e, Bot& bot, float dt, Edits& edits) {
        Player& p = world.get<Player>(e);
        if (!p.alive || game.phase != Playing) {
            runtime::setCharacterInput(world, e, vec3(0.0f));
            return;
        }
        const vec3 at = feet(world, e);
        const vec3 centre = arenaCentre(game.mode);
        bot.think -= dt;
        const bool carrier = game.bomb.carrier == e;
        vec3 way(0.0f);
        bool decided = false;
        if (game.mode == Climb || game.mode == Blueprint) {
            // Every frame: a climber's cube goes in at the top of its jump.
            bot.way = game.mode == Climb ? climbBot(world, game, e, bot, edits, dt) : blueprintBot(world, game, e, bot, edits, dt);
            way = bot.way;
            decided = true;
        } else if (game.mode == Dash) {
            if (bot.think <= 0.0f) {
                // A racer thinks often: at a run it is at a gap's edge in a moment.
                bot.think = game.uniform(0.05f, 0.1f);
                bot.way = dashBot(world, game, e, bot, edits);
            }
            way = bot.way;
            decided = true;
        } else if (bot.think <= 0.0f) {
            bot.think = game.uniform(0.25f, 0.5f);
            bot.jump = false;
            if (game.mode == Crumble) {
                // Keep moving, toward floor nobody has walked yet; shove whoever comes close; lay a
                // block when the floor runs out.
                Game::Tile* under = tileUnder(game, at);
                if (glm::distance(vec3(bot.goal.x, at.y, bot.goal.z), at) < 1.0f || !under || game.uniform(0, 1) < 0.15f ||
                    footing(game, bot.goal, vec3(0.0f), 0.0f) != 0) {
                    vec3 best = at;
                    float bestScore = -1e9f;
                    for (const Game::Tile& t : game.tiles) {
                        if (t.dropped || t.crack >= 0.0f || game.floors[t.floor].spec.mode != Crumble) continue;
                        if (std::abs(t.spec.centre.y - at.y) > 1.0f) continue;
                        const float d = glm::distance(t.spec.centre, at);
                        const float score = -std::abs(d - 4.0f) + game.uniform(0.0f, 2.5f);
                        if (score > bestScore) { bestScore = score; best = t.spec.centre; }
                    }
                    if (bestScore < -6.0f && p.blocks > 0) placeBlock(world, game, e, edits);
                    bot.goal = best;
                }
                maybeShove(world, game, e, nearestOther(world, e, 2.2f), 0.5f);
            } else {
                if (carrier) {
                    // The nearest it may pass to.
                    const Entity target = nearestOther(world, e, 100.0f, game.bomb.fromWait > 0.0f ? game.bomb.from : NullEntity);
                    bot.goal = target != NullEntity ? feet(world, target) : centre;
                } else {
                    // Away from the bomb, and toward the middle rather than the edge.
                    const Entity holder = game.bomb.carrier;
                    vec3 away = centre - at;
                    if (holder != NullEntity && world.valid(holder)) {
                        const vec3 d = at - feet(world, holder);
                        const float distance = glm::length(vec3(d.x, 0, d.z));
                        away = glm::normalize(vec3(d.x, 0, d.z) + vec3(1e-3f)) * (12.0f / std::max(distance, 1.0f)) +
                               (centre - at) * 0.08f;
                        if (distance < 2.0f) maybeShove(world, game, e, holder, 0.3f);
                    }
                    bot.goal = at + away * 4.0f;
                }
            }
        }
        if (!decided) {
            vec3 want = bot.goal - at;
            want.y = 0.0f;
            way = glm::length(want) > 0.3f ? safeWay(game, at, want) : vec3(0.0f);
        }
        // Running from the bomb, a bot is a step slower than a person: catchable.
        const float pace = carrier ? CARRIER_SPEED : game.mode == Tag ? 0.92f : 1.0f;
        const vec3 walk = way * WALK_SPEED * pace;
        // Stuck a while: a hop.
        bot.moved += dt;
        if (bot.moved > 0.8f) {
            const bool still = glm::length(vec3(at.x - bot.last.x, 0, at.z - bot.last.z)) < 0.3f && glm::length(walk) > 0.1f;
            bot.stuck = still ? bot.stuck + 1 : 0;
            bot.last = at;
            bot.moved = 0.0f;
            if (bot.stuck >= 1) bot.jump = true;
        }
        if (glm::length(walk) > 0.1f) p.yaw = std::atan2(walk.z, walk.x);
        p.knock *= std::max(0.0f, 1.0f - KNOCK_DECAY * dt / std::max(glm::length(p.knock), 1e-3f));
        runtime::setCharacterInput(world, e, walk + p.knock, bot.jump || p.pop);
        p.pop = false;
        bot.jump = false;
    }

    // ---- The match ----------------------------------------------------------------------------------

    // The order the games come in: bomb tag first, then a run, then a build, by turns.
    inline Mode modeOf(const Game& game, int round) {
        static const Mode order[] = {Tag, Dash, Climb, Crumble, Blueprint};
        return game.options.only >= 0 ? Mode(game.options.only) : order[(round - 1) % MODES];
    }

    // Everything put back as the files have it: what was laid taken away, what went put back.
    inline void restoreStructures(World& world, Game& game) {
        auto& scene = world.ctx().get<projv::Scene>();
        for (const Game::Structure& s : game.structures) {
            const projv::ComponentHandle h = runtime::voxelComponentOf(world, s.entity);
            if (h == projv::INVALID_COMPONENT_HANDLE) continue;
            std::map<std::tuple<int, int, int>, uint8_t> present, original;
            for (const auto& v : projv::utils::componentVoxels(scene, h)) present[{v.position.x, v.position.y, v.position.z}] = v.slot;
            std::vector<projv::PendingVoxelOp> ops;
            const auto& palette = scene.components[h].materialPalette;
            for (const auto& v : s.original) {
                original[{v.position.x, v.position.y, v.position.z}] = v.slot;
                auto it = present.find({v.position.x, v.position.y, v.position.z});
                if (it != present.end() && it->second == v.slot) continue;
                projv::PendingVoxelOp op{true, v.position, v.slot < palette.size() ? palette[v.slot].packedColor : 0u};
                op.slot = v.slot;
                ops.push_back(op);
            }
            for (auto [at, slot] : present)
                if (!original.count(at)) ops.push_back({false, ivec3(std::get<0>(at), std::get<1>(at), std::get<2>(at)), 0});
            if (!ops.empty()) runtime::editVoxels(world, s.entity, ops);
        }
        for (Game::Tile& t : game.tiles) {
            t.crack = -1.0f;
            t.regrow = -1.0f;
            t.stage = -1;
            t.dropped = !t.spec.present;
        }
        for (Game::Plot& plot : game.plots) plot.cubes.clear();
    }

    inline void clearBomb(World& world, Game& game) {
        if (game.bomb.marker != NullEntity && world.valid(game.bomb.marker)) world.destroy(game.bomb.marker);
        game.bomb = Game::Bomb{};
    }

    inline void startRound(World& world, Game& game) {
        std::vector<Entity> doomed;
        for (Entity e : world.view<projv::Piece>()) doomed.push_back(e);
        for (Entity e : world.view<Thrown>()) doomed.push_back(e);
        for (Entity e : doomed) if (world.valid(e)) world.destroy(e);
        clearBomb(world, game);
        game.mode = modeOf(game, game.round);
        game.phase = Intro;
        game.clock = INTRO_SECONDS;
        game.fallen.clear();
        game.finished = 0;
        for (auto [e, p] : world.view<Player>().each()) ready(world, game, e, p);
        if (game.mode == Blueprint) {
            // A new shape on the display: a column of 0 to 3 on each of its 3 by 3, between 5 and 12 in all.
            game.shape.clear();
            while (game.shape.size() < 5 || game.shape.size() > 12) {
                game.shape.clear();
                for (int z = 0; z < SHAPE_CELLS; z++)
                    for (int x = 0; x < SHAPE_CELLS; x++) {
                        const int height = int(game.uniform(0.0f, float(SHAPE_LAYERS) + 0.99f));
                        for (int y = 0; y < height; y++) game.shape.insert({x, y, z});
                    }
            }
            const int display = plotNamed(Blueprint, true);
            if (display >= 0) {
                Edits edits;
                const int off = (PAD_CELLS - SHAPE_CELLS) / 2;
                auto& list = edits.of(game.plots[size_t(display)].entity);
                for (auto [x, y, z] : game.shape)
                    for (const projv::PendingVoxelOp& op : cubeVoxels(ivec3(x + off, y, z + off), vec3(1.0f, 0.8f, 0.2f))) list.push_back(op);
                edits.flush(world);
            }
        }
        banner(world, "ROUND " + std::to_string(game.round) + ": " + modeName(game.mode), vec3(1.0f, 0.95f, 0.8f), INTRO_SECONDS);
    }

    // The round over: everyone ranked, and the ranks scored.
    inline void endRound(World& world, Game& game) {
        game.phase = Over;
        game.clock = OVER_SECONDS;
        clearBomb(world, game);
        // Best first. Sky Dash: by where they finished, then how far they got. Sky Climb, Blueprint:
        // whoever won, then the highest, the best copy. Otherwise: whoever is still up, then the last
        // to go out.
        std::vector<std::pair<float, Entity>> order;
        for (auto [e, p] : world.view<Player>().each()) {
            const auto it = std::find(game.fallen.begin(), game.fallen.end(), e);
            const float outOrder = it == game.fallen.end() ? -1.0f : float(it - game.fallen.begin());
            float score;
            if (game.mode == Dash) score = p.place ? 1000.0f - float(p.place) : p.furthest;
            else if (game.mode == Climb || game.mode == Blueprint) score = p.place ? 1000.0f : p.best;
            else score = p.alive ? 1000.0f : outOrder;
            order.push_back({score, e});
        }
        std::stable_sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::string winner;
        vec3 colour(1.0f);
        for (size_t i = 0; i < order.size(); i++) {
            // Ties share the better place.
            size_t rank = i;
            while (rank > 0 && order[rank - 1].first == order[i].first) rank--;
            Player& p = world.get<Player>(order[i].second);
            p.points = uint8_t(p.points + POINTS[std::min<size_t>(rank, MAX_PLAYERS - 1)]);
            if (rank == 0) {
                winner += (winner.empty() ? "" : " + ") + p.name;
                colour = colourOf(p.slot);
            }
        }
        // The floors put back now, so they are solid again well before anyone is set down on them.
        restoreStructures(world, game);
        if (game.round < game.options.rounds) {
            banner(world, winner.empty() ? "NOBODY WINS" : winner + " WINS", colour, OVER_SECONDS);
        } else {
            game.phase = MatchOver;
            game.clock = MATCH_OVER_SECONDS;
            int best = -1;
            std::string champion;
            for (auto [e, p] : world.view<Player>().each()) {
                if (p.points > best) { best = p.points; champion = p.name; colour = colourOf(p.slot); }
                else if (p.points == best) champion += " + " + p.name;
            }
            banner(world, champion + " WINS THE PARTY!", colour, MATCH_OVER_SECONDS);
        }
    }

    // ---- Each frame ---------------------------------------------------------------------------------

    inline void runTag(World& world, Game& game, float dt, Edits& edits) {
        Game::Bomb& b = game.bomb;
        if (b.carrier != NullEntity && (!world.valid(b.carrier) || !world.get<Player>(b.carrier).alive)) b.carrier = NullEntity;
        if (b.carrier == NullEntity) {
            if (b.marker != NullEntity && world.valid(b.marker)) world.destroy(b.marker);
            b.marker = NullEntity;
            if (aliveCount(world) < 2 || (b.nextIn -= dt) > 0.0f) return;
            // A new bomb, to someone at random.
            std::vector<Entity> alive;
            for (auto [e, p] : world.view<Player>().each()) if (p.alive) alive.push_back(e);
            b.carrier = alive[size_t(game.uniform(0.0f, float(alive.size()) - 0.01f))];
            b.fuseMax = b.fuse = game.uniform(FUSE_MIN, FUSE_MAX);
            b.passWait = PASS_COOLDOWN;
            b.from = NullEntity;
            b.marker = runtime::instantiatePrefab(world, game.prefab(BOMB), feet(world, b.carrier) + vec3(0.0f, 2.3f, 0.0f));
            if (b.marker != NullEntity) {
                world.emplace<projv::Replicated>(b.marker, projv::Replicated{BOMB, {}});
                runtime::assignNetId(world, b.marker);
            }
            const Player& p = world.get<Player>(b.carrier);
            banner(world, p.name + " HAS THE BOMB", colourOf(p.slot), 1.8f);
            return;
        }
        // Passed on a touch -- not straight back to whoever passed it.
        b.passWait -= dt;
        b.fromWait -= dt;
        const vec3 at = feet(world, b.carrier);
        if (b.passWait <= 0.0f) {
            for (auto [e, p] : world.view<Player>().each()) {
                if (e == b.carrier || !p.alive || (e == b.from && b.fromWait > 0.0f)) continue;
                const vec3 d = feet(world, e) - at;
                if (glm::length(vec3(d.x, 0, d.z)) < PASS_REACH && std::abs(d.y) < 1.5f) {
                    projv::core::info("party: {} passed the bomb to {}", world.get<Player>(b.carrier).name, p.name);
                    b.from = b.carrier;
                    b.fromWait = NO_TAG_BACKS;
                    b.carrier = e;
                    b.passWait = PASS_COOLDOWN;
                    boom(world, feet(world, e) + vec3(0.0f, 1.0f, 0.0f), -1.0f);
                    break;
                }
            }
        }
        const vec3 holder = feet(world, b.carrier);
        if (b.marker != NullEntity && world.valid(b.marker))
            world.patch<projv::Transform>(b.marker, [&](projv::Transform& t) { t.position = holder + vec3(0.0f, 2.3f, 0.0f); });
        // Bang: the holder out, the floor round them gone, everyone near thrown.
        if ((b.fuse -= dt) <= 0.0f) {
            const Entity victim = b.carrier;
            b.carrier = NullEntity;
            b.nextIn = NEXT_BOMB_SECONDS;
            dropAround(game, holder, CRATER_RADIUS, edits);
            std::vector<std::pair<Entity, vec3>> pushed;
            for (auto [e, p] : world.view<Player>().each()) {
                if (e == victim || !p.alive) continue;
                vec3 d = feet(world, e) - holder;
                d.y = 0.0f;
                const float distance = glm::length(d);
                if (distance < 6.0f) pushed.push_back({e, glm::normalize(d + vec3(1e-3f)) * BOMB_KNOCK * (1.0f - distance / 8.0f)});
            }
            for (auto [e, v] : pushed) knock(world, e, v);
            boom(world, holder + vec3(0.0f, 1.0f, 0.0f), CRATER_RADIUS);
            eliminate(world, game, victim, "BLEW UP");
        }
    }

    // Sky Dash's moving parts: always turning and sliding, whatever game is on (they are far from the
    // others), and throwing whoever they meet while it is on.
    inline void runHazards(World& world, Game& game, float dt) {
        game.hazardClock += dt;
        const float t = game.hazardClock;
        const vec3 ring(DASH_X, SWEEPER_HEIGHT, DASH_RING_Z);
        for (int i = 0; i < 2; i++) {
            if (!world.valid(game.sweepers[i])) continue;
            // One each way, at speeds that drift in and out of step.
            const float angle = i == 0 ? t * SWEEPER_SPEED : -t * SWEEPER_SPEED * 0.73f + 1.2f;
            world.patch<projv::Transform>(game.sweepers[i], [&](projv::Transform& tr) {
                tr.position = ring;
                tr.rotation = glm::angleAxis(angle, vec3(0, 1, 0));
            });
        }
        const float narrowZ = -25.0f + 25 * PITCH;
        for (int i = 0; i < 3; i++) {
            if (!world.valid(game.pushers[i])) continue;
            const float x = DASH_X + std::sin(t * PUSHER_SPEED + float(i) * 2.1f) * PUSHER_REACH;
            world.patch<projv::Transform>(game.pushers[i], [&](projv::Transform& tr) { tr.position = vec3(x, 0.0f, narrowZ + 3.0f + float(i) * 4.0f); });
        }
        if (game.mode != Dash || game.phase != Playing) return;
        std::vector<std::pair<Entity, vec3>> hit;
        for (auto [e, p] : world.view<Player>().each()) {
            p.hazardWait = std::max(0.0f, p.hazardWait - dt);
            if (!p.alive || p.hazardWait > 0.0f) continue;
            const size_t before = hit.size();
            const vec3 at = feet(world, e);
            for (int i = 0; i < 2; i++) {
                if (!world.valid(game.sweepers[i])) continue;
                const projv::Transform& tr = world.get<projv::Transform>(game.sweepers[i]);
                const vec3 along = tr.rotation * vec3(1, 0, 0), side(-along.z, 0.0f, along.x), d = at - tr.position;
                const float a = glm::dot(d, along), off = glm::dot(d, side);
                // Under its top, near its line: swept along the way it turns.
                if (std::abs(a) < SWEEPER_LENGTH * 0.5f && std::abs(off) < 0.55f && at.y < SWEEPER_HEIGHT + 0.5f) {
                    const float turning = (i == 0 ? 1.0f : -1.0f) * (a >= 0.0f ? -1.0f : 1.0f);
                    hit.push_back({e, side * turning * HAZARD_KNOCK});
                }
            }
            for (int i = 0; i < 3; i++) {
                if (!world.valid(game.pushers[i])) continue;
                const vec3 d = at - world.get<projv::Transform>(game.pushers[i]).position;
                if (std::abs(d.x) < PUSHER_HALF + 0.35f && std::abs(d.z) < 0.5f + 0.35f && at.y < 1.5f)
                    hit.push_back({e, vec3(d.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f) * HAZARD_KNOCK});
            }
            if (hit.size() > before) {
                hit.resize(before + 1);   // one throw at a time
                p.hazardWait = 0.6f;
            }
        }
        for (auto [e, v] : hit) knock(world, e, v);
    }

    inline void runDash(World& world, Game& game) {
        for (auto [e, p] : world.view<Player>().each()) {
            if (!p.alive || p.place) continue;
            const vec3 at = feet(world, e);
            if (at.y < -1.0f) continue;
            p.furthest = std::max(p.furthest, at.z);
            for (int c = 2; c > p.checkpoint; c--)
                if (at.z > DASH_CHECKPOINTS[c] - 1.0f) { p.checkpoint = c; break; }
            if (at.z > DASH_FINISH) {
                p.place = ++game.finished;
                static const char* places[] = {"", "1ST", "2ND", "3RD", "4TH"};
                banner(world, p.name + " IS HOME " + places[std::min(p.place, 4)], colourOf(p.slot), 2.0f);
            }
        }
    }

    // Sky Climb: how high everyone has stood; the first to stand at the goal has won.
    inline void runClimb(World& world, Game& game, float dt) {
        if ((game.surveyClock -= dt) <= 0.0f) {
            game.surveyClock = 0.4f;
            for (size_t i = 0; i < game.plots.size(); i++)
                if (game.plots[i].spec.mode == Climb) surveyPlot(world, game, i);
        }
        for (auto [e, p] : world.view<Player>().each()) {
            if (!p.alive) continue;
            const runtime::CharacterState state = runtime::characterState(world, e);
            if (!state.onGround) continue;
            p.best = std::max(p.best, state.position.y);
            if (state.position.y >= CLIMB_GOAL - 0.05f && !p.place) {
                p.place = 1;
                banner(world, p.name + " REACHED THE TOP!", colourOf(p.slot), 2.0f);
            }
        }
    }

    // Blueprint: how much of the shape each pad has -- right cubes, less wrong ones -- and whether any
    // has it exactly.
    inline void runBlueprint(World& world, Game& game, float dt) {
        if ((game.surveyClock -= dt) > 0.0f) return;
        game.surveyClock = 0.3f;
        const int off = (PAD_CELLS - SHAPE_CELLS) / 2;
        for (auto [e, p] : world.view<Player>().each()) {
            const int own = ownPlot(Blueprint, p.slot);
            if (own < 0) continue;
            surveyPlot(world, game, size_t(own));
            int right = 0, wrong = 0;
            for (auto [x, y, z] : game.plots[size_t(own)].cubes) {
                if (game.shape.count({x - off, y, z - off})) right++;
                else wrong++;
            }
            p.best = game.shape.empty() ? 0.0f : std::max(0.0f, float(right - wrong) / float(game.shape.size()));
            if (right == int(game.shape.size()) && wrong == 0 && !p.place) {
                p.place = 1;
                banner(world, p.name + " BUILT IT!", colourOf(p.slot), 2.0f);
            }
        }
    }

    inline void rules(projv::Application& app) {
        World& world = app.world;
        Game& game = world.ctx().get<Game>();
        const float dt = app.time().delta;
        if (dt <= 0.0f) return;
        balanceBots(world, game);
        Edits edits;

        game.clock -= dt;
        if (game.phase == Intro && game.clock <= 0.0f) {
            game.phase = Playing;
            game.clock = ROUND_SECONDS[game.mode];
            if (game.mode == Tag) game.bomb.nextIn = 0.5f;
            banner(world, "GO!", vec3(1.0f, 1.0f, 0.6f), 1.2f);
        } else if ((game.phase == Over || game.phase == MatchOver) && game.clock <= 0.0f) {
            if (game.phase == MatchOver) {
                game.round = 1;
                for (auto [e, p] : world.view<Player>().each()) p.points = 0;
            } else {
                game.round++;
            }
            startRound(world, game);
        }

        for (auto [e, bot] : world.view<Bot>().each()) botThink(world, game, e, bot, dt, edits);

        // Players: cooldowns, blocks growing back, facing, falls.
        std::vector<Entity> fell, back;
        for (auto [e, p] : world.view<Player>().each()) {
            p.shove = std::max(0.0f, p.shove - dt);
            p.toss = std::max(0.0f, p.toss - dt);
            p.block = std::max(0.0f, p.block - dt);
            if (BLOCK_REGROW[game.mode] > 0.0f && p.blocks < BLOCKS[game.mode] && (p.regrow -= dt) <= 0.0f) {
                p.blocks++;
                p.regrow = BLOCK_REGROW[game.mode];
            }
            if (std::abs(p.yaw - p.shownYaw) > 0.01f) {
                p.shownYaw = p.yaw;
                world.patch<projv::Transform>(e, [&](projv::Transform& t) { t.rotation = facing(p.yaw); });
            }
            if (p.alive && feet(world, e).y < FALLEN_BELOW) fell.push_back(e);
            if (!p.alive && !p.out && (p.respawnIn -= dt) <= 0.0f) back.push_back(e);
        }
        for (Entity e : fell) {
            Player& p = world.get<Player>(e);
            const bool comesBack = game.mode == Dash || game.mode == Climb || game.mode == Blueprint;
            if (game.phase != Playing) {
                moveCharacter(world, e, startPoint(game.mode, p.slot));
            } else if (comesBack) {
                p.alive = false;
                p.respawnIn = RESPAWN_SECONDS;
                moveCharacter(world, e, penPoint(p.slot));
            } else {
                eliminate(world, game, e, "FELL");
            }
        }
        for (Entity e : back) {
            Player& p = world.get<Player>(e);
            p.alive = true;
            moveCharacter(world, e, game.mode == Dash ? checkpoint(p.checkpoint, p.slot) : startPoint(game.mode, p.slot));
        }

        runHazards(world, game, dt);
        if (game.phase == Playing) {
            // What crumbles cracks under foot, and drops.
            for (auto [e, p] : world.view<Player>().each()) {
                if (!p.alive) continue;
                const vec3 at = feet(world, e);
                for (vec3 offset : {vec3(0.0f), vec3(0.25f, 0, 0.25f), vec3(-0.25f, 0, 0.25f), vec3(0.25f, 0, -0.25f), vec3(-0.25f, 0, -0.25f)})
                    if (Game::Tile* t = tileUnder(game, at + offset); t && t->crumbles && !t->dropped && t->crack < 0.0f) t->crack = 0.0f;
            }
            for (Game::Tile& t : game.tiles) {
                if (t.dropped && t.regrow > 0.0f && (t.regrow -= dt) <= 0.0f) lay(game, t, nullptr, edits);
                if (t.crack < 0.0f || t.dropped) continue;
                t.crack += dt;
                const bool kind = game.mode == Dash;
                recolour(game, t, t.crack < (kind ? DASH_CRACK_SECONDS : CRACK_SECONDS) ? 0 : 1, edits);
                if (t.crack >= (kind ? DASH_DROP_SECONDS : DROP_SECONDS)) drop(game, t, edits);
            }
            if (game.mode == Tag) runTag(world, game, dt, edits);
            if (game.mode == Dash) runDash(world, game);
            if (game.mode == Climb) runClimb(world, game, dt);
            if (game.mode == Blueprint) runBlueprint(world, game, dt);

            // Lobbed bombs burst where they land: slowed by what they hit, near a player, or late.
            std::vector<std::pair<Entity, vec3>> bursting;
            for (auto [e, thrown] : world.view<Thrown>().each()) {
                thrown.age += dt;
                if (!runtime::hasBody(world, e)) continue;
                const runtime::BodyState pose = runtime::bodyPose(world, e);
                bool now = (thrown.fuse -= dt) <= 0.0f || pose.position.y < FALLEN_BELOW;
                now = now || (thrown.age > 0.1f && glm::length(pose.linearVelocity - thrown.velocity) > 4.0f);
                for (auto [other, p] : world.view<Player>().each())
                    if (p.alive && other != thrown.by && glm::distance(feet(world, other) + vec3(0, 0.9f, 0), pose.position) < 0.9f) now = true;
                thrown.velocity = pose.linearVelocity;
                if (now) bursting.push_back({e, pose.position});
            }
            for (auto [e, at] : bursting) {
                if (at.y > FALLEN_BELOW) burst(world, game, at, edits);
                world.destroy(e);
            }

            // Who has won.
            const int players = playersIn(world), standing = standingCount(world);
            bool done = game.clock <= 0.0f;
            if (game.mode == Dash) done = done || game.finished >= std::max(1, players - 1) || game.finished >= players;
            else if (game.mode == Climb || game.mode == Blueprint)
                for (auto [e, p] : world.view<Player>().each()) done = done || p.place;
            else done = done || (players > 1 ? standing <= 1 : standing == 0);
            if (done) endRound(world, game);
        }
        edits.flush(world);

        // What has fallen into the lava is gone.
        if ((game.sweepClock -= dt) <= 0.0f) {
            game.sweepClock = 0.5f;
            std::vector<Entity> sunk;
            for (Entity e : world.view<projv::Piece>())
                if (runtime::hasBody(world, e) && runtime::bodyPose(world, e).position.y < LAVA_Y + 4.0f) sunk.push_back(e);
            for (Entity e : sunk) world.destroy(e);
        }

        // Each player told how things stand, fifteen times a second.
        if ((game.stateClock -= dt) <= 0.0f) {
            game.stateClock = 1.0f / 15.0f;
            State state;
            state.mode = game.mode;
            state.phase = game.phase;
            state.round = uint8_t(game.round);
            state.rounds = uint8_t(game.options.rounds);
            state.clock = std::max(0.0f, game.clock);
            state.fuse = game.mode == Tag && game.bomb.carrier != NullEntity ? game.bomb.fuse / game.bomb.fuseMax : -1.0f;
            for (auto [e, p] : world.view<Player>().each()) {
                State::Player& s = state.players[p.slot];
                s.present = true;
                s.alive = !p.out;
                s.carrier = game.bomb.carrier == e;
                s.name = p.name;
                s.points = p.points;
                s.place = uint8_t(p.place);
                s.extra = game.mode == Dash ? p.furthest - DASH_CHECKPOINTS[0]
                        : (game.mode == Climb || game.mode == Blueprint) ? p.best : 0.0f;
            }
            for (auto [e, p] : world.view<Player>().each()) {
                if (!p.client) continue;
                state.you = int8_t(p.slot);
                state.shove = p.shove / SHOVE_COOLDOWN;
                state.toss = THROW_COOLDOWN[game.mode] > 0.0f ? p.toss / THROW_COOLDOWN[game.mode] : -1.0f;
                state.blocks = uint8_t(std::max(0, p.blocks));
                projv::net::BitWriter w;
                state.write(w);
                runtime::sendToClient(world, p.client, w.data(), false);
            }
        }
    }

    // Something has dropped or been cut loose: its own piece, falling through everything but the lava,
    // past whoever stood on it.
    inline void onBroken(World& world, const projv::BodyBroken& e) {
        for (Entity piece : e.pieces)
            if (world.valid(piece))
                world.emplace_or_replace<projv::CollisionGroups>(piece, projv::CollisionGroups{{"falling"}, {"floor", "falling", "player"}, 0});
    }

    // ---- Installing it ------------------------------------------------------------------------------

    // Loads the scene, its physics and the rules into `app`, and serves it. False if it cannot listen.
    inline bool installServer(projv::Application& app, ServerOptions options) {
        World& world = app.world;
        projv::Scene& loaded = world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(options.folder));
        if (loaded.components.empty()) {
            projv::core::error("party: no arenas in '{}' (run from the binary's folder, or --write-assets)", options.folder);
            return false;
        }
        runtime::installSceneBridge(app);
        runtime::PhysicsConfig physics;
        physics.maxPieces = 1500;
        runtime::installPhysics(app, physics);
        runtime::loadPhysicsMaterials(world, (std::filesystem::path(options.folder) / MATERIALS).string());
        for (const char* group : {"player", "floor", "falling"}) runtime::definePhysicsGroup(world, group);
        runtime::spawnEntities(world);

        Game& game = world.ctx().emplace<Game>();
        game.options = options;
        // The floors and the plots, as the files have them, and every tile.
        const auto& scene = world.ctx().get<projv::Scene>();
        for (auto [e, collider] : world.view<projv::StaticCollider>().each()) {
            if (!collider.splits) continue;
            const projv::ComponentHandle h = runtime::voxelComponentOf(world, e);
            if (h == projv::INVALID_COMPONENT_HANDLE) continue;
            game.structures.push_back({e, projv::utils::componentVoxels(scene, h)});
            world.emplace_or_replace<projv::CollisionGroups>(e, projv::CollisionGroups{{"floor"}, {}, 0});
            const std::string& name = scene.components[h].name;
            for (const FloorSpec& spec : floorSpecs())
                if (name == spec.name) {
                    uint8_t anchor = projv::INVALID_MATERIAL;
                    const auto& palette = scene.components[h].materialPalette;
                    for (size_t slot = 0; slot < palette.size(); slot++)
                        if (palette[slot].packedColor == ANCHOR_COLOUR) anchor = uint8_t(slot);
                    game.floors.push_back({e, spec, anchor});
                }
            const auto& plots = plotSpecs();
            if (game.plots.empty()) game.plots.resize(plots.size());
            for (size_t i = 0; i < plots.size(); i++)
                if (name == plots[i].name) game.plots[i] = {e, plots[i], {}};
        }
        for (size_t f = 0; f < game.floors.size(); f++) {
            const FloorSpec& spec = game.floors[f].spec;
            for (const TileSpec& t : tilesOf(spec)) {
                Game::Tile tile;
                tile.floor = f;
                tile.spec = t;
                tile.fixed = spec.fixed;
                tile.crumbles = spec.crumbles;
                tile.dropped = !t.present;
                game.tileAt[{f, t.i, t.j}] = game.tiles.size();
                game.tiles.push_back(tile);
            }
        }
        // Each tile's voxels as the files have them.
        for (const Game::Structure& st : game.structures)
            for (size_t f = 0; f < game.floors.size(); f++) {
                if (game.floors[f].entity != st.entity) continue;
                const auto& palette = scene.components[runtime::voxelComponentOf(world, st.entity)].materialPalette;
                for (const auto& v : st.original) {
                    auto it = game.tileAt.find({f, v.position.x / PITCH_VOXELS, v.position.z / PITCH_VOXELS});
                    if (it == game.tileAt.end()) continue;
                    projv::PendingVoxelOp op{true, v.position, v.slot < palette.size() ? palette[v.slot].packedColor : 0u};
                    op.slot = v.slot;
                    game.tiles[it->second].original.push_back(op);
                }
            }
        if (game.floors.size() != floorSpecs().size())
            projv::core::warn("party: found {} of {} floors in '{}'", game.floors.size(), floorSpecs().size(), options.folder);

        app.events().on<projv::BodyBroken>([&world](const projv::BodyBroken& e) { onBroken(world, e); });
        app.events().on<projv::BodyEmptied>([&world](const projv::BodyEmptied& e) {
            if (world.valid(e.entity) && world.all_of<projv::Piece>(e.entity)) world.destroy(e.entity);
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
        // A person takes a bot's seat; with every seat a person's, nobody more can join.
        config.spawnPlayer = [](World& w, runtime::ClientId id) {
            Game& g = w.ctx().get<Game>();
            if (freeSlot(w) < 0)
                for (auto [e, p] : w.view<Player>().each())
                    if (!p.client) {
                        if (g.bomb.carrier == e) g.bomb.carrier = NullEntity;
                        w.destroy(e);
                        break;
                    }
            return makePlayer(w, g, id, "PLAYER");
        };
        // A person's walking: still while the round is not on or they are out. It carries their
        // shoves (their client adds them), so it may be faster than a walk.
        config.applyInput = [](World& w, runtime::ClientId, Entity player, vec3 move, bool jump) {
            const Player* p = w.valid(player) ? w.try_get<Player>(player) : nullptr;
            if (!p || !p->alive || w.ctx().get<Game>().phase != Playing) {
                runtime::setCharacterInput(w, player, vec3(0.0f));
                return;
            }
            move.y = 0.0f;
            const float most = WALK_SPEED * CARRIER_SPEED + SHOVE_SPEED + BOMB_KNOCK;
            if (glm::length(move) > most) move = glm::normalize(move) * most;
            runtime::setCharacterInput(w, player, move, jump);
        };
        config.onMessage = [](World& w, runtime::ClientId, Entity player, std::span<const uint8_t> message) {
            onMessage(w, w.ctx().get<Game>(), player, message);
        };
        if (!runtime::installServer(app, projv::net::makeEnetTransport(), config)) return false;
        app.addSystem(projv::Stage::Update, "party: rules", rules);
        app.addSystem(projv::Stage::Startup, "party: first round", [](projv::Application& a) {
            World& w = a.world;
            Game& g = w.ctx().get<Game>();
            // Sky Dash's moving parts, made once and kept turning.
            for (Entity& s : g.sweepers) s = runtime::instantiatePrefab(w, g.prefab(SWEEPER), vec3(DASH_X, SWEEPER_HEIGHT, DASH_RING_Z));
            for (Entity& p : g.pushers) p = runtime::instantiatePrefab(w, g.prefab(PUSHER), vec3(DASH_X, 0.0f, 0.0f));
            for (auto [list, count, name] : {std::tuple{g.sweepers, 2, SWEEPER}, std::tuple{g.pushers, 3, PUSHER}})
                for (int i = 0; i < count; i++) {
                    if (list[i] == NullEntity) continue;
                    w.emplace<projv::Replicated>(list[i], projv::Replicated{name, {}});
                    runtime::assignNetId(w, list[i]);
                }
            for (int i = 0; i < g.options.players; i++) balanceBots(w, g);
            startRound(w, g);
        });
        return true;
    }
}
