// ProjectV Siege: a client
//
// Two fortresses, a team in each, a flag on each keep. Knock the other team's flag to the ground and
// they stop coming back; then finish them. Rounds last three minutes.
//
//   ./siege_client                         host a game here (others can join it) and play in it
//   ./siege_client --host 10.0.0.5         join someone else's game, or a siege_server
//   ./siege_client --name ann --port 27100
//   ./siege_client --team-size 6 --round-seconds 120    when hosting: bots fill each team up to six
//   ./siege_client --headless --seconds 40 ...          no window: for the self-test (selftest.sh)
//       --walk                walk toward the enemy and shoot
//       --expect-rounds 2     see at least this many rounds begin
//
// Controls: W/A/S/D walk, Shift run, Space jump, the mouse looks (Esc frees it, a click takes it
// back). 1 bullets, 2 blocks, 3 blast; left click uses what you hold. E mans a cannon you stand by;
// on it the mouse aims, a click fires, and E again leaves it.
//
// The server decides everything (game.h); this sends what you do and shows what it says. Your own
// soldier walks here at once (prediction); everyone and everything else is drawn where the server has
// it. Shots, blasts and the chips that fly are drawn here only, from what the server tells.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "core/application.h"
#include "core/log.h"
#include "core/paths.h"
#include "game.h"
#include "graphics/disk_io.h"
#include "graphics/gpu_interface.h"
#include "graphics/input.h"
#include "graphics/manage_resources.h"
#include "graphics/perform_renderer.h"
#include "graphics/render_instance.h"
#include "siege.h"
#include "utils/material.h"
#include "voxel_renderer.h"

using namespace siege;
using projv::core::vec4;
namespace fs = std::filesystem;

namespace {
    struct Options {
        std::string host;            // empty: host here
        uint16_t    port = PORT;
        std::string name = "PLAYER";
        int         teamSize = TEAM_SIZE;
        float       roundSeconds = ROUND_SECONDS;
        bool        headless = false;
        double      seconds = 0.0;
        bool        walk = false;
        bool        gunner = false;       // climb to the left cannon, man it, fire at the enemy keep
        int         expectRounds = 0;
        std::string capture;              // a screenshot every few seconds, named with this
    };
    Options options;
    int exitCode = 0;
    std::unique_ptr<projv::Application> hosted;   // the game this client hosts, if it does

    struct Proto {
        projv::ComponentHandle node = projv::INVALID_COMPONENT_HANDLE;
    };
    struct Chip { double at; };
    struct Tracer { Entity e; vec3 position, velocity; float age = 0.0f; };
    struct Flash { Entity e; float age = 0.0f, size = 1.0f; };
    struct Burst { vec3 centre; float size; std::vector<uint32_t> colours; int count; float speed; };
    constexpr size_t MAX_CHIPS = 160;

    struct Client {
        State  state;
        bool   haveState = false, facedHome = false;
        float  yaw = 0.0f, pitch = -0.1f;
        vec3   eye{0.0f, 25.0f, -70.0f}, look{0.0f, -0.3f, 1.0f};   // the camera
        float  aimClock = 0.0f, titleClock = 0.0f;
        float  useClock[3] = {0, 0, 0};
        std::string banner;
        vec3   bannerColour{1.0f};
        float  bannerFor = 0.0f, hurt = 0.0f, shake = 0.0f;
        std::optional<vec3> marker;
        Proto  fireball, tracer;
        std::vector<Tracer> tracers;
        std::vector<Flash>  flashes;
        std::vector<Burst>  bursts;
        std::mt19937 random{7};
        double clock = 0.0;
        // What a headless client saw, for its checks.
        int      states = 0, banners = 0;
        uint16_t firstRound = 0, lastRound = 0;
    };

    float unit(Client& c) { return std::uniform_real_distribution<float>(-1.0f, 1.0f)(c.random); }

    Entity own(World& world) { return runtime::entityForNetId(world, runtime::sessionStats(world).player); }

    // Where the own soldier is drawn (its Transform: between its last two steps), not where its
    // character was at the last step.
    vec3 drawnFeet(World& world, Entity e) {
        const projv::Transform* t = e != NullEntity && world.valid(e) ? world.try_get<projv::Transform>(e) : nullptr;
        return t ? t->position : vec3(0.0f);
    }

    void send(World& world, const projv::net::BitWriter& w, bool reliable = true) { runtime::sendToServer(world, w.data(), reliable); }

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

