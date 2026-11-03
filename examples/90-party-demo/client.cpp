// ProjectV Party: a client
//
// Five little games for three or four friends, a round each in turn, over a sea of lava:
//   BOMB TAG   the bomb goes to whoever its holder touches; when it goes off, so does its holder
//   SKY DASH   a race: a crumbling strip, sweepers to jump, gaps to bridge with tiles, pushers
//   SKY CLIMB  build up -- jump, lay a cube under you -- to the gold; bomb the others' towers down
//   CRUMBLE    two floors of tiles that drop a moment after you step on them; last one up wins
//   BLUEPRINT  copy the gold shape on the display onto your pad, cube by cube; first exact copy wins
// Every round ranks the players (3, 2, 1, 0 points); the most after five rounds takes the party.
// Bots fill the seats nobody has.
//
//   ./party_demo                        host a party here (others can join it) and play in it
//   ./party_demo --host 10.0.0.5        join someone else's, or a party_demo_server
//   ./party_demo --name ann --port 27100
//   ./party_demo --players 3 --rounds 10 --mode climb   when hosting: seats, rounds, one game only
//   ./party_demo --headless --seconds 40 ...            no window: for the self-test (selftest.sh)
//       --wander              walk about and shove
//       --build               in a game of cubes, lay them by aiming, as first person does
//       --expect-rounds 2     see at least this many rounds begin
//
// Controls, made for a laptop: W/A/S/D walk (the way the camera faces), Space jumps; the trackpad or
// the arrow keys turn the camera (Esc frees the cursor, a click takes it back). V switches between
// third and first person. F (or a click) shoves whoever is in front of you; C (or a right click) lays
// a block -- in first person where the crosshair points, a ring marking the spot; X takes a cube away;
// R throws a bomb -- hold it to throw further, and a ring shows where it will land.
//
// The server decides everything (game.h); this sends what you do and shows what it says. Your own
// player walks here at once (prediction); everyone and everything else is drawn where the server has
// it. When you are shoved the server says so, and your walking carries the push, as the server's does.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>

#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "game.h"
#include "graphics/disk_io.h"
#include "graphics/engine_shaders.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/camera.h"
#include "graphics/render_instance.h"
#include "ui/ui.h"
#include "ui/user.h"
#include "utils/material.h"
#include "voxel_renderer.h"

using namespace party;
using projv::core::vec4;
namespace fs = std::filesystem;

namespace {
    struct Options {
        std::string host;            // empty: host here
        uint16_t    port = PORT;
        std::string name = "PLAYER";
        int         players = MAX_PLAYERS;
        int         rounds = ROUNDS;
        int         only = -1;
        bool        headless = false;
        double      seconds = 0.0;
        bool        wander = false;
        bool        build = false;
        int         expectRounds = 0;
    };
    Options options;
    int exitCode = 0;
    std::unique_ptr<projv::Application> hosted;   // the party this client hosts, if it does

    struct Proto {
        projv::ComponentHandle node = projv::INVALID_COMPONENT_HANDLE;
    };
    struct Chip { double at; };
    struct Flash { Entity e; float age = 0.0f, size = 1.0f; };
    struct Burst { vec3 centre; float size; std::vector<uint32_t> colours; int count; float speed; };
    constexpr size_t MAX_CHIPS = 160;

    struct Client {
        State  state;
        bool   haveState = false;
        float  yaw = 0.0f, pitch = -0.42f;   // the camera's
        float  facing = 0.0f;                // where the player faces: the way it last walked
        vec3   eye{0.0f, 20.0f, -30.0f}, look{0.0f, -0.4f, 1.0f};
        vec3   knock{0.0f};                  // a shove still carrying this player
        bool   hop = false;
        float  lookClock = 0.0f, titleClock = 0.0f, flash = 0.0f, shake = 0.0f;
        std::string banner;
        vec3   bannerColour{1.0f};
        float  bannerFor = 0.0f;
        bool   firstPerson = false;          // V: through the player's own eyes
        std::optional<vec3> aim;             // first person, in a game of cubes: where one would go
        float  charge = -1.0f;               // how long R has been held; < 0 not held
        std::optional<vec3> marker;          // where a bomb thrown now would land
        Proto  fireball;
        std::vector<Flash> flashes;
        std::vector<Burst> bursts;
        std::mt19937 random{7};
        double clock = 0.0;
        Mode   facedFor = Mode(255);         // the camera turned toward the middle for this game
        uint8_t lastRound = 0;
        // What a headless client saw, for its checks.
        int      states = 0, banners = 0, knocks = 0, placed = 0;
        uint16_t firstRound = 0, rounds = 0;
    };

    // What the HUD document shows (ui/hud.rml), bound to its model "hud" and filled each frame.
    struct Hud {
        std::string top, banner, bannerColour = "#ffffff", hint, info, fuseColour = "#ffa626";
        std::vector<std::vector<std::string>> scores;   // name, points, extra, colour, "1" for yours
        float fuse = -1.0f, shove = 0.0f, toss = -1.0f, charge = -1.0f, flash = 0.0f;
        bool  fuseMine = false, alive = false, crosshair = false;
        bool  markerShown = false, aimShown = false;
        float markerX = 0, markerY = 0, markerR = 0, aimX = 0, aimY = 0, aimR = 0;
        int   fps = 0;
        bool  showFps = false;
    };
    // The shared menu's settings (examples/shared/ui/menu.rml), bound to its model "menu".
    struct Menu {
        std::string game = "Party";
        float sensitivity = 1.0f;
        bool  invertY = false, showFps = false, open = false;
    };

