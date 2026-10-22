// What the playground server and its clients share: the tools and their numbers, the messages each
// way, and the HUD's text packing.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "net/wire.h"
#include "world.h"

namespace playground {
    constexpr uint16_t PORT = 27018;

    constexpr float WALK_SPEED = 5.5f, EYE_HEIGHT = 1.4f;
    constexpr float FALLEN_BELOW = -25.0f;     // m: off the edge, or down a hole -- back to the plaza
    constexpr float GRAVITY = 9.81f, MOON_GRAVITY = 1.62f;

    // ---- Tools ------------------------------------------------------------------------------------------
    //
    // Each player has all eight, one in hand (1-8, or the wheel); a click uses it where the crosshair
    // points. The server casts the same ray, from the camera the client says it looks through.
    enum Tool : uint8_t { Cannon = 0, Blast, Tnt, Crate, Ball, Build, Dig, Push };
    constexpr int TOOLS = 8;
    inline const char* toolName(Tool t) {
        static const char* names[TOOLS] = {"CANNON", "BLAST", "TNT", "CRATE", "BALL", "BUILD", "DIG", "PUSH"};
        return names[t];
    }
    constexpr float COOLDOWN[TOOLS] = {0.6f, 1.2f, 0.7f, 0.25f, 0.4f, 0.12f, 0.1f, 0.5f};
    // How far the crosshair reaches for each: a blast lands anywhere in sight; building, digging and
    // pushing are done close to.
    constexpr float REACH[TOOLS] = {200.0f, 120.0f, 200.0f, 200.0f, 200.0f, 10.0f, 10.0f, 12.0f};
    // The ring drawn where the crosshair meets something: what the tool will take in (0: none).
    constexpr float RING[TOOLS] = {0.0f, 4.5f, 0.0f, 0.0f, 0.0f, 0.55f, 1.3f, 0.0f};
    // A client's camera may be this far from its player's head (third person's is 6 m back).
    constexpr float CAMERA_SLACK = 9.0f;

    constexpr float CANNON_SPEED = 48.0f, CANNONBALL_SECONDS = 10.0f;
    constexpr float BLAST_RADIUS = 4.5f, BLAST_ENERGY = 3.0e5f, BLAST_KICK = 18.0f, BLAST_KNOCK = 16.0f;
    constexpr float TNT_THROW = 16.0f, TNT_FUSE = 6.0f, TNT_ARMED = 0.4f, TNT_JOLT = 9.0f, TNT_CHAIN = 0.15f;
    constexpr float THROW_SPEED = 12.0f;       // crates and balls
    constexpr float DIG_RADIUS = 1.3f;
    constexpr float PUSH_SPEED = 16.0f, PUSH_CONE = 0.7f;   // cos of the cone's half-angle
    constexpr float KNOCK_DECAY = 14.0f;       // m/s each second: how fast a knock wears off
    constexpr int   MAX_THINGS = 160;          // cannonballs, TNT, crates, balls made by players: past it the oldest go
    constexpr int   MAX_BLOCKS = 120;          // a player's built blocks: past it the oldest go
    constexpr float RESET_HOLD = 1.5f;         // s: Backspace held this long puts the world back

