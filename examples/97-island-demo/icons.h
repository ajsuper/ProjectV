// The HUD's icons (D9): pixel art, 16 x 16, made at startup and written beside the program as
// icons/<name>.pixel.tga -- the tools from the same pictures the held ones are built of (fx.h), the
// materials as little blocks in the island's own shades. The ".pixel" in the name has the user layer
// sample them nearest, so they stay crisp at whole multiples.
#ifndef ISLAND_DEMO_ICONS_H
#define ISLAND_DEMO_ICONS_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "build.h"
#include "fx.h"
#include "gather.h"
#include "island.h"

namespace icons {
    constexpr int SIZE = 16;

    struct Rgba { uint8_t r = 0, g = 0, b = 0, a = 0; };
    using Picture = std::array<Rgba, SIZE * SIZE>;

    inline Rgba opaque(int r, int g, int b) { return {uint8_t(std::clamp(r, 0, 255)), uint8_t(std::clamp(g, 0, 255)), uint8_t(std::clamp(b, 0, 255)), 255}; }
    inline Rgba shade(island::Slot slot, float light = 1.0f) {
        const island::Shade& s = island::PALETTE[slot];
        return opaque(int(float(s.r) / 1023.0f * 255.0f * light), int(float(s.g) / 1023.0f * 255.0f * light), int(float(s.b) / 1023.0f * 255.0f * light));
    }
    inline Rgba scaled(Rgba c, float light) { return opaque(int(c.r * light), int(c.g * light), int(c.b * light)); }

