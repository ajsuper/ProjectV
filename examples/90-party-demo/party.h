// What the party server and its clients share: the rules' numbers, the messages each way, and the
// HUD's text packing.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "arena.h"
#include "net/wire.h"

namespace party {
    constexpr uint16_t PORT = 27017;

    // ---- The rules ----------------------------------------------------------------------------------
    //
    // A match is ROUNDS rounds, the five games in turn. Each round ranks the players, and the ranks
    // score POINTS; the most points at the end takes the match.
    constexpr int   ROUNDS = 5;
    constexpr int   POINTS[MAX_PLAYERS] = {3, 2, 1, 0};
    constexpr float INTRO_SECONDS = 4.0f;       // the round's name and its one rule, everyone held still
    constexpr float OVER_SECONDS = 5.0f;        // who won it
    constexpr float MATCH_OVER_SECONDS = 10.0f;
    //                                     Crumble Tag     Dash    Climb   Blueprint
    constexpr float ROUND_SECONDS[MODES] = {120.0f, 150.0f, 120.0f, 100.0f, 90.0f};
    constexpr float FALLEN_BELOW = -6.0f;       // m: below every floor -- on the way to the lava

    constexpr float WALK_SPEED = 5.0f, EYE_HEIGHT = 1.4f;
    constexpr float EYE_AHEAD = 0.35f;          // m: a first-person eye is this far in front of the body's middle

    // Everyone, every game: a shove -- whoever is in front, near, is thrown back.
    constexpr float SHOVE_REACH = 2.4f, SHOVE_CONE = 0.45f;   // cos of the half-angle
    constexpr float SHOVE_SPEED = 11.0f, SHOVE_COOLDOWN = 1.2f;
    constexpr float KNOCK_DECAY = 14.0f;        // m/s each second: how fast a shove wears off

    // Crumble: a tile stood on turns orange, then red, then drops.
    constexpr float CRACK_SECONDS = 0.35f, DROP_SECONDS = 0.8f;

    // Bomb tag: whoever holds the bomb passes it by touching someone. When it goes off it takes its
    // holder and the floor round them.
    constexpr float FUSE_MIN = 13.0f, FUSE_MAX = 19.0f;
    constexpr float PASS_REACH = 1.3f, PASS_COOLDOWN = 0.8f, CARRIER_SPEED = 1.25f;
    constexpr float NO_TAG_BACKS = 3.0f;        // s: not straight back to whoever passed it to you
    constexpr float CRATER_RADIUS = 3.2f, NEXT_BOMB_SECONDS = 2.0f;

    // Blocks: C lays one -- a tile into a gap in front of you (Sky Dash, and a few in Crumble to save
    // yourself), a cube on a plot (Sky Climb, Blueprint): in front of you, or in first person
    // where the crosshair points. X takes away the cube the crosshair is on (or the one in front).
    constexpr int   BLOCKS[MODES] = {3, 0, 8, 24, 40};                    // each round's
    constexpr float BLOCK_REGROW[MODES] = {0.0f, 0.0f, 1.5f, 1.2f, 0.0f};  // s for another, 0 none
    constexpr bool  BREAKS[MODES] = {false, false, false, true, true};    // whether X does anything
    constexpr float BLOCK_COOLDOWN = 0.25f, REACH = 6.0f;

    // Bombs: R lobs one, further the longer it is held; it bursts where it lands, knocking out tiles
    // (and in Sky Climb, cubes) and throwing players about.
    constexpr float THROW_COOLDOWN[MODES] = {0.0f, 0.0f, 5.0f, 4.0f, 0.0f};   // 0: none in that game
    constexpr float CHARGE_SECONDS = 1.0f;      // held this long, the longest throw
    constexpr float THROW_NEAR = 4.0f, THROW_FAR = 15.0f, THROW_UP = 5.5f, THROW_FUSE = 3.0f;
    constexpr float BOMB_RADIUS = 2.2f, BOMB_KNOCK_RADIUS = 3.8f, BOMB_KNOCK = 13.0f;
    constexpr float BOMB_BITE = 1.3f;           // m: how much of a tower a bomb takes out
    constexpr float GRAVITY = 9.81f;
    inline vec3 throwVelocity(float yaw, float charge) {
        const float c = std::clamp(charge, 0.0f, 1.0f);
        return vec3(std::cos(yaw), 0.0f, std::sin(yaw)) * (THROW_NEAR + (THROW_FAR - THROW_NEAR) * c) + vec3(0.0f, THROW_UP, 0.0f);
    }
    inline vec3 throwOrigin(vec3 feet, float yaw) { return feet + vec3(0.0f, 1.6f, 0.0f) + vec3(std::cos(yaw), 0.0f, std::sin(yaw)) * 0.5f; }

    // Sky Dash: first home wins; a fall puts you back at your last checkpoint. The sweepers turn, the
    // pushers slide, and either throws whoever it meets.
    constexpr float RESPAWN_SECONDS = 1.5f;
    constexpr float TILE_REGROW = 2.5f;         // s: a dropped tile of the crumbling strip comes back
    constexpr float DASH_CRACK_SECONDS = 0.6f, DASH_DROP_SECONDS = 1.4f;   // the strip is kinder than Crumble
    constexpr float SWEEPER_SPEED = 1.1f, PUSHER_SPEED = 1.6f, PUSHER_REACH = 3.2f, HAZARD_KNOCK = 13.0f;

    // Sky Climb: build up -- jump and lay a cube under you -- to the gold on the posts; bomb the others'
    // towers down. A fall off the ground puts you back on it.
    // Blueprint: copy the gold shape on the display onto your pad. Right cubes count, wrong ones (on
    // the pad, anywhere) count against; the first exact copy wins.