    inline vec3 forwardOf(float yaw) { return {std::cos(yaw), 0.0f, std::sin(yaw)}; }
    inline vec3 lookDir(float yaw, float pitch) { return vec3(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)); }
    // A player's turn, built facing +z.
    inline quat facing(float yaw) { return glm::angleAxis(1.5707963f - yaw, vec3(0, 1, 0)); }
    inline float wrapAngle(float a) {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }
    // Where each player starts and comes back to: round the plaza, facing the castle.
    inline vec3 spawnPoint(int slot) {
        const float angle = 0.7853982f + float(slot) * 1.5707963f;
        return vec3(std::cos(angle) * 3.0f, 0.05f, std::sin(angle) * 3.0f);
    }

    // ---- Text for the HUD ---------------------------------------------------------------------------
    // 24 characters packed three to a float, six bits each, centred: what hud.frag draws.
    inline uint32_t textCode(char c) {
        if (c >= '0' && c <= '9') return uint32_t(c - '0') + 1;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return uint32_t(c - 'A') + 11;
        switch (c) {
            case ':': return 37;
            case '!': return 38;
            case '-': return 39;
            case '/': return 40;
            case '.': return 41;
            case '>': return 42;
            case '+': return 43;
            default:  return 0;
        }
    }
    inline void packText(const std::string& text, projv::core::vec4& a, projv::core::vec4& b) {
        uint32_t codes[24] = {};
        const size_t length = std::min<size_t>(text.size(), 24), pad = (24 - length) / 2;
        for (size_t i = 0; i < length; i++) codes[pad + i] = textCode(text[i]);
        float packed[8];
        for (int f = 0; f < 8; f++) packed[f] = float(codes[f * 3] + codes[f * 3 + 1] * 64u + codes[f * 3 + 2] * 4096u);
        a = projv::core::vec4(packed[0], packed[1], packed[2], packed[3]);
        b = projv::core::vec4(packed[4], packed[5], packed[6], packed[7]);
    }

    // ---- Messages -----------------------------------------------------------------------------------
    //
    // A client says where its player faces, and when it uses a tool: which, and the ray it aims along
    // (its camera's eye and direction). It asks for the gravity to change and the world to be put back.
    // The server does it, and tells everyone what to draw (Boom), a player thrown by a blast or a push
    // (Knock, so its own walking carries it, as in the party), and how things stand (State).
    enum class ToServer : uint8_t { Look = 1, Use, Gravity, Reset };
    enum class ToClient : uint8_t { State = 1, Banner, Knock, Boom };
    // What a Boom draws: a blast, a puff (a push, a block laid, a shot), dust (dug).
    enum class BoomKind : uint8_t { Blast = 0, Puff, Dust, Muzzle };

    inline void writeVec(projv::net::BitWriter& w, vec3 v) { for (int i = 0; i < 3; i++) w.f32(v[i]); }
    inline vec3 readVec(projv::net::BitReader& r) {
        vec3 v;
        for (int i = 0; i < 3; i++) v[i] = r.f32();
        return v;
    }
    inline bool finite(vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    // How things stand: what each client is told several times a second.
    struct State {
        bool    moon = false;            // the gravity
        int8_t  you = -1;                // which player is this client's
        float   cooldown[TOOLS] = {};    // its tools', 0..1 of the way to ready (0 ready)
        uint16_t things = 0, pieces = 0; // what is about: thrown things, broken pieces
        struct Player {
            bool        present = false;
            std::string name;
        };
        Player players[MAX_PLAYERS];

        void write(projv::net::BitWriter& w) const {
            w.varint(uint8_t(ToClient::State));
            w.boolean(moon);
            w.varint(uint8_t(you + 1));
            for (float c : cooldown) w.f32(c);
            w.varint(things);
            w.varint(pieces);
            for (const Player& p : players) {
                w.boolean(p.present);
                if (p.present) w.bytes(std::vector<uint8_t>(p.name.begin(), p.name.end()));
            }
        }
        bool read(projv::net::BitReader& r) {
            moon = r.boolean();
            you = int8_t(std::min<uint64_t>(r.varint(), MAX_PLAYERS)) - 1;
            for (float& c : cooldown) c = r.f32();
            things = uint16_t(std::min<uint64_t>(r.varint(), 65535));
            pieces = uint16_t(std::min<uint64_t>(r.varint(), 65535));
            for (Player& p : players) {
                p = Player{};
                p.present = r.boolean();
                if (!p.present) continue;
                const std::vector<uint8_t> name = r.bytes(16);
                p.name.assign(name.begin(), name.end());
            }
            return !r.failed;
        }
    };
}