    // Where something broke, as the server said: a burst of chips in its colours.
    void noteBreak(World& world, const projv::BodyBroken& event) {
        const auto& scene = world.ctx().get<projv::Scene>();
        const projv::ComponentHandle h = runtime::voxelComponentOf(world, event.entity);
        if (h == projv::INVALID_COMPONENT_HANDLE) return;
        vec3 sum(0.0f);
        size_t n = 0;
        for (const auto& piece : event.pieceVoxels) for (auto v : piece) { sum += vec3(v) + 0.5f; n++; }
        for (auto v : event.dust) { sum += vec3(v) + 0.5f; n++; }
        if (n == 0) return;
        const projv::ComponentRecord& rec = scene.components[h];
        float voxel = 0.25f;
        const projv::ChunkHandle first = rec.kind == projv::ComponentKind::Chunk ? rec.chunkHandle
            : [&] { for (int32_t c : scene.grids[size_t(rec.gridIndex)].cellToChunk) if (c >= 0) return projv::ChunkHandle(c); return projv::INVALID_CHUNK_HANDLE; }();
        if (first < scene.chunks.size()) voxel = scene.chunks[first].header.voxelScale;
        const auto m = projv::utils::getComponentWorldMatrix(scene, h);
        const vec3 centre = vec3(m * vec4(sum / float(n) * (voxel / glm::length(vec3(m[0]))), 1.0f));
        std::vector<uint32_t> colours;
        for (const auto& material : rec.materialPalette) colours.push_back(material.packedColor);
        burst(world, centre, std::move(colours), int(std::min<size_t>(8 + event.dust.size() / 3, 30)), 4.0f,
              std::clamp(voxel * 0.6f, 0.05f, 0.15f));
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
            c.lastRound = state.round;
            if (!c.facedHome) {   // look toward the enemy, the first time
                c.facedHome = true;
                c.yaw = Frame::of(state.team).yaw();
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
            c.bannerFor = std::clamp(seconds, 0.5f, 10.0f);
            c.banners++;
            return;
        }
        if (kind == ToClient::Hurt) {
            const float amount = r.f32();
            if (!r.failed) c.hurt = std::min(1.0f, c.hurt + amount / 45.0f);
            return;
        }
        if (options.headless) return;   // the rest is for looking at
        if (kind == ToClient::Shot) {
            const vec3 from = readVec(r), velocity = readVec(r);
            if (r.failed || !finite(from) || !finite(velocity)) return;
            if (c.tracers.size() < 64) c.tracers.push_back({copyOf(world, c.tracer, from), from, velocity});
        } else if (kind == ToClient::Blast) {
            const vec3 at = readVec(r);
            if (r.failed || !finite(at)) return;
            c.flashes.push_back({copyOf(world, c.fireball, at, 0.6f), 0.0f, 1.6f});
            burst(world, at, {rgb(1.0f, 0.6f, 0.15f), rgb(0.95f, 0.85f, 0.3f), rgb(0.3f, 0.3f, 0.3f), rgb(0.2f, 0.2f, 0.2f)}, 26, 8.0f);
            c.shake = std::max(c.shake, 1.0f - std::min(glm::distance(at, c.eye) / 40.0f, 1.0f));
        } else if (kind == ToClient::Muzzle) {
            const vec3 at = readVec(r), aim = readVec(r);
            if (r.failed || !finite(at) || !finite(aim)) return;
            c.flashes.push_back({copyOf(world, c.fireball, at, 0.3f), 0.0f, 0.6f});
            burst(world, at + aim * 0.5f, {rgb(0.6f, 0.6f, 0.6f), rgb(0.45f, 0.45f, 0.45f), rgb(0.8f, 0.75f, 0.7f)}, 12, 3.0f);
            c.shake = std::max(c.shake, 0.6f - std::min(glm::distance(at, c.eye) / 30.0f, 0.6f));
        } else if (kind == ToClient::Death) {
            const vec3 at = readVec(r);
            const Team team = r.varint() ? Red : Blue;
            if (r.failed || !finite(at)) return;
            const uint32_t coat = team == Blue ? rgb(0.2f, 0.36f, 0.9f) : rgb(0.85f, 0.2f, 0.16f);
            burst(world, at, {coat, coat, rgb(0.95f, 0.8f, 0.65f), rgb(0.2f, 0.2f, 0.24f)}, 18, 4.0f);
        }
    }