    inline vec3 forwardOf(float yaw) { return {std::cos(yaw), 0.0f, std::sin(yaw)}; }
    // A player's turn, built facing +z.
    inline quat facing(float yaw) { return glm::angleAxis(1.5707963f - yaw, vec3(0, 1, 0)); }
    inline float wrapAngle(float a) {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }

    // Where the players start in each game: round the edge of the arena (the top floor in Crumble),
    // side by side on Sky Dash's start, beside their own pad in Blueprint.
    inline vec3 startPoint(Mode mode, int slot) {
        if (mode == Dash) return vec3(DASH_X + (float(slot) - 1.5f) * 2.2f, 0.05f, DASH_CHECKPOINTS[0]);
        if (mode == Blueprint) {
            const vec3 pad = padCentre(slot), toMiddle = glm::normalize(arenaCentre(Blueprint) - pad);
            return pad + toMiddle * (PAD_CELLS * 0.5f + 1.0f) + vec3(0.0f, 0.05f, 0.0f);
        }
        const float angle = 0.7853982f + float(slot) * 1.5707963f;
        const float top = mode == Crumble ? 8.0f : 0.0f;
        const float out = mode == Climb ? 3.5f : 8.0f;
        return arenaCentre(mode) + vec3(std::cos(angle) * out, top + 0.05f, std::sin(angle) * out);
    }
    // Where a racer comes back after a fall: its last checkpoint, its own lane.
    inline vec3 checkpoint(int index, int slot) { return vec3(DASH_X + (float(slot) - 1.5f) * 2.2f, 0.05f, DASH_CHECKPOINTS[index]); }

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
    // A client says where its player faces and when it shoves or throws (walking is
    // runtime::setLocalInput). The server decides, and tells each client how the round and the
    // players stand, and everyone what to draw: banners, bursts. A player shoved is told, so its own
    // client carries it back as the server does.

    // Place and Break carry whether they were aimed (first person: from the eye, along yaw and pitch)
    // or not (third person: in front of the player).
    enum class ToServer : uint8_t { Look = 1, Shove, Throw, Place, Break };
    enum class ToClient : uint8_t { State = 1, Banner, Knock, Boom };
    enum Phase : uint8_t { Intro = 0, Playing, Over, MatchOver };

    inline void writeVec(projv::net::BitWriter& w, vec3 v) { for (int i = 0; i < 3; i++) w.f32(v[i]); }
    inline vec3 readVec(projv::net::BitReader& r) {
        vec3 v;
        for (int i = 0; i < 3; i++) v[i] = r.f32();
        return v;
    }
    inline bool finite(vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    // How the round stands, and every player: what a client is told several times a second.
    struct State {
        Mode     mode = Crumble;
        Phase    phase = Intro;
        uint8_t  round = 1, rounds = ROUNDS;
        float    clock = 0.0f;           // the intro's countdown, or the round's time left
        float    fuse = -1.0f;           // bomb tag: the fuse left, 0..1; -1 no bomb
        int8_t   you = -1;               // which player is this client's
        float    shove = 0.0f, toss = 0.0f;   // its cooldowns left, 0..1 (toss -1: no bombs in this game)
        uint8_t  blocks = 0;             // its blocks left
        struct Player {
            bool        present = false, alive = false, carrier = false;
            std::string name;
            uint8_t     points = 0;
            uint8_t     place = 0;       // Sky Dash: where it finished (1 first), 0 not yet
            float       extra = 0.0f;    // Sky Dash: metres along the course; Sky Climb: height; Blueprint: match 0..1
        };
        Player players[MAX_PLAYERS];

        void write(projv::net::BitWriter& w) const {
            w.varint(uint8_t(ToClient::State));
            w.varint(mode);
            w.varint(phase);
            w.varint(round);
            w.varint(rounds);
            w.f32(clock);
            w.f32(fuse);
            w.varint(uint8_t(you + 1));
            w.f32(shove);
            w.f32(toss);
            w.varint(blocks);
            for (const Player& p : players) {
                w.boolean(p.present);
                if (!p.present) continue;
                w.boolean(p.alive);
                w.boolean(p.carrier);
                w.bytes(std::vector<uint8_t>(p.name.begin(), p.name.end()));
                w.varint(p.points);
                w.varint(p.place);
                w.f32(p.extra);
            }
        }
        bool read(projv::net::BitReader& r) {
            mode = Mode(std::min<uint64_t>(r.varint(), MODES - 1));
            phase = Phase(std::min<uint64_t>(r.varint(), MatchOver));
            round = uint8_t(std::min<uint64_t>(r.varint(), 255));
            rounds = uint8_t(std::min<uint64_t>(r.varint(), 255));
            clock = r.f32();
            fuse = r.f32();
            you = int8_t(std::min<uint64_t>(r.varint(), MAX_PLAYERS)) - 1;
            shove = r.f32();
            toss = r.f32();
            blocks = uint8_t(std::min<uint64_t>(r.varint(), 255));
            for (Player& p : players) {
                p = Player{};
                p.present = r.boolean();
                if (!p.present) continue;
                p.alive = r.boolean();
                p.carrier = r.boolean();
                const std::vector<uint8_t> name = r.bytes(16);
                p.name.assign(name.begin(), name.end());
                p.points = uint8_t(std::min<uint64_t>(r.varint(), 255));
                p.place = uint8_t(std::min<uint64_t>(r.varint(), MAX_PLAYERS));
                p.extra = r.f32();
            }
            return !r.failed;
        }
    };
}
