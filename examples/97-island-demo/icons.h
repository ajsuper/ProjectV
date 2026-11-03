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
    inline bool write(const std::filesystem::path& path, const Picture& p) {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, SIZE, 0, SIZE, 0, 32, 0x28};
        out.write(reinterpret_cast<const char*>(header), sizeof header);
        for (const Rgba& c : p) {
            const char bgra[4] = {char(c.b), char(c.g), char(c.r), char(c.a)};
            out.write(bgra, 4);
        }
        return bool(out);
    }

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

    // Writes every icon into `folder` (made if missing). False if one could not be written.
    inline bool writeAll(const std::filesystem::path& folder) {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        bool ok = write(folder / "pickaxe.pixel.tga", tool(fx::PICKAXE_ART)) && write(folder / "chisel.pixel.tga", tool(fx::CHISEL_ART));
        for (const char* m : gather::MATERIALS) ok = write(folder / (std::string(m) + ".pixel.tga"), material(m)) && ok;
        return ok;
    }
}

#endif