    float unit(Client& c) { return std::uniform_real_distribution<float>(-1.0f, 1.0f)(c.random); }

    Entity own(World& world) { return runtime::entityForNetId(world, runtime::sessionStats(world).player); }

    // Where the own player is drawn (its Transform: between its last two steps).
    vec3 drawnFeet(World& world, Entity e) {
        const projv::Transform* t = e != NullEntity && world.valid(e) ? world.try_get<projv::Transform>(e) : nullptr;
        return t ? t->position : vec3(0.0f);
    }

    void send(World& world, ToServer kind) {
        projv::net::BitWriter w;
        w.varint(uint8_t(kind));
        runtime::sendToServer(world, w.data(), true);
    }

    // Where the player faces, told the server: what a shove or a throw goes by.
    void sendFacing(World& world, float facing, bool reliable) {
        projv::net::BitWriter w;
        w.varint(uint8_t(ToServer::Look));
        w.f32(facing);
        runtime::sendToServer(world, w.data(), reliable);
    }

    bool alive(const Client& c) { return c.haveState && c.state.you >= 0 && c.state.players[c.state.you].alive; }

    // ---- What only this client draws -----------------------------------------------------------------

    // A prototype: an asset node with its voxels inside, parked far below; copies share its geometry.
    template <typename ColourAt>
    Proto prototype(projv::Scene& scene, const char* name, int size, float voxel, float glow, ColourAt colourAt) {
        using namespace projv;
        Proto p;
        p.node = utils::addComponent(scene, ComponentKind::Asset, name, INVALID_COMPONENT_HANDLE, 4, 1.0f);
        ComponentHandle chunk = utils::addComponent(scene, ComponentKind::Chunk, name, p.node, 16, voxel);
        std::vector<PendingVoxelOp> ops;
        for (int z = 0; z < size; z++)
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++)
                    if (uint32_t colour = colourAt(x, y, z)) ops.push_back({true, projv::core::ivec3(x, y, z), colour});
        utils::queueVoxelAdd(scene, chunk, ops);
        utils::updateScene(scene);
        utils::setComponentTransform(scene, chunk, vec3(-0.5f * float(size) * voxel), quat(1, 0, 0, 0), 1.0f);
        utils::setComponentTransform(scene, p.node, vec3(0, -1000, 0), quat(1, 0, 0, 0), 1.0f);
        // It glows: it lights what is near it (the renderer's ray-traced light).
        const auto& palette = scene.components[chunk].materialPalette;
        for (size_t slot = 0; slot < palette.size(); slot++)
            utils::setMaterialProperties(scene, chunk, uint8_t(slot), palette[slot].packedColor, packSurfaceWord(0.0f, 0.0f, 0.0f, 1.0f),
                                         packExtraWord(glow, 0.0f, 0u));
        return p;
    }

    Entity copyOf(World& world, const Proto& proto, vec3 at, float scale = 1.0f) {
        auto& scene = world.ctx().get<projv::Scene>();
        const projv::ComponentHandle node = projv::utils::duplicateComponent(scene, proto.node);
        projv::utils::setComponentTransform(scene, node, at, quat(1, 0, 0, 0), scale);
        return runtime::spawnComponent(world, node, projv::LinkMode::Root, projv::OnUnlink::Destroy);
    }

    void burst(World& world, vec3 at, std::vector<uint32_t> colours, int count, float speed, float size = 0.12f) {
        world.ctx().get<Client>().bursts.push_back({at, size, std::move(colours), count, speed});
    }

    void heard(World& world, std::span<const uint8_t> message) {
        Client& c = world.ctx().get<Client>();
        projv::net::BitReader r(message);
        const auto kind = ToClient(r.varint());
        if (kind == ToClient::State) {
            State state;
            if (!state.read(r)) return;
            c.state = state;
            c.haveState = true;
            c.states++;
            if (!c.firstRound) c.firstRound = state.round;
            if (state.round != c.lastRound) {
                c.lastRound = state.round;
                c.rounds++;
            }
            return;
        }
        if (kind == ToClient::Banner) {
            const std::vector<uint8_t> text = r.bytes(64);
            const vec3 colour = readVec(r);
            const float seconds = r.f32();
            if (r.failed) return;
            c.banner.assign(text.begin(), text.end());
            c.bannerColour = colour;
            c.bannerFor = std::clamp(seconds, 0.5f, 12.0f);
            c.banners++;
            return;
        }
        if (kind == ToClient::Knock) {
            const vec3 velocity = readVec(r);
            if (r.failed || !finite(velocity) || glm::length(velocity) > 40.0f) return;
            c.knock = vec3(velocity.x, 0.0f, velocity.z);
            c.hop = true;
            c.flash = 1.0f;
            c.knocks++;
            return;
        }
        if (options.headless) return;   // the rest is for looking at
        if (kind == ToClient::Boom) {
            const vec3 at = readVec(r);
            const float radius = r.f32();
            if (r.failed || !finite(at) || !std::isfinite(radius)) return;
            if (radius > 0.0f) {
                c.flashes.push_back({copyOf(world, c.fireball, at, 0.6f), 0.0f, radius * 0.6f});
                burst(world, at, {rgb(1.0f, 0.6f, 0.15f), rgb(0.95f, 0.85f, 0.3f), rgb(0.3f, 0.3f, 0.3f)}, 30, 9.0f);
                c.shake = std::max(c.shake, 1.0f - std::min(glm::distance(at, c.eye) / 35.0f, 1.0f));
            } else if (radius == 0.0f) {
                burst(world, at, {rgb(0.95f, 0.95f, 0.9f), rgb(0.8f, 0.85f, 0.9f)}, 8, 3.0f, 0.08f);
            } else {
                burst(world, at, {rgb(1.0f, 0.85f, 0.2f), rgb(1.0f, 0.4f, 0.1f)}, 14, 4.0f, 0.07f);
            }
        }
    }

    void effects(projv::Application& app) {
        World& world = app.world;
        Client& c = world.ctx().get<Client>();
        const float dt = app.time().delta;
        c.clock += dt;
        c.flash = std::max(0.0f, c.flash - dt * 2.5f);
        c.shake = std::max(0.0f, c.shake - dt * 2.0f);
        c.bannerFor -= dt;
        if (options.headless) return;
        // Fireballs grow and are gone in half a second.
        for (Flash& f : c.flashes) {
            f.age += dt;
            if (!world.valid(f.e)) continue;
            if (f.age > 0.45f) { world.destroy(f.e); continue; }
            world.patch<projv::Transform>(f.e, [&](projv::Transform& t) { t.scale = f.size * (0.4f + f.age * 3.5f); });
        }
        std::erase_if(c.flashes, [&](const Flash& f) { return f.age > 0.45f || !world.valid(f.e); });

        // Chips: made for each burst, gone after two seconds, never more than MAX_CHIPS.
        std::vector<Entity> old;
        for (auto [e, chip] : world.view<Chip>().each()) if (c.clock - chip.at > 2.0) old.push_back(e);
        for (Entity e : old) world.destroy(e);
        auto& scene = world.ctx().get<projv::Scene>();
        for (const Burst& b : c.bursts) {
            for (int i = 0; i < b.count && world.view<Chip>().size() < MAX_CHIPS; i++) {
                const uint32_t colour = b.colours.empty() ? rgb(0.5f, 0.5f, 0.5f) : b.colours[c.random() % b.colours.size()];
                projv::ComponentHandle h = projv::utils::addComponent(scene, projv::ComponentKind::Chunk, "chip", projv::INVALID_COMPONENT_HANDLE,
                                                                      4, b.size * 0.5f);
                std::vector<projv::PendingVoxelOp> cube;
                for (int k = 0; k < 8; k++) cube.push_back({true, projv::core::ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1), colour});
                projv::utils::queueVoxelAdd(scene, h, cube);
                projv::utils::updateScene(scene);
                const vec3 spread(unit(c), unit(c), unit(c));
                projv::utils::setComponentTransform(scene, h, b.centre + spread * 0.4f, quat(1, 0, 0, 0), 1.0f);
                Entity e = runtime::spawnComponent(world, h, projv::LinkMode::Root, projv::OnUnlink::Destroy);
                projv::RigidBody body;
                body.density = 1500.0f;
                world.emplace<projv::RigidBody>(e, body);
                world.emplace<projv::CollisionGroups>(e, projv::CollisionGroups{{"falling"}, {"falling", "player"}, 0});
                world.emplace<Chip>(e, Chip{c.clock});
                runtime::setVelocity(world, e, glm::normalize(spread + vec3(0, 1.2f, 0)) * (b.speed * (0.6f + 0.6f * std::abs(unit(c)))),
                                     spread * 8.0f);
            }
        }
        c.bursts.clear();
    }

    // ---- Playing --------------------------------------------------------------------------------------

    // The walking sent this step: `move`, carried on by any shove, with a hop when shoved.
    void walk(World& world, Client& c, vec3 move, bool jump, float dt) {
        const float left = glm::length(c.knock);
        if (left > 0.0f) c.knock *= std::max(0.0f, 1.0f - KNOCK_DECAY * dt / left);
        runtime::setLocalInput(world, move + c.knock, jump || c.hop);
        c.hop = false;
    }

    // Opens or closes the shared menu (D23): the cursor is the menu's while it is open.
    void setMenu(projv::Application& app, bool open) {
        Menu& menu = app.world.ctx().get<Menu>();
        auto& renderInstance = app.world.ctx().get<projv::graphics::RenderInstance>();
        menu.open = open;
        if (open) projv::ui::user::show(app, "menu");
        else projv::ui::user::hide(app, "menu");
        projv::graphics::setCursorCaptured(app, renderInstance, !open);
    }

    // The HUD and the menu: documents over the frame (docs/plans/ui.md, U5).
    void installHud(projv::Application& app, const fs::path& here) {
        World& world = app.world;
        Hud& h = world.ctx().emplace<Hud>();
        Menu& m = world.ctx().emplace<Menu>();
        const std::string ui = (here / "shared_ui").string();
        projv::ui::user::loadFont(app, ui + "/fonts/LatoLatin-Regular.ttf");
        projv::ui::user::loadFont(app, ui + "/fonts/LatoLatin-Bold.ttf");
        projv::ui::user::model(app, "hud")
            .bind("top", &h.top).bind("banner", &h.banner).bind("bannerColour", &h.bannerColour).bind("hint", &h.hint)
            .bind("info", &h.info).bind("scores", &h.scores).bind("fuse", &h.fuse).bind("fuseMine", &h.fuseMine)
            .bind("fuseColour", &h.fuseColour).bind("alive", &h.alive).bind("shove", &h.shove).bind("toss", &h.toss)
            .bind("charge", &h.charge).bind("flash", &h.flash).bind("crosshair", &h.crosshair)
            .bind("markerShown", &h.markerShown).bind("markerX", &h.markerX).bind("markerY", &h.markerY).bind("markerR", &h.markerR)
            .bind("aimShown", &h.aimShown).bind("aimX", &h.aimX).bind("aimY", &h.aimY).bind("aimR", &h.aimR)
            .bind("fps", &h.fps).bind("showFps", &h.showFps);
        projv::ui::user::model(app, "menu")
            .bind("game", &m.game).bind("sensitivity", &m.sensitivity).bind("invertY", &m.invertY).bind("showFps", &m.showFps);
        projv::ui::user::open(app, "hud", (here / "ui/hud.rml").string());
        projv::ui::user::open(app, "menu", ui + "/menu.rml", {.shown = false, .modal = true, .fadeOut = 0.35f});
        app.events().on<projv::ui::UiAction>([&app](const projv::ui::UiAction& action) {
            if (action.name == "resume") setMenu(app, false);
            else if (action.name == "leave") app.closeRequested = true;
        });
    }

    void controls(projv::Application& app) {
        World& world = app.world;
        Client& c = world.ctx().get<Client>();
        const float dt = app.time().unscaledDelta;
        const bool joined = runtime::sessionStats(world).connected;
        const Entity me = own(world);
        const bool up = joined && alive(c) && me != NullEntity;
        const bool playing = up && c.state.phase == Playing;
        const bool carrier = up && c.state.players[c.state.you].carrier;
        const float speed = WALK_SPEED * (carrier ? CARRIER_SPEED : 1.0f);

        // A new game: the camera turned to look across the arena from where the player starts.
        if (c.haveState && c.state.mode != c.facedFor && c.state.phase == Intro && me != NullEntity) {
            c.facedFor = c.state.mode;
            const vec3 in = arenaCentre(c.state.mode) - drawnFeet(world, me);
            c.yaw = c.facing = std::atan2(in.z, in.x);
        }

        if (options.build && playing && cubes(c.state.mode)) {
            // Aim at a cell of its own pad (or the ground, in Sky Climb) and lay a cube there, as a first
            // person player does: the aim from its eye, the server's ray along it.
            static float clock = 0.0f;
            static int blocks = -1;
            if (blocks >= 0 && c.state.blocks < blocks) c.placed++;
            blocks = c.state.blocks;
            walk(world, c, vec3(0.0f), false, dt);
            if ((clock -= dt) > 0.0f) return;
            clock = 0.6f;
            const int own = ownPlot(c.state.mode, c.state.you);
            const vec3 middle = own >= 0 ? plotSpecs()[size_t(own)].centre : arenaCentre(c.state.mode);
            const vec3 target = middle + vec3(unit(c) * 1.5f, 0.0f, unit(c) * 1.5f);
            const vec3 head = drawnFeet(world, me) + vec3(0.0f, EYE_HEIGHT, 0.0f);
            const vec3 flat = target - head;
            c.yaw = c.facing = std::atan2(flat.z, flat.x);
            const vec3 d = target - (head + forwardOf(c.yaw) * EYE_AHEAD);
            c.pitch = std::atan2(d.y, glm::length(vec3(d.x, 0.0f, d.z)));
            projv::net::BitWriter w;
            w.varint(uint8_t(ToServer::Place));
            w.boolean(true);
            w.f32(c.yaw);
            w.f32(c.pitch);
            runtime::sendToServer(world, w.data(), true);
            return;
        }
        if (options.headless || options.wander) {
            // Wander toward the middle and about it, shoving now and then.
            if (!options.wander || !playing) return walk(world, c, vec3(0.0f), false, dt);
            const vec3 at = drawnFeet(world, me);
            static vec3 goal(0.0f);
            static float rethink = 0.0f, shoveClock = 0.0f;
            if ((rethink -= dt) <= 0.0f) {
                rethink = 1.5f;
                goal = arenaCentre(c.state.mode) + vec3(unit(c) * 4.0f, 0.0f, unit(c) * 4.0f);
            }
            vec3 to = goal - at;
            to.y = 0.0f;
            const vec3 move = glm::length(to) > 0.5f ? glm::normalize(to) * speed : vec3(0.0f);
            if (glm::length(move) > 0.1f) c.facing = std::atan2(move.z, move.x);
            world.patch<projv::Transform>(me, [&](projv::Transform& t) { t.rotation = party::facing(c.facing); });
            walk(world, c, move, false, dt);
            sendFacing(world, c.facing, false);
            if ((shoveClock -= dt) <= 0.0f) {
                shoveClock = 2.0f;
                send(world, ToServer::Shove);
                projv::net::BitWriter place;
                place.varint(uint8_t(ToServer::Place));
                place.boolean(false);
                place.f32(c.facing);
                place.f32(0.0f);
                runtime::sendToServer(world, place.data(), true);
                projv::net::BitWriter w;
                w.varint(uint8_t(ToServer::Throw));
                w.f32(0.5f);
                runtime::sendToServer(world, w.data(), true);
            }
            return;
        }

        auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
        const auto& input = world.ctx().get<projv::Input>();
        // The camera: the trackpad (the window holds the cursor; Esc lets it go, a click takes it back)
        // or the arrow keys. V: first person or third.
        // Esc: the menu (D23), which frees the cursor; it does not pause the game, which is the server's.
        Menu& menu = world.ctx().get<Menu>();
        if (input.pressed(projv::Key::Escape)) setMenu(app, !menu.open);
        if (menu.open) return;
        const bool captured = input.cursorCaptured;
        if (!captured && !input.wantCaptureMouse && input.pressed(projv::MouseButton::Left)) {
            projv::graphics::setCursorCaptured(app, renderInstance, true);
            return;   // that click was for the cursor
        }
        if (input.pressed(projv::Key::V)) c.firstPerson = !c.firstPerson;
        const float lowest = c.firstPerson ? -1.5f : -1.2f, highest = c.firstPerson ? 1.5f : 0.1f;
        const float turn = float(input.down(projv::Key::Right)) - float(input.down(projv::Key::Left));
        c.yaw = wrapAngle(c.yaw + turn * 2.2f * dt);
        c.pitch = std::clamp(c.pitch + (float(input.down(projv::Key::Up)) - float(input.down(projv::Key::Down))) * 1.2f * dt, lowest, highest);
        if (captured) {
            const float speed = 0.0025f * menu.sensitivity;
            c.yaw = wrapAngle(c.yaw + input.cursorDelta.x * speed);
            c.pitch = std::clamp(c.pitch - input.cursorDelta.y * speed * (menu.invertY ? -1.0f : 1.0f), lowest, highest);
        }

        const vec3 forward = forwardOf(c.yaw), right(-std::sin(c.yaw), 0.0f, std::cos(c.yaw));
        vec3 move(0.0f);
        if (input.down(projv::Key::W)) move += forward;
        if (input.down(projv::Key::S)) move -= forward;
        if (input.down(projv::Key::D)) move += right;
        if (input.down(projv::Key::A)) move -= right;
        if (playing && glm::length(move) > 0.0f) {
            move = glm::normalize(move) * speed;
            c.facing = std::atan2(move.z, move.x);
        } else {
            move = vec3(0.0f);
        }
        // In first person the player faces where it looks.
        if (c.firstPerson) c.facing = c.yaw;
        walk(world, c, move, playing && input.pressed(projv::Key::Space), dt);
        // Your player faces the way it walks; everyone else sees it from the server.
        if (up) world.patch<projv::Transform>(me, [&](projv::Transform& t) { t.rotation = party::facing(c.facing); });
        if (!joined) return;

        if ((c.lookClock -= dt) <= 0.0f) {
            c.lookClock = 1.0f / 20.0f;
            sendFacing(world, c.facing, false);
        }
        if (!playing) {
            c.charge = -1.0f;
            return;
        }
        // The facing first, reliably, so a shove or a throw goes the way it was meant.
        if (input.pressed(projv::Key::F) || input.pressed(projv::MouseButton::Left)) {
            sendFacing(world, c.facing, true);
            send(world, ToServer::Shove);
        }
        // C / X: a cube laid or taken away -- where the crosshair points, in first person.
        for (auto [key, kind] : {std::pair{projv::Key::C, ToServer::Place}, std::pair{projv::Key::X, ToServer::Break}}) {
            const bool click = kind == ToServer::Place && input.pressed(projv::MouseButton::Right);
            if (!input.pressed(key) && !click) continue;
            const bool aimed = c.firstPerson && cubes(c.state.mode);
            sendFacing(world, c.facing, true);
            projv::net::BitWriter w;
            w.varint(uint8_t(kind));
            w.boolean(aimed);
            w.f32(aimed ? c.yaw : c.facing);
            w.f32(c.pitch);
            runtime::sendToServer(world, w.data(), true);
        }
        // R: held to charge, thrown on letting go.
        const bool canThrow = c.state.toss == 0.0f;
        if (input.down(projv::Key::R) && canThrow) c.charge = std::max(c.charge, 0.0f) + dt / CHARGE_SECONDS;
        if (!input.down(projv::Key::R) && c.charge >= 0.0f) {
            sendFacing(world, c.facing, true);
            projv::net::BitWriter w;
            w.varint(uint8_t(ToServer::Throw));
            w.f32(std::min(c.charge, 1.0f));
            runtime::sendToServer(world, w.data(), true);
            c.charge = -1.0f;
        }
        if (!canThrow) c.charge = -1.0f;
    }

    // PostUpdate, once the physics has placed everything for this frame: where the camera is.
    void followCamera(projv::Application& app) {
        World& world = app.world;
        Client& c = world.ctx().get<Client>();
        const Entity me = own(world);
        if (!c.haveState) return;
        const vec3 centre = arenaCentre(c.state.mode);
        const vec3 at = drawnFeet(world, me);
        // Out, or between lives: circling the arena, looking at the middle.
        if (!alive(c) || me == NullEntity || at.y < FALLEN_BELOW) {
            c.marker.reset();
            c.aim.reset();
            const float angle = float(c.clock) * 0.15f;
            const float top = c.state.mode == Crumble ? 6.0f : 1.0f;
            const float out = c.state.mode == Dash ? 45.0f : 24.0f;
            c.eye = centre + vec3(std::cos(angle) * out, top + (c.state.mode == Dash ? 30.0f : 16.0f), std::sin(angle) * out);
            c.look = glm::normalize(centre + vec3(0.0f, top, 0.0f) - c.eye);
            return;
        }
        const vec3 head = at + vec3(0.0f, EYE_HEIGHT, 0.0f);
        const vec3 dir = lookDir(c.yaw, c.pitch);
        c.aim.reset();
        if (c.firstPerson) {
            // Through its eyes, just in front of its face; and, building, where a cube would go.
            c.eye = head + forwardOf(c.yaw) * EYE_AHEAD;
            c.look = dir;
            ivec3 cell;
            if (cubes(c.state.mode) && c.state.phase == Playing)
                if (const int plot = aimedCell(world, c.state.mode, me, c.eye, dir, true, cell); plot >= 0 && !plotSpecs()[size_t(plot)].locked)
                    c.aim = cellCentre(plot, cell);
        } else {
            // Behind and above, pulled in when something is in the way.
            vec3 back = -dir * 7.5f + vec3(0.0f, 0.6f, 0.0f);
            const float length = glm::length(back);
            if (auto hit = runtime::castRay(world, head, back / length, length, runtime::layerBit(runtime::PhysicsLayer::Static), me))
                back *= std::max(hit->distance - 0.3f, 0.6f) / length;
            c.eye = head + back;
            c.look = glm::normalize(head + vec3(0.0f, 0.3f, 0.0f) - c.eye);
        }
        // Charging a throw: where it would land.
        c.marker.reset();
        if (c.charge >= 0.0f) c.marker = landing(world, throwOrigin(at, c.facing), throwVelocity(c.facing, std::min(c.charge, 1.0f)));
    }

    // ---- Drawing ------------------------------------------------------------------------------------

    // The shared renderer (examples/shared/voxelRenderer), drawing to the window; the HUD is a
    // document over it (ui/hud.rml).
    void installRenderer(World& world, projv::graphics::RenderInstance& renderInstance, const fs::path& here) {
        voxel_renderer::View& view = world.ctx().emplace<voxel_renderer::View>();
        view.finestVoxel = 0.125f;
        view.hazeRadius = 200.0f;
        renderInstance.addRendererSpecification(1, voxel_renderer::specification(world, here));
        bgfx::ShaderHandle vertexShader = projv::graphics::loadEngineShader("vs_quad");
        renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    }

    std::string clockText(float seconds) {
        const int s = int(std::ceil(std::max(seconds, 0.0f)));
        char text[16];
        std::snprintf(text, sizeof text, "%d:%02d", s / 60, s % 60);
        return text;
    }

    // Each game's one rule, under the banner while it is introduced.
    const char* ruleOf(Mode m) {
        switch (m) {
            case Crumble: return "TILES DROP - KEEP MOVING";
            case Tag:     return "TOUCH SOMEONE TO PASS IT";
            case Dash:    return "RACE - C BRIDGES GAPS";
            case Climb:   return "BUILD UP TO THE GOLD";
            default:      return "COPY THE GOLD SHAPE";
        }
    }

    std::string hex(vec3 colour) {
        char text[8];
        std::snprintf(text, sizeof text, "#%02x%02x%02x", int(std::clamp(colour.x, 0.0f, 1.0f) * 255.0f + 0.5f),
                      int(std::clamp(colour.y, 0.0f, 1.0f) * 255.0f + 0.5f), int(std::clamp(colour.z, 0.0f, 1.0f) * 255.0f + 0.5f));
        return text;
    }

    // A ring of `radius` metres round a point in the world, facing the camera, as an element (D22):
    // its centre and radius in window pixels, or false behind the camera.
    bool project(const projv::CameraView& camera, vec3 point, float radius, projv::core::vec2 window, float& x, float& y, float& r) {
        bool inFront = false;
        const projv::core::vec2 uv = projv::graphics::cameraWorldToUV(camera, point, window, &inFront);
        if (!inFront) return false;
        const vec3 side = point + projv::graphics::cameraBasis(camera.forward).right * radius;
        const projv::core::vec2 edge = projv::graphics::cameraWorldToUV(camera, side, window);
        x = uv.x * window.x;
        y = uv.y * window.y;
        r = std::max(std::abs(edge.x - uv.x) * window.x, 3.0f);
        return true;
    }

    // What the HUD shows: everything hud.frag drew, as values for ui/hud.rml.
    void updateHud(World& world, projv::graphics::RenderInstance& renderInstance, const projv::CameraView& camera, float dt) {
        Client& c = world.ctx().get<Client>();
        Hud& h = world.ctx().get<Hud>();
        const State& s = c.state;
        const bool joined = runtime::sessionStats(world).connected && c.haveState;
        const projv::core::vec2 window = renderInstance.getWindowResolution();

        h.top.clear();
        if (joined) {
            h.top = std::string(modeName(s.mode)) + "   " + std::to_string(s.round) + "/" + std::to_string(s.rounds);
            if (s.phase == Playing) h.top += "   " + clockText(s.clock);
        }
        h.banner = c.bannerFor > 0.0f ? c.banner : "";
        h.bannerColour = hex(c.bannerColour);

        const bool up = joined && alive(c);
        h.hint.clear();
        if (!joined) h.hint = "Joining...";
        else if (s.phase == Intro) h.hint = std::string(ruleOf(s.mode)) + "   " + std::to_string(int(std::ceil(s.clock)));
        else if (s.phase == Playing && !up) h.hint = "Out - watching";
        else if (s.phase == Playing && s.you >= 0 && s.players[s.you].carrier) h.hint = "You have the bomb!";
        else if (s.phase == Playing && s.mode == Dash && s.you >= 0 && s.players[s.you].place) h.hint = "Home!";

        // The scoreboard: everyone by points; how far along (Sky Dash), how high (Sky Climb), how much of
        // the shape (Blueprint).
        int order[MAX_PLAYERS] = {0, 1, 2, 3};
        std::stable_sort(order, order + MAX_PLAYERS, [&](int a, int b) {
            if (s.players[a].present != s.players[b].present) return s.players[a].present;
            return s.players[a].points > s.players[b].points;
        });
        h.scores.clear();
        for (int line = 0; line < MAX_PLAYERS && joined; line++) {
            const State::Player& p = s.players[order[line]];
            if (!p.present) continue;
            static const char* places[] = {"", "1st", "2nd", "3rd", "4th"};
            std::string extra;
            if (s.mode == Dash && s.phase != Intro) extra = p.place ? places[p.place] : std::to_string(int(p.extra)) + " m";
            if (s.mode == Climb && s.phase != Intro) extra = std::to_string(int(p.extra)) + " m";
            if (s.mode == Blueprint && s.phase != Intro) extra = std::to_string(int(p.extra * 100.0f + 0.5f)) + "%";
            if (!p.alive && s.phase == Playing) extra = "out";
            h.scores.push_back({p.name, std::to_string(p.points), extra, hex(PLAYER_COLOURS[order[line]]), order[line] == s.you ? "1" : ""});
        }

        // The fuse (orange, red when low; pulsing when the bomb is yours), and the cooldowns.
        h.fuse = s.phase == Playing ? s.fuse : -1.0f;
        h.fuseMine = up && s.you >= 0 && s.players[s.you].carrier;
        h.fuseColour = hex(glm::mix(vec3(0.95f, 0.15f, 0.1f), vec3(1.0f, 0.65f, 0.15f), std::clamp(s.fuse * 2.0f, 0.0f, 1.0f)));
        h.alive = up;
        h.shove = s.shove;
        h.toss = s.toss;
        h.charge = c.charge >= 0.0f ? std::min(c.charge, 1.0f) : -1.0f;
        const int blocks = BLOCKS[s.mode] ? int(s.blocks) : -1;
        h.info = up && s.phase == Playing && blocks >= 0 ? "C  blocks: " + std::to_string(blocks) : "";
        h.flash = c.flash * 0.6f;

        // In the world: where a bomb would land (a 2.2 m ring), and in first person the crosshair and the
        // cell a cube would go in (0.55 m).
        h.markerShown = c.marker && project(camera, *c.marker, 2.2f, window, h.markerX, h.markerY, h.markerR);
        h.crosshair = c.firstPerson && up;
        h.aimShown = c.aim && project(camera, *c.aim, 0.55f, window, h.aimX, h.aimY, h.aimR);

        // The frame rate, when the menu asks for it.
        static float timer = 0.0f;
        static int frames = 0;
        timer += dt;
        frames++;
        if (timer >= 0.5f) {
            h.fps = int(std::lround(frames / timer));
            timer = 0.0f;
            frames = 0;
        }
        h.showFps = world.ctx().get<Menu>().showFps;
    }

    void render(projv::Application& app) {
        if (options.headless) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return;
        }
        World& world = app.world;
        auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
        auto& scene = world.ctx().get<projv::Scene>();
        auto& gpu = world.ctx().get<projv::GPUData>();
        Client& c = world.ctx().get<Client>();
        projv::graphics::flushSceneUpdates(scene, gpu);
        // A blast near you shakes the picture.
        vec3 eye = c.eye;
        if (c.shake > 0.0f) {
            const float t = float(c.clock), amount = c.shake * c.shake;
            eye += amount * 0.25f * vec3(std::sin(t * 37.0f), std::sin(t * 41.0f + 1.0f), std::sin(t * 53.0f + 2.0f));
        }
        voxel_renderer::upload(world, renderInstance, eye, c.look);
        projv::CameraView camera;
        camera.position = eye;
        camera.forward = c.look;
        updateHud(world, renderInstance, camera, app.time().unscaledDelta);
        projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpu);

        c.titleClock -= app.time().unscaledDelta;
        if (c.titleClock <= 0.0f) {
            c.titleClock = 0.5f;
            std::string title = "ProjectV Party";
            if (c.haveState) title += std::string("  |  ") + modeName(c.state.mode) + "  |  round " + std::to_string(c.state.round);
            if (hosted) title += "  |  hosting on port " + std::to_string(options.port);
            glfwSetWindowTitle(renderInstance.window, title.c_str());
        }
        if (renderInstance.shouldClose) app.closeRequested = true;

        // PARTY_CAPTURE=<file>: frame PARTY_CAPTURE_AT (default 900) saved as <file>.tga, then it exits;
        // PARTY_CAPTURE_MENU=1 with the menu open. An unattended look at the HUD.
        if (const char* capture = std::getenv("PARTY_CAPTURE")) {
            static const int at = std::getenv("PARTY_CAPTURE_AT") ? std::atoi(std::getenv("PARTY_CAPTURE_AT")) : 900;
            const char* menu = std::getenv("PARTY_CAPTURE_MENU");
            if (menu && std::string(menu) == "1" && app.frameCount == at - 60) setMenu(app, true);
            if (app.frameCount == at) bgfx::requestScreenShot(BGFX_INVALID_HANDLE, capture);
            if (app.frameCount == at + 5) app.closeRequested = true;
        }
    }

    // Headless: finish on time, after checking.
    void watch(projv::Application& app) {
        if (options.seconds <= 0.0) return;
        Client& c = app.world.ctx().get<Client>();
        static double clock = 0.0;
        clock += app.time().unscaledDelta;
        if (clock < options.seconds) return;
        const runtime::SessionStats stats = runtime::sessionStats(app.world);
        const int you = c.state.you;
        std::printf("PARTYTEST: %s: %s, player %d, %d states, %d banners, %d rounds seen, %d knocks, %d cubes laid, points %d, "
                    "world changes %llu, mismatches %llu\n",
                    options.name.c_str(), stats.connected ? "joined" : "not joined", you, c.states, c.banners, c.rounds, c.knocks, c.placed,
                    you >= 0 ? c.state.players[you].points : -1, (unsigned long long)stats.worldChanges,
                    (unsigned long long)stats.worldMismatches);
        const bool ok = stats.connected && you >= 0 && c.states > 10 && c.banners > 0 && c.rounds >= options.expectRounds &&
                        stats.worldMismatches == 0 && (!options.build || c.placed > 0);
        std::printf("PARTYTEST: %s: %s\n", options.name.c_str(), ok ? "PASS" : "FAIL");
        std::fflush(stdout);
        exitCode = ok ? 0 : 1;
        app.closeRequested = true;
    }

    void startup(projv::Application& app) {
        World& world = app.world;
        const fs::path here = projv::core::executableDirectory();
        const std::string folder = (here / FOLDER).string();

        // Hosting: the server runs here, in an Application of its own, a frame for each of ours.
        if (options.host.empty()) {
            hosted = std::make_unique<projv::Application>();
            ServerOptions server;
            server.port = options.port;
            server.players = options.players;
            server.rounds = options.rounds;
            server.only = options.only;
            server.folder = folder;
            if (!party::installServer(*hosted, server)) {
                projv::core::error("party: could not host on port {}; is a party already running here? (--host 127.0.0.1 joins it)",
                                   options.port);
                std::exit(1);
            }
            hosted->runStartup();
            projv::core::info("party: hosting on port {}: others join with --host <this machine's address>", options.port);
        }

        if (!options.headless) {
            auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
            renderInstance.initialize(1600, 900, "ProjectV Party");
            projv::graphics::installPlatform(app, renderInstance);
            projv::ui::installUi(app, renderInstance);   // the dev layer: F1
            // The trackpad turns the camera: the window holds the cursor (Esc lets it go).
            projv::graphics::setCursorCaptured(app, renderInstance, true);
        }
        auto& scene = world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder));
        if (scene.components.empty()) {
            projv::core::error("party: no arenas in '{}'", folder);
            std::exit(1);
        }
        // Physics here too: this client walks its own player at once, applies the server's tile drops,
        // and throws its own chips about.
        runtime::installSceneBridge(app);
        runtime::installPhysics(app);
        runtime::loadPhysicsMaterials(world, (fs::path(folder) / MATERIALS).string());
        for (const char* group : {"player", "floor", "falling"}) runtime::definePhysicsGroup(world, group);
        runtime::spawnEntities(world);
        Client& c = world.ctx().emplace<Client>();
        if (!options.headless)
            c.fireball = prototype(scene, "fireball", 8, 0.25f, 12.0f, [](int x, int y, int z) -> uint32_t {
                const float r = glm::length(vec3(x, y, z) + 0.5f - 4.0f);
                return r > 4.0f ? 0u : r < 2.5f ? rgb(1.0f, 0.9f, 0.4f) : rgb(1.0f, 0.45f, 0.1f);
            });

        runtime::ClientConfig config;
        config.host = options.host.empty() ? "127.0.0.1" : options.host;
        config.port = options.port;
        config.sceneFolder = folder;
        config.name = options.name;
        config.onMessage = [](World& w, std::span<const uint8_t> message) { heard(w, message); };
        if (!runtime::installClient(app, projv::net::makeEnetTransport(), config)) std::exit(1);
        app.events().on<runtime::LeftServer>([&app](const runtime::LeftServer& e) {
            projv::core::warn("party: left the party: {}", e.reason);
            std::printf("PARTYTEST: %s: left the party: %s\n", options.name.c_str(), e.reason.c_str());
            exitCode = 1;
            app.closeRequested = true;
        });

        if (options.headless) return;
        installRenderer(world, world.ctx().get<projv::graphics::RenderInstance>(), here);
        world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));
        installHud(app, here);
    }

    void shutdown(projv::Application& app) {
        if (auto* gpu = app.world.ctx().find<projv::GPUData>()) projv::graphics::destroyGPUData(*gpu);
        if (!options.headless) {
            // Stopped here rather than at exit: bgfx's render thread still presenting while the driver's
            // memory is freed under it has crashed drivers at exit (Voxel Mayhem).
            bgfx::shutdown();
            glfwTerminate();
        }
        hosted.reset();
    }
}