    void effects(projv::Application& app) {
        World& world = app.world;
        Client& c = world.ctx().get<Client>();
        const float dt = app.time().delta;
        c.clock += dt;
        c.hurt = std::max(0.0f, c.hurt - dt * 1.2f);
        c.shake = std::max(0.0f, c.shake - dt * 2.0f);
        c.bannerFor -= dt;
        if (options.headless) return;
        const Entity me = own(world);
        // Tracers fly on as the server's bullets do, and stop at the first thing they meet here.
        for (Tracer& t : c.tracers) {
            t.age += dt;
            const vec3 next = t.position + t.velocity * dt;
            t.velocity.y -= BULLET_DROP * dt;
            const vec3 step = next - t.position;
            if (auto hit = runtime::castRay(world, t.position, glm::normalize(step), glm::length(step), solidLayers(), me, debris(world))) {
                burst(world, hit->position, {rgb(1.0f, 0.85f, 0.4f), rgb(0.5f, 0.5f, 0.5f)}, 3, 2.0f, 0.05f);
                t.age = 1e9f;
            }
            t.position = next;
            if (world.valid(t.e)) world.patch<projv::Transform>(t.e, [&](projv::Transform& tr) { tr.position = t.position; });
        }
        for (Tracer& t : c.tracers) if (t.age > 1.5f && world.valid(t.e)) world.destroy(t.e);
        std::erase_if(c.tracers, [](const Tracer& t) { return t.age > 1.5f; });
        // Fireballs grow and are gone in half a second.
        for (Flash& f : c.flashes) {
            f.age += dt;
            if (!world.valid(f.e)) continue;
            if (f.age > 0.45f) { world.destroy(f.e); continue; }
            world.patch<projv::Transform>(f.e, [&](projv::Transform& t) { t.scale = f.size * (0.4f + f.age * 3.5f); });
        }
        std::erase_if(c.flashes, [&](const Flash& f) { return f.age > 0.45f || !world.valid(f.e); });

        // Chips: made for each burst, gone after three seconds, never more than MAX_CHIPS.
        std::vector<Entity> old;
        for (auto [e, chip] : world.view<Chip>().each()) if (c.clock - chip.at > 3.0) old.push_back(e);
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
                world.emplace<projv::CollisionGroups>(e, projv::CollisionGroups{{"small debris"}, {"small debris"}, 0});
                world.emplace<Chip>(e, Chip{c.clock});
                runtime::setVelocity(world, e, glm::normalize(spread + vec3(0, 1.2f, 0)) * (b.speed * (0.6f + 0.6f * std::abs(unit(c)))),
                                     spread * 8.0f);
            }
        }
        c.bursts.clear();
    }

    // ---- Playing --------------------------------------------------------------------------------------

    vec3 crosshairTarget(World& world, const Client& c, Entity me) {
        if (auto hit = runtime::castRay(world, c.eye, c.look, 400.0f, solidLayers(), me, debris(world))) return hit->position;
        return c.eye + c.look * 400.0f;
    }

    void askToUse(World& world, Entity me, vec3 target) {
        projv::net::BitWriter w;
        w.varint(uint8_t(ToServer::Use));
        writeVec(w, drawnFeet(world, me) + vec3(0.0f, EYE_HEIGHT, 0.0f));
        writeVec(w, target);
        send(world, w);
    }

    void controls(projv::Application& app) {
        World& world = app.world;
        Client& c = world.ctx().get<Client>();
        const float dt = app.time().unscaledDelta;
        for (float& u : c.useClock) u = std::max(0.0f, u - dt);
        const bool joined = runtime::sessionStats(world).connected;
        const Entity me = own(world);
        const bool alive = joined && c.haveState && c.state.alive && me != NullEntity;
        const bool mounted = alive && c.state.cannon >= 0;
        const bool playing = alive && !c.state.over;

        if (options.gunner && alive) {
            // Up the stairs to the left cannon, on it, and fire at the enemy keep.
            const Frame home = Frame::of(c.state.team);
            const vec3 path[] = {home.at(6.0f, 0, -2.3f), home.at(8.25f, 0, -2.3f), home.at(8.25f, WALL_HEIGHT, -8.1f),
                                 home.at(9.6f, WALL_HEIGHT, -8.1f), home.cannonSeat(0)};
            static size_t next = 0;
            const vec3 at = drawnFeet(world, me);
            if (mounted) {
                runtime::setLocalInput(world, vec3(0.0f));
                const vec3 d = Frame::of(other(c.state.team)).at(-6.5f, KEEP_HEIGHT - 2.0f, 0.0f) - cannonPivot(c.state.cannon);
                c.yaw = std::atan2(d.z, d.x);
                c.pitch = ballisticPitch(glm::length(vec3(d.x, 0, d.z)), d.y, CANNON_SPEED, false).value_or(0.2f);
                clampCannon(c.state.cannon, c.yaw, c.pitch);
                if (c.state.reload <= 0.0f && c.useClock[0] <= 0.0f) {
                    c.useClock[0] = 1.0f;
                    askToUse(world, me, at);
                }
            } else if (next < std::size(path)) {
                vec3 to = path[next] - at;
                to.y = 0.0f;
                if (glm::length(to) < 0.6f) next++;
                else {
                    c.yaw = std::atan2(to.z, to.x);
                    runtime::setLocalInput(world, glm::normalize(to) * WALK_SPEED);
                    world.patch<projv::Transform>(me, [&](projv::Transform& t) { t.rotation = facing(c.yaw); });
                }
            } else if (c.useClock[1] <= 0.0f) {
                c.useClock[1] = 1.0f;
                runtime::setLocalInput(world, vec3(0.0f));
                projv::net::BitWriter w;
                w.varint(uint8_t(ToServer::Mount));
                send(world, w);
            }
            projv::net::BitWriter w;
            w.varint(uint8_t(ToServer::Aim));
            w.f32(c.yaw);
            w.f32(c.pitch);
            send(world, w, false);
            return;
        }
        if (options.headless || options.walk) {
            // Walk out of the gate toward the enemy, shooting ahead now and then.
            if (!options.walk || !alive) return runtime::setLocalInput(world, vec3(0.0f));
            world.patch<projv::Transform>(me, [&](projv::Transform& t) { t.rotation = facing(c.yaw); });
            const vec3 at = drawnFeet(world, me);
            const vec3 goal = Frame::of(other(c.state.team)).gateOutside();
            // Out through the gate first, while inside the walls.
            const vec3 toGate = Frame::of(c.state.team).gateOutside() - at;
            const vec3 to = std::abs(at.x) > FORTRESS_X - 13.0f ? toGate : goal - at;
            runtime::setLocalInput(world, glm::length(to) > 1.0f ? glm::normalize(vec3(to.x, 0, to.z)) * WALK_SPEED : vec3(0.0f));
            c.yaw = std::atan2(to.z, to.x);
            if (c.useClock[0] <= 0.0f && playing) {
                c.useClock[0] = 0.5f;
                askToUse(world, me, at + vec3(0, EYE_HEIGHT, 0) + direction(c.yaw, 0.0f) * 30.0f);
            }
            return;
        }

        auto& renderInstance = world.ctx().get<projv::graphics::RenderInstance>();
        const auto& input = world.ctx().get<projv::Input>();
        const bool captured = input.cursorCaptured;
        if (captured && input.pressed(projv::Key::Escape)) projv::graphics::setCursorCaptured(app, renderInstance, false);
        else if (!captured && input.pressed(projv::MouseButton::Left)) {
            projv::graphics::setCursorCaptured(app, renderInstance, true);
            return;   // that click was for the cursor
        }
        if (captured) {
            c.yaw = wrapAngle(c.yaw + input.cursorDelta.x * 0.0025f);
            c.pitch = std::clamp(c.pitch - input.cursorDelta.y * 0.0025f, -1.45f, 1.45f);
        }
        if (mounted) clampCannon(c.state.cannon, c.yaw, c.pitch);

        const vec3 forward{std::cos(c.yaw), 0.0f, std::sin(c.yaw)};
        const vec3 right{-std::sin(c.yaw), 0.0f, std::cos(c.yaw)};
        vec3 move(0.0f);
        if (input.down(projv::Key::W)) move += forward;
        if (input.down(projv::Key::S)) move -= forward;
        if (input.down(projv::Key::D)) move += right;
        if (input.down(projv::Key::A)) move -= right;

        // Out of the round: fly about and watch.
        if (joined && c.haveState && c.state.out) {
            const float speed = (input.down(projv::Key::LeftShift) ? 30.0f : 14.0f) * dt;
            c.eye += (glm::length(move) > 0.0f ? glm::normalize(move) : move) * speed;
            if (input.down(projv::Key::Space)) c.eye.y += speed;
            if (input.down(projv::Key::LeftControl)) c.eye.y -= speed;
        }
        if (playing && !mounted) {
            if (glm::length(move) > 0.0f) move = glm::normalize(move) * (input.down(projv::Key::LeftShift) ? SPRINT_SPEED : WALK_SPEED);
            runtime::setLocalInput(world, move, input.pressed(projv::Key::Space));
            // Your soldier faces where you look; everyone else sees it from the server.
            world.patch<projv::Transform>(me, [&](projv::Transform& t) { t.rotation = facing(c.yaw); });
        } else {
            runtime::setLocalInput(world, vec3(0.0f));
        }
        if (!joined) return;

        for (int i = 0; i < 3; i++)
            if (input.pressed(projv::Key(int(projv::Key::Num1) + i))) {
                c.state.item = uint8_t(i);
                projv::net::BitWriter w;
                w.varint(uint8_t(ToServer::Select));
                w.varint(uint64_t(i));
                send(world, w);
            }
        if (input.pressed(projv::Key::E) && alive) {
            projv::net::BitWriter w;
            w.varint(uint8_t(ToServer::Mount));
            send(world, w);
        }
        c.aimClock -= dt;
        if (c.aimClock <= 0.0f) {
            c.aimClock = 1.0f / 30.0f;
            projv::net::BitWriter w;
            w.varint(uint8_t(ToServer::Aim));
            w.f32(c.yaw);
            w.f32(c.pitch);
            send(world, w, false);
        }
        if (!captured || !playing) return;
        if (mounted) {
            if (input.pressed(projv::MouseButton::Left)) askToUse(world, me, crosshairTarget(world, c, me));
            return;
        }
        const uint8_t item = c.state.item;
        // Bullets while the button is held; a block or a blast a click.
        const bool wants = item == Bullet ? input.down(projv::MouseButton::Left) : input.pressed(projv::MouseButton::Left);
        if (wants && c.useClock[item] <= 0.0f && (item == Bullet || c.state.cooldown[item] <= 0.0f)) {
            c.useClock[item] = item == Bullet ? COOLDOWN[Bullet] : 0.3f;
            askToUse(world, me, crosshairTarget(world, c, me));
        }
    }

    // PostUpdate, once the physics has placed everything for this frame: where the camera is.
    void followCamera(projv::Application& app) {
        World& world = app.world;
        Client& c = world.ctx().get<Client>();
        const Entity me = own(world);
        c.marker.reset();
        if (!c.haveState || me == NullEntity) return;
        if (c.state.out) {   // flying, in controls
            c.look = direction(c.yaw, c.pitch);
            return;
        }
        // Dead (or just killed, and not told yet: your soldier is already in the pen below the field):
        // high over your own keep, looking out over the field.
        if (!c.state.alive || drawnFeet(world, me).y < -4.0f) {
            const Frame home = Frame::of(c.state.team);
            c.eye = home.at(-16.0f, 20.0f, 0.0f);
            c.look = glm::normalize(home.forward() + vec3(0.0f, -0.32f, 0.0f));
            return;
        }
        if (c.state.cannon >= 0) {
            // Behind and above the cannon, looking a little below where it points; the ring shows
            // where its ball would land.
            const vec3 flat{std::cos(c.yaw), 0.0f, std::sin(c.yaw)};
            c.eye = cannonPivot(c.state.cannon) - flat * 5.0f + vec3(0.0f, 2.2f, 0.0f);
            c.look = direction(c.yaw, c.pitch * 0.45f - 0.14f);
            c.marker = landing(world, muzzle(c.state.cannon, c.yaw, c.pitch), direction(c.yaw, c.pitch) * CANNON_SPEED);
            return;
        }
        // Over the shoulder, pulled in when a wall is behind.
        const vec3 head = drawnFeet(world, me) + vec3(0.0f, EYE_HEIGHT, 0.0f);
        c.look = direction(c.yaw, c.pitch);
        const vec3 right{-std::sin(c.yaw), 0.0f, std::cos(c.yaw)};
        vec3 back = -c.look * 4.3f + right * 0.95f + vec3(0.0f, 0.45f, 0.0f);
        const float length = glm::length(back);
        if (auto hit = runtime::castRay(world, head, back / length, length, solidLayers(), me, debris(world)))
            back *= std::max(hit->distance - 0.25f, 0.2f) / length;
        c.eye = head + back;
    }

    // ---- Drawing ------------------------------------------------------------------------------------

    constexpr const char* HUD_UNIFORMS[] = {"windowRes", "hudState", "hudCooldown", "hudFlags", "hudMarker", "hudBannerColour",
                                            "hudBanner0", "hudBanner1", "hudTop0", "hudTop1", "hudHint0", "hudHint1"};

    // The shared renderer (examples/shared/voxelRenderer), with one pass more: its display pass draws
    // into a texture instead of the window, and the HUD (hudShaders/hud.frag) draws that and itself
    // over it to the window. Built from the shared renderer's own files, so it changes as they do.
    void installRenderer(World& world, projv::graphics::RenderInstance& renderInstance, const fs::path& here) {
        voxel_renderer::View& view = world.ctx().emplace<voxel_renderer::View>();
        view.finestVoxel = 0.125f;
        view.hazeRadius = 240.0f;
        projv::RendererSpecification spec = projv::graphics::loadRendererSpecification((here / "voxelRenderer").string() + "/");
        for (projv::Texture& texture : spec.resources.textures)
            if (texture.sizeMode == projv::TextureSizeMode::Relative && texture.name != "reconstructed" && texture.name != "taaColor")
                texture.scale *= view.renderScale;
        uint32_t textureId = 0, frameBufferId = 0, shaderId = 0;
        for (const auto& t : spec.resources.textures) textureId = std::max(textureId, t.textureID + 1);
        for (const auto& f : spec.resources.FrameBuffers) frameBufferId = std::max(frameBufferId, f.frameBufferID + 1);
        for (const auto& s : spec.resources.shaders) shaderId = std::max(shaderId, s.shaderID + 1);

        projv::Texture shown;
        shown.textureID = textureId;
        shown.name = "shown";
        shown.format = bgfx::TextureFormat::RGBA8;
        shown.sizeMode = projv::TextureSizeMode::Relative;
        shown.scale = 1.0f;
        shown.origin = projv::CreateNew;
        spec.resources.textures.push_back(shown);
        spec.resources.FrameBuffers.push_back(projv::FrameBuffer{frameBufferId, {textureId}});
        projv::Shader hud;
        hud.shaderID = shaderId;
        hud.filePath = (here / "hudShaders/hud.bin").string();
        hud.shaderFileContents = projv::graphics::readFile(hud.filePath);
        spec.resources.shaders.push_back(hud);
        for (const char* name : HUD_UNIFORMS) spec.resources.uniforms.push_back(projv::Uniform{0, projv::Vec4, name});
        for (projv::RenderPass& pass : spec.dependencyGraph.renderPasses)
            if (pass.frameBufferOutputID == -1) pass.frameBufferOutputID = int(frameBufferId);
        projv::RenderPass pass{};
        pass.shaderID = shaderId;
        pass.frameBufferInputIDs = {frameBufferId};
        pass.frameBufferOutputID = -1;
        spec.dependencyGraph.renderPasses.push_back(pass);

        renderInstance.addRendererSpecification(1, spec);
        bgfx::ShaderHandle vertexShader = projv::graphics::loadShader((here / "voxelRenderer/shaders/vs_quad.bin").string());
        renderInstance.setActiveRenderer(projv::graphics::constructRendererSpecification(renderInstance.getRendererSpecification(1), vertexShader));
    }

    std::string clockText(float seconds) {
        const int s = int(std::ceil(std::max(seconds, 0.0f)));
        char text[16];
        std::snprintf(text, sizeof text, "%d:%02d", s / 60, s % 60);
        return text;
    }

    void uploadHud(World& world, projv::graphics::RenderInstance& renderInstance) {
        Client& c = world.ctx().get<Client>();
        auto renderer = renderInstance.getActiveRenderer();
        auto set = [&](const char* name, vec4 value) { projv::graphics::setUniformToValue(renderer, name, value); };
        auto text = [&](const char* a, const char* b, const std::string& line) {
            vec4 first, second;
            packText(line, first, second);
            set(a, first);
            set(b, second);
        };
        const State& s = c.state;
        const bool joined = runtime::sessionStats(world).connected && c.haveState;
        const bool alive = joined && s.alive;
        const bool mounted = alive && s.cannon >= 0;
        const projv::core::vec2 resolution = renderInstance.getWindowResolution();
        set("windowRes", vec4(resolution.x, resolution.y, 0.0f, 0.0f));
        set("hudState", vec4(s.health / MAX_HEALTH, float(s.item), mounted ? 1.0f : 0.0f, c.hurt));
        set("hudCooldown", vec4(std::max(s.cooldown[0], c.useClock[0] / COOLDOWN[0]), s.cooldown[1], s.cooldown[2], s.reload));
        set("hudFlags", vec4(s.flagUp[0] ? 1.0f : 0.0f, s.flagUp[1] ? 1.0f : 0.0f, float(s.team), alive ? 1.0f : 0.0f));
        set("hudMarker", c.marker ? vec4(*c.marker, 1.0f) : vec4(0.0f));
        set("hudBannerColour", vec4(c.bannerColour, c.bannerFor > 0.0f ? 1.0f : 0.0f));
        text("hudBanner0", "hudBanner1", c.bannerFor > 0.0f ? c.banner : "");
        text("hudTop0", "hudTop1", joined ? "BLUE " + std::to_string(s.standing[Blue]) + "   " + clockText(s.timeLeft) + "   RED " +
                                                std::to_string(s.standing[Red])
                                          : "");

        std::string hint;
        if (!joined) hint = "JOINING...";
        else if (s.over) hint = "";
        else if (s.out) hint = "FLAG DOWN - SPECTATING";
        else if (!s.alive) hint = "RESPAWN IN " + std::to_string(int(std::ceil(std::max(s.respawnIn, 0.0f))));
        else if (mounted) hint = "CLICK FIRE / E LEAVE";
        else {
            const vec3 at = drawnFeet(world, own(world));
            for (int side = 0; side < 2; side++)
                if (glm::distance(at, Frame::of(s.team).cannonSeat(side)) < CANNON_REACH) hint = "E: MAN THE CANNON";
        }
        text("hudHint0", "hudHint1", hint);
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
        // A blast near you shakes the picture (aiming uses the steady camera).
        vec3 eye = c.eye, look = c.look;
        if (c.shake > 0.0f) {
            const float t = float(c.clock), amount = c.shake * c.shake;
            eye += amount * 0.25f * vec3(std::sin(t * 37.0f), std::sin(t * 41.0f + 1.0f), std::sin(t * 53.0f + 2.0f));
            look = glm::normalize(look + amount * 0.03f * vec3(std::sin(t * 61.0f), std::sin(t * 47.0f), 0.0f));
        }
        voxel_renderer::upload(world, renderInstance, eye, look);
        uploadHud(world, renderInstance);
        projv::graphics::renderConstructedRenderer(renderInstance, renderInstance.getActiveRenderer(), &gpu);
        if (!options.capture.empty()) {
            static double next = 4.0;
            if (c.clock >= next) {
                bgfx::requestScreenShot(BGFX_INVALID_HANDLE, (options.capture + "-" + std::to_string(int(next))).c_str());
                next += 4.0;
            }
        }

        c.titleClock -= app.time().unscaledDelta;
        if (c.titleClock <= 0.0f) {
            c.titleClock = 0.5f;
            const State& s = c.state;
            std::string title = "ProjectV Siege";
            if (c.haveState)
                title += std::string("  |  ") + teamName(s.team) + "  |  round " + std::to_string(s.round) + "  |  kills " +
                         std::to_string(s.kills) + "  deaths " + std::to_string(s.deaths) +
                         (hosted ? "  |  hosting on port " + std::to_string(options.port) : "");
            glfwSetWindowTitle(renderInstance.window, title.c_str());
        }
        if (renderInstance.shouldClose) app.closeRequested = true;
    }

    // Headless: finish on time, after checking.
    void watch(projv::Application& app) {
        if (options.seconds <= 0.0) return;
        Client& c = app.world.ctx().get<Client>();
        static double clock = 0.0;
        clock += app.time().unscaledDelta;
        if (clock < options.seconds) return;
        const runtime::SessionStats stats = runtime::sessionStats(app.world);
        const int rounds = c.firstRound ? c.lastRound - c.firstRound + 1 : 0;
        std::printf("SIEGETEST: %s: %s, %s, %d states, %d banners, rounds %d..%d, kills %u deaths %u, standing %u/%u, "
                    "world changes %llu, mismatches %llu\n",
                    options.name.c_str(), stats.connected ? "joined" : "not joined", c.haveState ? teamName(c.state.team) : "no team",
                    c.states, c.banners, c.firstRound, c.lastRound, c.state.kills, c.state.deaths, c.state.standing[0],
                    c.state.standing[1], (unsigned long long)stats.worldChanges, (unsigned long long)stats.worldMismatches);
        const bool ok = stats.connected && c.states > 10 && c.banners > 0 && rounds >= options.expectRounds && stats.worldMismatches == 0;
        std::printf("SIEGETEST: %s: %s\n", options.name.c_str(), ok ? "PASS" : "FAIL");
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
            server.teamSize = options.teamSize;
            server.roundSeconds = options.roundSeconds;
            server.folder = folder;
            if (!siege::installServer(*hosted, server)) {
                projv::core::error("siege: could not host on port {}; is a game already running here? (--host 127.0.0.1 joins it)",
                                   options.port);
                std::exit(1);
            }
            hosted->runStartup();
            projv::core::info("siege: hosting on port {}: others join with --host <this machine's address>", options.port);
        }

        if (!options.headless) {
            auto& renderInstance = world.ctx().emplace<projv::graphics::RenderInstance>();
            renderInstance.initialize(1600, 900, "ProjectV Siege");
            projv::graphics::installPlatform(app, renderInstance);
            projv::graphics::setCursorCaptured(app, renderInstance, true);
        }
        auto& scene = world.ctx().emplace<projv::Scene>(projv::utils::loadComposeFromDisk(folder));
        if (scene.components.empty()) {
            projv::core::error("siege: no field in '{}'", folder);
            std::exit(1);
        }
        // Physics here too: this client walks its own soldier at once, applies the server's breaks,
        // and throws its own chips about.
        runtime::installSceneBridge(app);
        runtime::installPhysics(app);
        runtime::loadPhysicsMaterials(world, (fs::path(folder) / MATERIALS).string());
        for (const char* group : {"small debris", "player"}) runtime::definePhysicsGroup(world, group);
        runtime::spawnEntities(world);
        Client& c = world.ctx().emplace<Client>();
        if (!options.headless) {
            c.fireball = prototype(scene, "fireball", 8, 0.25f, 12.0f, [](int x, int y, int z) -> uint32_t {
                const float r = glm::length(vec3(x, y, z) + 0.5f - 4.0f);
                return r > 4.0f ? 0u : r < 2.5f ? rgb(1.0f, 0.9f, 0.4f) : rgb(1.0f, 0.45f, 0.1f);
            });
            c.tracer = prototype(scene, "tracer", 2, 0.06f, 8.0f, [](int, int, int) { return rgb(1.0f, 0.85f, 0.35f); });
            app.events().on<projv::BodyBroken>([&world](const projv::BodyBroken& e) { noteBreak(world, e); });
        }

        runtime::ClientConfig config;
        config.host = options.host.empty() ? "127.0.0.1" : options.host;
        config.port = options.port;
        config.sceneFolder = folder;
        config.name = options.name;
        config.onMessage = [](World& w, std::span<const uint8_t> message) { heard(w, message); };
        if (!runtime::installClient(app, projv::net::makeEnetTransport(), config)) std::exit(1);
        app.events().on<runtime::LeftServer>([&app](const runtime::LeftServer& e) {
            projv::core::warn("siege: left the game: {}", e.reason);
            std::printf("SIEGETEST: %s: left the game: %s\n", options.name.c_str(), e.reason.c_str());
            exitCode = 1;
            app.closeRequested = true;
        });

        if (options.headless) return;
        installRenderer(world, world.ctx().get<projv::graphics::RenderInstance>(), here);
        world.ctx().emplace<projv::GPUData>(projv::graphics::createTexturesForScene(scene));
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
    for (int i = 1; i < argc; i++) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--host")) options.host = next();
        else if (!std::strcmp(argv[i], "--port")) options.port = uint16_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--name")) options.name = next();
        else if (!std::strcmp(argv[i], "--team-size")) options.teamSize = std::clamp(std::atoi(next()), 1, 16);
        else if (!std::strcmp(argv[i], "--round-seconds")) options.roundSeconds = std::max(10.0f, float(std::atof(next())));
        else if (!std::strcmp(argv[i], "--headless")) options.headless = true;
        else if (!std::strcmp(argv[i], "--seconds")) options.seconds = std::atof(next());
        else if (!std::strcmp(argv[i], "--walk")) options.walk = true;
        else if (!std::strcmp(argv[i], "--gunner")) options.gunner = true;
        else if (!std::strcmp(argv[i], "--capture")) options.capture = next();
        else if (!std::strcmp(argv[i], "--expect-rounds")) options.expectRounds = std::atoi(next());
    }
    for (char& ch : options.name) ch = char(std::toupper(static_cast<unsigned char>(ch)));

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
