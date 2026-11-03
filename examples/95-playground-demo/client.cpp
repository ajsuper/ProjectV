// ProjectV Playground: a client
//
// A field of things to break, for up to four friends: a castle, brick chimneys, a timber house, a
// glass greenhouse, a stone bridge, trees, crates and TNT. No rounds and no score -- eight tools and
// whatever you do with them:
//   1 CANNON  an iron ball where you aim       5 BALL   a big rubber ball, thrown
//   2 BLAST   an explosion where you aim       6 BUILD  a metre of stone against what you aim at
//   3 TNT     a barrel thrown; it goes off     7 DIG    takes out what you aim at; what it held falls
//             when jolted, or after a while    8 PUSH   throws what is in front of you, players too
//   4 CRATE   a crate, thrown
//
//   ./playground_demo                        host a playground here (others can join it) and play
//   ./playground_demo --host 10.0.0.5        join someone else's, or a playground_demo_server
//   ./playground_demo --name ann --port 27100
//   ./playground_demo --headless --seconds 40 --play   no window: for the self-test (selftest.sh)
//       --play                walk to the castle and use every tool on it in turn
//       --gravity             ask for the other gravity once, and --reset to put the world back once
//
// Controls, made for a laptop: W/A/S/D walk, Space jumps, the trackpad or the arrow keys look (Esc
// frees the cursor, a click takes it back). A click or F uses the tool in hand -- held, it goes on;
// 1-8, the wheel or Q/E pick the tool. V switches between first and third person, G between the
// Earth's gravity and the Moon's (for everyone), H shows the help; Backspace held puts the world back.
//
// The server decides everything (game.h); this sends what you do and shows what it says. Your own
// player walks here at once (prediction); everyone and everything else is drawn where the server has
// it, and every break arrives as the server made it.

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

using namespace playground;
using projv::core::vec4;
namespace fs = std::filesystem;