int main(int argc, char** argv) {
    projv::ui::setCommandLine(argc, argv);
    for (int i = 1; i < argc; i++) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--host")) options.host = next();
        else if (!std::strcmp(argv[i], "--port")) options.port = uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--name")) options.name = next();
        else if (!std::strcmp(argv[i], "--players")) options.players = std::clamp(std::atoi(next()), 1, MAX_PLAYERS);
        else if (!std::strcmp(argv[i], "--rounds")) options.rounds = std::clamp(std::atoi(next()), 1, 99);
        else if (!std::strcmp(argv[i], "--mode")) {
            const std::string mode = next();
            options.only = mode == "crumble" ? Crumble : mode == "tag" ? Tag : mode == "dash" ? Dash : mode == "climb" ? Climb : mode == "blueprint" ? Blueprint : -1;
        } else if (!std::strcmp(argv[i], "--headless")) options.headless = true;
        else if (!std::strcmp(argv[i], "--seconds")) options.seconds = std::atof(next());
        else if (!std::strcmp(argv[i], "--wander")) options.wander = true;
        else if (!std::strcmp(argv[i], "--build")) options.build = true;
        else if (!std::strcmp(argv[i], "--expect-rounds")) options.expectRounds = std::atoi(next());
    }
    for (char& ch : options.name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    options.name = options.name.substr(0, 10);

    projv::Application app;
    app.addSystem(projv::Stage::Startup,    "startup",  startup);
    app.addSystem(projv::Stage::PreUpdate,  "host",     [](projv::Application&) { if (hosted) hosted->runFrame(); });
    app.addSystem(projv::Stage::Update,     "controls", controls);
    app.addSystem(projv::Stage::Update,     "effects",  effects);
    app.addSystem(projv::Stage::Update,     "watch",    watch);
    app.addSystem(projv::Stage::PostUpdate, "camera",   followCamera, {.after = "physics: present"});
    app.addSystem(projv::Stage::Render,     "render",   render);
    app.addSystem(projv::Stage::Shutdown,   "shutdown", shutdown);
    app.run();
    return exitCode;
}