    // Uncompressed 32-bit TGA, top row first: the smallest thing the user layer reads.
    inline bool write(const std::filesystem::path& path, const Rgba* pixels, int width, int height) {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(width), uint8_t(width >> 8),
                                    uint8_t(height), uint8_t(height >> 8), 32, 0x28};
        out.write(reinterpret_cast<const char*>(header), sizeof header);
        for (int i = 0; i < width * height; i++) {
            const char bgra[4] = {char(pixels[i].b), char(pixels[i].g), char(pixels[i].r), char(pixels[i].a)};
            out.write(bgra, 4);
        }
        return bool(out);
    }
    inline bool write(const std::filesystem::path& path, const Picture& p) { return write(path, p.data(), SIZE, SIZE); }

    inline Picture tool(const char* const art[16]) {
        Picture p{};
        for (int row = 0; row < SIZE; row++)
            for (int col = 0; col < SIZE; col++)
                if (const fx::Ink* ink = fx::inkOf(art[row][col])) p[row * SIZE + col] = opaque(ink->r, ink->g, ink->b);
        return p;
    }

    // A block of a material: an outline in its darkest, a lit top, shaded right and bottom edges, its face a
    // scatter of the island's shades of it, and a mark of its own (grass over dirt, planks, spots...).
    inline Picture material(const std::string& name) {
        using namespace island;
        Picture p{};
        auto at = [&](int x, int y) -> Rgba& { return p[y * SIZE + x]; };
        auto noise = [](int x, int y, uint32_t salt) { return island::unit(island::hash(x, y, salt)); };
        const std::vector<uint8_t> slots = build::shadesOf(name);
        auto face = [&](int x, int y) { return shade(island::Slot(slots[size_t(noise(x, y, 7u) * float(slots.size())) % slots.size()])); };

        for (int y = 1; y < SIZE - 1; y++)
            for (int x = 1; x < SIZE - 1; x++) {
                Rgba c = face(x, y);
                if (name == "grass") {
                    // Green over dirt, ragged where they meet.
                    const int edge = 5 + int(noise(x, 0, 3u) * 3.0f);
                    c = y < edge ? shade(Slot(Grass0 + int(noise(x, y, 5u) * 3.0f) % 3)) : shade(noise(x, y, 6u) < 0.5f ? Dirt0 : Dirt1);
                } else if (name == "wood") {
                    // Planks: seams every four rows, a knot or two, grain along them.
                    c = shade(Wood, 0.92f + 0.16f * noise(x / 3, y, 11u));
                    if ((y - 1) % 4 == 3) c = shade(Bark, 0.85f);
                    if ((x == 4 && y < 5) || (x == 11 && y >= 5 && y < 9) || (x == 6 && y >= 9 && y < 13)) c = shade(Bark, 0.9f);
                } else if (name == "leaves") {
                    if (noise(x, y, 13u) < 0.14f) c = shade(Leaves2, 0.55f);
                    else if (noise(x, y, 14u) < 0.12f) c = shade(Leaves1, 1.3f);
                } else if (name == "mushroom") {
                    c = shade(CapRed);
                    const int spots[][2] = {{4, 4}, {10, 3}, {7, 8}, {12, 10}, {3, 11}};
                    for (const auto& s : spots)
                        if (std::abs(x - s[0]) + std::abs(y - s[1]) <= 1) c = shade(CapSpot);
                } else if (name == "stone") {
                    // A crack or two across the face.
                    const bool crack = (y == 5 && x > 2 && x < 8) || (x == 8 && y > 5 && y < 9) || (y == 11 && x > 8 && x < 13);
                    if (crack) c = shade(StoneDark, 0.8f);
                } else if (name == "dirt") {
                    if (noise(x, y, 17u) < 0.06f) c = shade(Pebble0);
                }
                // Lit from the top left.
                if (y == 1 || x == 1) c = scaled(c, 1.25f);
                if (y == SIZE - 2 || x == SIZE - 2) c = scaled(c, 0.7f);
                at(x, y) = c;
            }
        // The outline, corners cut so it reads round.
        const Rgba line = scaled(face(0, 0), 0.32f);
        for (int i = 1; i < SIZE - 1; i++) at(i, 0) = at(i, SIZE - 1) = at(0, i) = at(SIZE - 1, i) = line;
        if (name == "leaves")   // a ragged edge
            for (int i = 2; i < SIZE - 2; i += 3) at(i, 0) = at(SIZE - 1, i) = Rgba{};
        return p;
    }

    // The menus' boards (start.rml, pause.rml, graphics.rml): planks in a beam frame with iron corners, vines
    // climbing over the top and hanging down its sides. Drawn at one art pixel a dp, so a document sizes its
    // panel to its board exactly.
    constexpr int BOARD_FRAME = 8;
    struct BoardSize { const char* name; int width, height; };
    constexpr BoardSize BOARDS[] = {{"board", 208, 264}, {"board-pause", 184, 156}, {"board-graphics", 244, 272}};

    inline std::vector<Rgba> board(int W, int H) {
        const int F = BOARD_FRAME;
        std::vector<Rgba> p(size_t(W) * H);
        auto at = [&](int x, int y) -> Rgba* { return x >= 0 && y >= 0 && x < W && y < H ? &p[size_t(y) * W + x] : nullptr; };
        auto put = [&](int x, int y, Rgba c) { if (Rgba* q = at(x, y)) *q = c; };
        auto noise = [](int x, int y, uint32_t salt) { return island::unit(island::hash(x, y, salt)); };

        // The planks: rows of 11, each its own shade, grain in streaks along it, a knot now and then.
        for (int y = F; y < H - F; y++) {
            const int plank = (y - F) / 12, row = (y - F) % 12;
            const float tone = 0.85f + 0.25f * noise(plank, 0, 21u);
            for (int x = F; x < W - F; x++) {
                const int shift = int(noise(plank, 1, 22u) * 60.0f);   // where the plank's butt joint is
                Rgba c = opaque(int(118 * tone), int(78 * tone), int(46 * tone));
                if (noise((x + shift) / 7, y, 23u) < 0.22f) c = scaled(c, 0.82f);     // grain
                if (row == 0) c = scaled(c, 1.18f);                                     // lit edge
                if (row == 11 || (x - F + shift * 2) % 97 == 0) c = opaque(46, 28, 16);   // seam, joint
                put(x, y, c);
            }
            if (row == 5 && noise(plank, 2, 24u) < 0.6f) {   // a knot
                const int kx = F + 10 + int(noise(plank, 3, 25u) * float(W - 2 * F - 20));
                for (int dx = -2; dx <= 2; dx++) put(kx + dx, y, opaque(70, 44, 26));
                put(kx - 1, y - 1, opaque(84, 54, 32)); put(kx + 1, y + 1, opaque(84, 54, 32));
            }
        }
        // The frame: a beam round it, lit on its top and left, an outline outside and a shadow inside.
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const int d = std::min(std::min(x, y), std::min(W - 1 - x, H - 1 - y));
                if (d >= F) continue;
                Rgba c = opaque(92, 58, 34);
                if (noise(x / 5 + y, x + y / 5, 26u) < 0.25f) c = opaque(78, 48, 28);
                if (d == 1) c = scaled(c, x < W / 2 && y < H / 2 ? 1.35f : 1.15f);
                if (d == F - 2) c = scaled(c, 0.75f);
                if (d == 0 || d == F - 1) c = opaque(26, 15, 8);
                put(x, y, c);
            }
        // Iron at the corners, a rivet in each.
        for (int cy : {0, H - 12})
            for (int cx : {0, W - 12})
                for (int y = 0; y < 12; y++)
                    for (int x = 0; x < 12; x++) {
                        const bool edge = x == 0 || y == 0 || x == 11 || y == 11;
                        Rgba c = edge ? opaque(28, 28, 32) : (x == 1 || y == 1 ? opaque(150, 152, 160) : opaque(98, 100, 110));
                        if ((x == 5 || x == 6) && (y == 5 || y == 6)) c = x == 5 && y == 5 ? opaque(190, 192, 200) : opaque(40, 40, 46);
                        put(cx + x, cy + y, c);
                    }

        // Vines. A leaf is a little lozenge in two greens, outlined dark on its underside.
        const Rgba stem = opaque(44, 86, 36), light = opaque(120, 184, 76), mid = opaque(78, 140, 54), dark = opaque(30, 62, 28);
        auto leaf = [&](int x, int y, int side) {
            put(x + side, y - 1, light); put(x + 2 * side, y - 1, light);
            put(x + side, y, mid); put(x + 2 * side, y, light); put(x + 3 * side, y, mid);
            put(x + side, y + 1, dark); put(x + 2 * side, y + 1, mid); put(x + 3 * side, y + 1, dark);
            put(x + 2 * side, y + 2, dark);
        };
        auto flower = [&](int x, int y, uint32_t h) {
            const Rgba petal = h % 2 ? opaque(246, 240, 228) : opaque(240, 150, 190);
            put(x - 1, y, petal); put(x + 1, y, petal); put(x, y - 1, petal); put(x, y + 1, petal); put(x, y, opaque(250, 210, 70));
        };
        uint32_t seed = 1;
        // Along the top beam, wandering over it.
        for (int x = 2, y = 3; x < W - 2; x++) {
            if (noise(x, 0, 30u) < 0.3f) y = std::clamp(y + (noise(x, 1, 31u) < 0.5f ? -1 : 1), 2, 6);
            put(x, y, stem);
            if (x % 4 == 0) leaf(x, y + (x % 8 ? 1 : 0), x % 8 ? 1 : -1);
            if (x % 61 == 23) flower(x, y + 2, island::hash(seed++));
        }
        // Hanging: long ones down the sides (clear of the writing), short ones over the top.
        struct Hang { int x, length; };
        const Hang hangs[] = {{5, 150}, {14, 90}, {22, 46}, {34, 18}, {58, 9}, {83, 14}, {120, 7}, {146, 12},
                              {170, 22}, {186, 60}, {195, 120}, {203, 70}};
        // Laid out for a 208 x 264 board: the sides' vines keep to their edge and hang as far down in
        // proportion, the top's spread across it.
        for (Hang h : hangs) {
            if (h.x < 40) h.length = h.length * H / 264;
            else if (h.x > 168) { h.x = W - (208 - h.x); h.length = h.length * H / 264; }
            else h.x = h.x * W / 208;
            int x = h.x;
            for (int i = 0; i < h.length; i++) {
                const int y = 4 + i;
                if (i > 0 && noise(h.x, i, 32u) < 0.18f) x += noise(h.x, i, 33u) < 0.5f ? -1 : 1;
                x = std::clamp(x, h.x - 4, h.x + 4);
                put(x, y, stem);
                if (i > 1 && i % 3 == 0) leaf(x, y, (i / 3) % 2 ? 1 : -1);
                if (i > 6 && noise(h.x, i, 34u) < 0.012f) flower(x + 2, y, island::hash(seed++));
            }
            put(x, 4 + h.length, light);   // a bud at the end
        }
        return p;
    }

    // Writes every icon into `folder` (made if missing). False if one could not be written.
    inline bool writeAll(const std::filesystem::path& folder) {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        bool ok = write(folder / "pickaxe.pixel.tga", tool(fx::PICKAXE_ART)) && write(folder / "chisel.pixel.tga", tool(fx::CHISEL_ART));
        for (const char* m : gather::MATERIALS) ok = write(folder / (std::string(m) + ".pixel.tga"), material(m)) && ok;
        for (const BoardSize& b : BOARDS)
            ok = write(folder / (std::string(b.name) + ".pixel.tga"), board(b.width, b.height).data(), b.width, b.height) && ok;
        return ok;
    }
}

#endif
