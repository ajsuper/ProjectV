// What the siege server and its clients share: the rules' numbers, the game's messages each way, and
// the arithmetic both sides must do alike (where a cannon points, where its ball goes).
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "fortress.h"
#include "net/wire.h"

namespace siege {
    constexpr uint16_t PORT = 27016;

    // ---- The rules ----------------------------------------------------------------------------------
    constexpr float ROUND_SECONDS = 180.0f;    // a round, unless a team is wiped out first
    constexpr float ROUND_OVER_SECONDS = 6.0f; // the result shown, then the next round
    constexpr float RESPAWN_SECONDS = 4.0f;
    constexpr float MAX_HEALTH = 100.0f;
    constexpr float FLAG_DOWN_BELOW = 6.0f;    // m: a flag lower than this has been knocked down
    constexpr int   TEAM_SIZE = 4;             // bots fill each team up to this

    constexpr float WALK_SPEED = 4.5f, SPRINT_SPEED = 6.5f;
    constexpr float EYE_HEIGHT = 1.6f;

    enum Item : uint8_t { Bullet = 0, Blocks = 1, Blast = 2 };
    constexpr float COOLDOWN[3] = {0.25f, 0.5f, 8.0f};

    constexpr float BULLET_SPEED = 90.0f, BULLET_DAMAGE = 20.0f, BULLET_ENERGY = 2500.0f, BULLET_DROP = 3.0f;
    constexpr float BLOCK_REACH = 8.0f;
    constexpr int   MAX_BLOCKS = 40;           // a player's; the oldest goes
    constexpr float BLAST_RANGE = 40.0f, BLAST_RADIUS = 4.5f, BLAST_ENERGY = 3.0e5f, BLAST_DAMAGE = 85.0f;

    constexpr float CANNON_RELOAD = 5.0f, CANNON_SPEED = 42.0f, CANNON_REACH = 2.5f;
    constexpr float CANNONBALL_DAMAGE = 100.0f, CANNONBALL_SECONDS = 8.0f;
    constexpr float CANNON_YAW_LIMIT = 1.1f, CANNON_PITCH_MIN = -0.25f, CANNON_PITCH_MAX = 0.8f;
    constexpr float GRAVITY = 9.81f;

    inline vec3 direction(float yaw, float pitch) {
        return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
    }

    inline float wrapAngle(float a) {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }

    // ---- Cannons ------------------------------------------------------------------------------------
    // Four: Blue's two (0, 1), then Red's (2, 3); in each fortress the left (v = -5) then the right.
    constexpr int CANNONS = 4;
    inline Team cannonTeam(int cannon) { return cannon < 2 ? Blue : Red; }
    inline int cannonSide(int cannon) { return cannon % 2; }
    inline vec3 cannonPivot(int cannon) { return Frame::of(cannonTeam(cannon)).cannonPivot(cannonSide(cannon)); }

    // Where a cannon points when asked to point at (yaw, pitch): no further round than the gap in the
    // parapet allows, nor higher or lower than the barrel goes.
    inline void clampCannon(int cannon, float& yaw, float& pitch) {
        const float base = Frame::of(cannonTeam(cannon)).yaw();
        yaw = base + std::clamp(wrapAngle(yaw - base), -CANNON_YAW_LIMIT, CANNON_YAW_LIMIT);
        pitch = std::clamp(pitch, CANNON_PITCH_MIN, CANNON_PITCH_MAX);
    }
    inline vec3 muzzle(int cannon, float yaw, float pitch) { return cannonPivot(cannon) + direction(yaw, pitch) * 1.3f; }

    // The barrel's turn: built along +x, turned up by the pitch, then round by the yaw.
    inline quat barrelRotation(float yaw, float pitch) {
        return glm::angleAxis(-yaw, vec3(0, 1, 0)) * glm::angleAxis(pitch, vec3(0, 0, 1));
    }
    // A soldier's, built facing +z.
    inline quat facing(float yaw) { return glm::angleAxis(1.5707963f - yaw, vec3(0, 1, 0)); }

    // The pitch that lands a ball fired at `speed` on a point `across` metres away and `up` metres
    // higher: the low arc, or the high. Nothing when it is out of reach.
    inline std::optional<float> ballisticPitch(float across, float up, float speed, bool high) {
        const float v2 = speed * speed;
        const float root = v2 * v2 - GRAVITY * (GRAVITY * across * across + 2.0f * up * v2);
        if (root < 0.0f || across < 0.01f) return std::nullopt;
        return std::atan((v2 + (high ? 1.0f : -1.0f) * std::sqrt(root)) / (GRAVITY * across));
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
    // A client says what its player does beyond walking (walking is runtime::setLocalInput): where it
    // looks, what it uses, what it holds, the cannon. The server decides, and tells each client how its
    // player and the round stand, and every client what to draw that is not an entity: shots, blasts,
    // deaths, banners.

    enum class ToServer : uint8_t { Aim = 1, Use, Select, Mount };
    enum class ToClient : uint8_t { State = 1, Banner, Shot, Blast, Muzzle, Death, Hurt };

    inline void writeVec(projv::net::BitWriter& w, vec3 v) { for (int i = 0; i < 3; i++) w.f32(v[i]); }
    inline vec3 readVec(projv::net::BitReader& r) {
        vec3 v;
        for (int i = 0; i < 3; i++) v[i] = r.f32();
        return v;
    }
    inline bool finite(vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    // What a client is told of the round and its own player, several times a second.
    struct State {
        float    timeLeft = ROUND_SECONDS;
        uint16_t round = 1;
        bool     over = false;
        bool     flagUp[2] = {true, true};
        uint8_t  standing[2] = {0, 0};   // players alive or waiting to respawn, by team
        Team     team = Blue;
        float    health = MAX_HEALTH;
        bool     alive = true, out = false;
        float    respawnIn = 0.0f;
        uint8_t  item = Bullet;
        float    cooldown[3] = {0, 0, 0};
        int8_t   cannon = -1;            // the cannon its player is on
        float    reload = 0.0f;
        uint16_t kills = 0, deaths = 0;

        void write(projv::net::BitWriter& w) const {
            w.varint(uint8_t(ToClient::State));
            w.f32(timeLeft);
            w.varint(round);
            w.boolean(over);
            w.boolean(flagUp[0]);
            w.boolean(flagUp[1]);
            w.varint(standing[0]);
            w.varint(standing[1]);
            w.varint(team);
            w.f32(health);
            w.boolean(alive);
            w.boolean(out);
            w.f32(respawnIn);
            w.varint(item);
            for (float c : cooldown) w.f32(c);
            w.varint(uint8_t(cannon + 1));
            w.f32(reload);
            w.varint(kills);
            w.varint(deaths);
        }
        bool read(projv::net::BitReader& r) {
            timeLeft = r.f32();
            round = uint16_t(r.varint());
            over = r.boolean();
            flagUp[0] = r.boolean();
            flagUp[1] = r.boolean();
            standing[0] = uint8_t(r.varint());
            standing[1] = uint8_t(r.varint());
            team = r.varint() ? Red : Blue;
            health = r.f32();
            alive = r.boolean();
            out = r.boolean();
            respawnIn = r.f32();
            item = uint8_t(std::min<uint64_t>(r.varint(), 2));
            for (float& c : cooldown) c = r.f32();
            cannon = int8_t(std::min<uint64_t>(r.varint(), CANNONS)) - 1;
            reload = r.f32();
            kills = uint16_t(r.varint());
            deaths = uint16_t(r.varint());
            return !r.failed;
        }
    };
}