namespace {
    struct Options {
        std::string host;            // empty: host here
        uint16_t    port = PORT;
        std::string name = "PLAYER";
        bool        headless = false;
        double      seconds = 0.0;
        bool        play = false;
        bool        gravity = false, reset = false;
    };
    Options options;
    int exitCode = 0;
    std::unique_ptr<projv::Application> hosted;   // the playground this client hosts, if it does

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
        float  yaw = 1.5707963f, pitch = -0.1f;   // the camera's: toward the castle
        float  facing = 1.5707963f;              // where the player faces
        vec3   eye{0.0f, 6.0f, -8.0f}, look{0.0f, -0.2f, 1.0f};
        vec3   knock{0.0f};                       // a blast or a push still carrying this player
        bool   hop = false;
        Tool   tool = Cannon;
        float  useClock = 0.0f;                   // till a held click uses the tool again
        float  resetHeld = -1.0f;                 // how long Backspace has been held; < 0 not
        bool   moon = false;                      // the gravity this client's physics has
        float  lookClock = 0.0f, titleClock = 0.0f, flash = 0.0f, shake = 0.0f;
        float  helpFor = 20.0f;                   // the help shows this long at the start; H toggles it
        std::string banner;
        vec3   bannerColour{1.0f};
        float  bannerFor = 0.0f;
        bool   firstPerson = false;
        std::optional<vec3> ring;                 // where the tool would take effect
        Proto  fireball;
        std::vector<Flash> flashes;
        std::vector<Burst> bursts;
        std::mt19937 random{7};
        double clock = 0.0;
        // What a headless client saw, for its checks.
        int states = 0, banners = 0, knocks = 0, uses = 0;
    };

    // What the HUD document shows (ui/hud.rml), bound to its model "hud" and filled each frame.
    struct Hud {
        std::string top, banner, bannerColour = "#ffffff", toolName;
        std::vector<std::vector<std::string>> players;   // name, colour, "1" for you
        std::vector<std::vector<std::string>> tools;     // number, name, readiness 0..1, "1" in hand
        std::vector<std::string> help;
        float reset = -1.0f, flash = 0.0f;
        bool  joined = false, ringShown = false;
        float ringX = 0, ringY = 0, ringR = 0;
        int   fps = 0;
        bool  showFps = false;
    };
    // The shared menu's settings (examples/shared/ui/menu.rml), bound to its model "menu".
    struct Menu {
        std::string game = "Playground";
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

    void sendFacing(World& world, float facing, bool reliable) {
        projv::net::BitWriter w;
        w.varint(uint8_t(ToServer::Look));
        w.f32(facing);
        runtime::sendToServer(world, w.data(), reliable);
    }

    void sendUse(World& world, Tool tool, vec3 eye, vec3 dir) {
        projv::net::BitWriter w;
        w.varint(uint8_t(ToServer::Use));
        w.varint(tool);
        writeVec(w, eye);
        writeVec(w, dir);
        runtime::sendToServer(world, w.data(), true);
        world.ctx().get<Client>().uses++;
    }

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
            // The gravity is everyone's: this client's physics walks its own player under it too.
            if (state.moon != c.moon) {
                c.moon = state.moon;
                runtime::setGravity(world, vec3(0.0f, -(c.moon ? MOON_GRAVITY : GRAVITY), 0.0f));
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
            const auto what = BoomKind(r.varint());
            const float size = r.f32();
            if (r.failed || !finite(at) || !std::isfinite(size)) return;
            switch (what) {
                case BoomKind::Blast:
                    c.flashes.push_back({copyOf(world, c.fireball, at, 0.6f), 0.0f, size * 0.6f});
                    burst(world, at, {rgb(1.0f, 0.6f, 0.15f), rgb(0.95f, 0.85f, 0.3f), rgb(0.3f, 0.3f, 0.3f)}, 30, 9.0f);
                    c.shake = std::max(c.shake, 1.0f - std::min(glm::distance(at, c.eye) / 40.0f, 1.0f));
                    break;
                case BoomKind::Muzzle:
                    burst(world, at, {rgb(1.0f, 0.85f, 0.4f), rgb(0.6f, 0.6f, 0.6f)}, 6, 4.0f, 0.08f);
                    break;
                case BoomKind::Dust:
                    burst(world, at, {rgb(0.55f, 0.5f, 0.42f), rgb(0.4f, 0.37f, 0.33f)}, 10, 3.0f, 0.1f);
                    break;
                default:
                    burst(world, at, {rgb(0.95f, 0.95f, 0.9f), rgb(0.8f, 0.85f, 0.9f)}, 8, 3.0f, 0.08f);
                    break;
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
        c.helpFor -= dt;
        if (options.headless) return;
        // Fireballs grow and are gone in half a second.
        for (Flash& f : c.flashes) {
            f.age += dt;
            if (!world.valid(f.e)) continue;
            if (f.age > 0.45f) { world.destroy(f.e); continue; }
            world.patch<projv::Transform>(f.e, [&](projv::Transform& t) { t.scale = f.size * (0.4f + f.age * 3.5f); });
        }
        std::erase_if(c.flashes, [&](const Flash& f) { return f.age > 0.45f || !world.valid(f.e); });

        // Chips: made for each burst, gone after two seconds, never more than MAX_CHIPS. This
        // client's alone: the server never hears of them.
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
                world.emplace<projv::CollisionGroups>(e, projv::CollisionGroups{{"small debris"}, {"small debris", "player"}, 0});
                world.emplace<Chip>(e, Chip{c.clock});
                runtime::setVelocity(world, e, glm::normalize(spread + vec3(0, 1.2f, 0)) * (b.speed * (0.6f + 0.6f * std::abs(unit(c)))),
                                     spread * 8.0f);
            }
        }
        c.bursts.clear();
    }

    // ---- Playing --------------------------------------------------------------------------------------

    // The walking sent this step: `move`, carried on by any knock, with a hop when knocked.
    void walk(World& world, Client& c, vec3 move, bool jump, float dt) {
        const float left = glm::length(c.knock);
        if (left > 0.0f) c.knock *= std::max(0.0f, 1.0f - KNOCK_DECAY * dt / left);
        runtime::setLocalInput(world, move + c.knock, jump || c.hop);
        c.hop = false;
    }

    // The self-test's player: to the castle's gate, then every tool in turn at its south wall, and
    // the gravity and the reset once each, when asked.
    void play(World& world, Client& c, float dt) {
        const Entity me = own(world);
        if (me == NullEntity || !c.haveState) return walk(world, c, vec3(0.0f), false, dt);
        static float clock = 0.0f, since = 0.0f;
        static int next = 0;
        static bool gravityAsked = false, resetAsked = false;
        since += dt;
        const vec3 at = drawnFeet(world, me);
        const vec3 stand(float(c.state.you) * 2.0f - 3.0f, 0.0f, 21.0f);
        vec3 to = stand - at;
        to.y = 0.0f;
        const vec3 move = glm::length(to) > 0.6f ? glm::normalize(to) * WALK_SPEED : vec3(0.0f);
        if (glm::length(move) > 0.1f) c.facing = std::atan2(move.z, move.x);
        world.patch<projv::Transform>(me, [&](projv::Transform& t) { t.rotation = playground::facing(c.facing); });
        walk(world, c, move, false, dt);
        if (options.gravity && !gravityAsked && since > 8.0f) {
            gravityAsked = true;
            send(world, ToServer::Gravity);
        }
        if (options.reset && !resetAsked && since > options.seconds * 0.6) {
            resetAsked = true;
            send(world, ToServer::Reset);
        }
        if ((clock -= dt) > 0.0f || glm::length(to) > 3.0f) return;
        clock = 0.45f;
        // At the wall beside the gate, a little different each time.
        const Tool tool = Tool(next++ % TOOLS);
        const vec3 head = at + vec3(0.0f, EYE_HEIGHT, 0.0f);
        const vec3 target(unit(c) * 6.0f + (std::abs(unit(c)) < 0.3f ? 4.0f : 0.0f), 1.0f + std::abs(unit(c)) * 3.0f, castleCentre().z - CASTLE_HALF);
        const vec3 dir = glm::normalize(target - head);
        c.facing = std::atan2(dir.z, dir.x);
        sendFacing(world, c.facing, true);
        sendUse(world, tool, head, dir);
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
            .bind("top", &h.top).bind("banner", &h.banner).bind("bannerColour", &h.bannerColour).bind("toolName", &h.toolName)
            .bind("players", &h.players).bind("tools", &h.tools).bind("help", &h.help).bind("reset", &h.reset)
            .bind("flash", &h.flash).bind("joined", &h.joined).bind("ringShown", &h.ringShown).bind("ringX", &h.ringX)
            .bind("ringY", &h.ringY).bind("ringR", &h.ringR).bind("fps", &h.fps).bind("showFps", &h.showFps);
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
        const bool up = joined && c.haveState && me != NullEntity;

        if (options.headless) {
            if (options.play && joined) return play(world, c, dt);
            return walk(world, c, vec3(0.0f), false, dt);
        }

        auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
        const auto& input = world.ctx().get<projv::Input>();
        // The camera: the trackpad (the window holds the cursor; Esc lets it go, a click takes it back)
        // or the arrow keys.
        // Esc: the menu (D23), which frees the cursor; it does not pause the game, which is the server's.
        Menu& menu = world.ctx().get<Menu>();
        if (input.pressed(projv::Key::Escape)) setMenu(app, !menu.open);
        if (menu.open) return;
        const bool captured = input.cursorCaptured;
        if (!captured && !input.wantCaptureMouse && input.pressed(projv::MouseButton::Left)) {
            projv::graphics::setCursorCaptured(app, renderInstance, true);
            c.useClock = 0.3f;   // that click was for the cursor
        }
        if (input.pressed(projv::Key::V)) c.firstPerson = !c.firstPerson;
        if (input.pressed(projv::Key::H)) c.helpFor = c.helpFor > 0.0f ? 0.0f : 1e9f;
        const float turn = float(input.down(projv::Key::Right)) - float(input.down(projv::Key::Left));
        c.yaw = wrapAngle(c.yaw + turn * 2.2f * dt);
        c.pitch = std::clamp(c.pitch + (float(input.down(projv::Key::Up)) - float(input.down(projv::Key::Down))) * 1.2f * dt, -1.5f, 1.45f);
        if (captured) {
            const float speed = 0.0025f * menu.sensitivity;
            c.yaw = wrapAngle(c.yaw + input.cursorDelta.x * speed);
            c.pitch = std::clamp(c.pitch - input.cursorDelta.y * speed * (menu.invertY ? -1.0f : 1.0f), -1.5f, 1.45f);
        }

        // The tool: 1-8, the wheel, Q and E.
        const projv::Key digits[TOOLS] = {projv::Key::Num1, projv::Key::Num2, projv::Key::Num3, projv::Key::Num4,
                                          projv::Key::Num5, projv::Key::Num6, projv::Key::Num7, projv::Key::Num8};
        for (int t = 0; t < TOOLS; t++) if (input.pressed(digits[t])) c.tool = Tool(t);
        int step = input.pressed(projv::Key::E) - input.pressed(projv::Key::Q);
        if (input.scroll.y != 0.0f) step += input.scroll.y < 0.0f ? 1 : -1;
        if (step) c.tool = Tool((int(c.tool) + step + TOOLS) % TOOLS);

        const vec3 forward = forwardOf(c.yaw), right(-std::sin(c.yaw), 0.0f, std::cos(c.yaw));
        vec3 move(0.0f);
        if (input.down(projv::Key::W)) move += forward;
        if (input.down(projv::Key::S)) move -= forward;
        if (input.down(projv::Key::D)) move += right;
        if (input.down(projv::Key::A)) move -= right;
        if (up && glm::length(move) > 0.0f) {
            move = glm::normalize(move) * WALK_SPEED;
            c.facing = std::atan2(move.z, move.x);
        } else {
            move = vec3(0.0f);
        }
        // In first person the player faces where it looks.
        if (c.firstPerson) c.facing = c.yaw;
        walk(world, c, move, up && input.pressed(projv::Key::Space), dt);
        if (up) world.patch<projv::Transform>(me, [&](projv::Transform& t) { t.rotation = playground::facing(c.facing); });
        if (!joined) return;

        if ((c.lookClock -= dt) <= 0.0f) {
            c.lookClock = 1.0f / 20.0f;
            sendFacing(world, c.facing, false);
        }
        if (input.pressed(projv::Key::G)) send(world, ToServer::Gravity);
        // Backspace, held: the world put back.
        if (input.down(projv::Key::Backspace)) {
            c.resetHeld = std::max(c.resetHeld, 0.0f) + dt;
            if (c.resetHeld >= RESET_HOLD) {
                send(world, ToServer::Reset);
                c.resetHeld = -1e9f;   // once per hold
            }
        } else {
            c.resetHeld = -1.0f;
        }

        // A click or F: the tool, along the camera's ray. Held, again as soon as it is ready.
        c.useClock -= dt;
        const bool using_ = captured && (input.down(projv::MouseButton::Left) || input.down(projv::Key::F));
        if (up && using_ && c.useClock <= 0.0f) {
            c.useClock = COOLDOWN[c.tool] + 0.02f;
            // Third person: the player turns to face where the tool goes.
            if (!c.firstPerson) c.facing = c.yaw;
            sendFacing(world, c.facing, true);
            sendUse(world, c.tool, c.eye, lookDir(c.yaw, c.pitch));
        }
    }

    // PostUpdate, once the physics has placed everything for this frame: where the camera is, and
    // what the crosshair is on.
    void followCamera(projv::Application& app) {
        World& world = app.world;
        Client& c = world.ctx().get<Client>();
        const Entity me = own(world);
        c.ring.reset();
        if (me == NullEntity) return;
        const vec3 head = drawnFeet(world, me) + vec3(0.0f, EYE_HEIGHT, 0.0f);
        const vec3 dir = lookDir(c.yaw, c.pitch);
        if (c.firstPerson) {
            c.eye = head + forwardOf(c.yaw) * 0.35f;
        } else {
            // Behind and over the right shoulder, looking along the aim: the crosshair is the middle
            // of the screen. Pulled in when something is in the way.
            const vec3 right(-std::sin(c.yaw), 0.0f, std::cos(c.yaw));
            vec3 back = -dir * 5.5f + right * 0.9f + vec3(0.0f, 0.5f, 0.0f);
            const float length = glm::length(back);
            if (auto hit = runtime::castRay(world, head, back / length, length, solidLayers(), me))
                back *= std::max(hit->distance - 0.3f, 0.4f) / length;
            c.eye = head + back;
        }
        c.look = dir;
        if (RING[c.tool] <= 0.0f) return;
        auto hit = runtime::castRay(world, c.eye, dir, REACH[c.tool] + glm::distance(c.eye, head), solidLayers(), me);
        if (!hit || glm::distance(hit->position, head) > REACH[c.tool]) return;
        c.ring = c.tool == Build ? glm::floor(hit->position + hit->normal * 0.5f) + vec3(0.5f) : hit->position;
    }

    // ---- Drawing ------------------------------------------------------------------------------------

    // The shared renderer (examples/shared/voxelRenderer), drawing to the window; the HUD is a
    // document over it (ui/hud.rml).
    void installRenderer(World& world, projv::graphics::RenderInstance& renderInstance, const fs::path& here) {
        voxel_renderer::View& view = world.ctx().emplace<voxel_renderer::View>();
        view.finestVoxel = 0.1f;
        view.hazeRadius = 180.0f;
        renderInstance.addRendererSpecification(1, voxel_renderer::specification(world, here));
        bgfx::ShaderHandle vertexShader = projv::graphics::loadEngineShader("vs_quad");
        renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    }

    const char* HELP[6] = {"W A S D  move,  Space  jump", "Click or F  use the tool", "1-8, the wheel, Q E  the tool",
                           "V  camera,  G  gravity", "Hold Backspace  rebuild", "H  this help,  Esc  the menu"};

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

        h.joined = joined;
        h.top = joined ? std::string(s.moon ? "Moon gravity" : "Playground") + "   pieces: " + std::to_string(s.pieces) : "Joining...";
        h.banner = c.bannerFor > 0.0f ? c.banner : "";
        h.bannerColour = hex(c.bannerColour);

        h.players.clear();
        for (int slot = 0; slot < MAX_PLAYERS && joined; slot++)
            if (s.players[slot].present) h.players.push_back({s.players[slot].name, hex(PLAYER_COLOURS[slot]), slot == s.you ? "1" : ""});

        // The tools: each one's number and readiness, the one in hand marked; the reset being held.
        h.tools.clear();
        for (int t = 0; t < TOOLS; t++) {
            char ready[16];
            std::snprintf(ready, sizeof ready, "%.3f", 1.0f - std::clamp(s.cooldown[t], 0.0f, 1.0f));
            h.tools.push_back({std::to_string(t + 1), toolName(Tool(t)), ready, t == int(c.tool) ? "1" : ""});
        }
        h.toolName = joined ? toolName(c.tool) : "";
        h.reset = c.resetHeld >= 0.0f ? std::min(c.resetHeld / RESET_HOLD, 1.0f) : -1.0f;
        h.flash = c.flash * 0.6f;
        h.help.clear();
        if (c.helpFor > 0.0f) h.help.assign(std::begin(HELP), std::end(HELP));

        // Where the tool takes effect: a ring of its reach, in the world.
        h.ringShown = c.ring && RING[c.tool] > 0.0f && project(camera, *c.ring, RING[c.tool], window, h.ringX, h.ringY, h.ringR);

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
            std::string title = "ProjectV Playground";
            if (hosted) title += "  |  hosting on port " + std::to_string(options.port);
            glfwSetWindowTitle(renderInstance.window, title.c_str());
        }
        if (renderInstance.shouldClose) app.closeRequested = true;

        // PLAY_CAPTURE=<file>: frame PLAY_CAPTURE_AT (default 900) saved as <file>.tga, then it exits;
        // PLAY_CAPTURE_MENU=1 with the menu open. An unattended look at the HUD.
        if (const char* capture = std::getenv("PLAY_CAPTURE")) {
            static const int at = std::getenv("PLAY_CAPTURE_AT") ? std::atoi(std::getenv("PLAY_CAPTURE_AT")) : 900;
            const char* menu = std::getenv("PLAY_CAPTURE_MENU");
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
        std::printf("PLAYTEST: %s: %s, player %d, %d states, %d banners, %d knocks, %d uses, world changes %llu, mismatches %llu, "
                    "reconciliations %llu\n",
                    options.name.c_str(), stats.connected ? "joined" : "not joined", c.state.you, c.states, c.banners, c.knocks, c.uses,
                    (unsigned long long)stats.worldChanges, (unsigned long long)stats.worldMismatches,
                    (unsigned long long)stats.reconciliations);
        const bool ok = stats.connected && c.state.you >= 0 && c.states > 10 && stats.worldMismatches == 0 &&
                        (!options.play || (c.uses > 8 && stats.worldChanges > 0));
        std::printf("PLAYTEST: %s: %s\n", options.name.c_str(), ok ? "PASS" : "FAIL");
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
            server.folder = folder;
            if (!playground::installServer(*hosted, server)) {
                projv::core::error("playground: could not host on port {}; is one already running here? (--host 127.0.0.1 joins it)",
                                   options.port);
                std::exit(1);
            }
            hosted->runStartup();
            projv::core::info("playground: hosting on port {}: others join with --host <this machine's address>", options.port);
        }

        if (!options.headless) {
            auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
            renderInstance.initialize(1600, 900, "ProjectV Playground");
            projv::graphics::installPlatform(app, renderInstance);
            projv::ui::installUi(app, renderInstance);   // the dev layer: F1
            projv::graphics::setCursorCaptured(app, renderInstance, true);
        }
        auto& scene = world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder));
        if (scene.components.empty()) {
            projv::core::error("playground: no field in '{}'", folder);
            std::exit(1);
        }
        // Physics here too: this client walks its own player at once, applies the server's breaks,
        // and throws its own chips about. It breaks nothing itself: that is the server's.
        runtime::installSceneBridge(app);
        runtime::installPhysics(app);
        runtime::loadPhysicsMaterials(world, (fs::path(folder) / MATERIALS).string());
        for (const char* group : {"small debris", "player"}) runtime::definePhysicsGroup(world, group);
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
            projv::core::warn("playground: left: {}", e.reason);
            std::printf("PLAYTEST: %s: left the playground: %s\n", options.name.c_str(), e.reason.c_str());
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
        else if (!std::strcmp(argv[i], "--headless")) options.headless = true;
        else if (!std::strcmp(argv[i], "--seconds")) options.seconds = std::atof(next());
        else if (!std::strcmp(argv[i], "--play")) options.play = true;
        else if (!std::strcmp(argv[i], "--gravity")) options.gravity = true;
        else if (!std::strcmp(argv[i], "--reset")) options.reset = true;
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
