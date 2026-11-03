// The island: a seed in, a world folder out (docs/plans/island-demo.md, D1-D4).
//
// A pure function of world position and the seed, so the cells can be made on any thread in any
// order and still meet at their seams: an fBm heightfield under a radial falloff into a shallow sea,
// beaches round it, rock where it is steep or high, grass over dirt elsewhere, and trees -- a trunk
// and a crown of leaves, each one different -- in clumps away from the shore and the rock.
//
// It writes the world as the engine's own scene, one grid of 64^3 cells at 0.25 m (compose.json and
// island.data), which is also what a save is (D11): a new world and a loaded one go the same way in.
#ifndef ISLAND_DEMO_ISLAND_H
#define ISLAND_DEMO_ISLAND_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/math.h"
#include "data_structures/compose.h"
#include "utils/compose_io.h"
#include "utils/material.h"
#include "utils/voxel_management.h"

namespace island {
    using projv::core::vec3;

    constexpr int   RES = 64;                 // voxels per cell edge
    constexpr float VOXEL = 0.25f;            // metres
    constexpr int   CELLS = 12;               // across (12 x 64 x 0.25 = 192 m)
    constexpr int   LAYERS = 4;               // high (64 m)
    constexpr float SIZE = CELLS * RES * VOXEL;
    constexpr float HEIGHT = LAYERS * RES * VOXEL;
    constexpr float CENTRE = SIZE * 0.5f;
    constexpr float SEA = 6.0f;               // the sea's surface, m
    constexpr float RADIUS = 82.0f;           // where the island meets the sea, on average

    // The palette's slots: a few shades of each material, each naming its physics material (D4).
    enum Slot : uint8_t {
        Grass0, Grass1, Grass2, Dirt, Sand0, Sand1, Stone0, Stone1, Stone2, Bark, Wood, Leaves0, Leaves1, Leaves2, Water,
        SLOTS
    };
    struct Shade { const char* name; int r, g, b; const char* physics; };
    constexpr Shade PALETTE[SLOTS] = {
        {"grass", 310, 560, 210, "grass"}, {"grass", 350, 600, 230, "grass"}, {"grass", 280, 520, 190, "grass"},
        {"dirt", 450, 320, 200, "dirt"},
        {"sand", 860, 790, 590, "sand"}, {"sand", 820, 750, 560, "sand"},
        {"stone", 500, 500, 520, "stone"}, {"stone", 440, 440, 460, "stone"}, {"stone", 560, 550, 560, "stone"},
        {"wood", 400, 280, 180, "wood"}, {"wood", 560, 420, 280, "wood"},
        {"leaves", 220, 470, 170, "leaves"}, {"leaves", 260, 520, 190, "leaves"}, {"leaves", 190, 420, 150, "leaves"},
        {"water", 180, 380, 560, "water"},
    };

    // ---- Noise: gradient noise from a hash of the seed and the lattice point -----------------------

    inline uint32_t hash(uint32_t x) {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return x;
    }
    inline uint32_t hash(int x, int z, uint32_t seed) { return hash(uint32_t(x) * 0x9e3779b1u ^ hash(uint32_t(z) + seed * 0x85ebca77u)); }
    inline float unit(uint32_t h) { return float(h & 0xffffffu) / float(0x1000000); }   // [0, 1)

    inline float gradientNoise(float x, float z, uint32_t seed) {
        const int ix = int(std::floor(x)), iz = int(std::floor(z));
        const float fx = x - float(ix), fz = z - float(iz);
        auto corner = [&](int cx, int cz) {
            const float a = unit(hash(ix + cx, iz + cz, seed)) * 6.2831853f;
            return std::cos(a) * (fx - float(cx)) + std::sin(a) * (fz - float(cz));
        };
        const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f), uz = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);
        const float a = corner(0, 0) + (corner(1, 0) - corner(0, 0)) * ux;
        const float b = corner(0, 1) + (corner(1, 1) - corner(0, 1)) * ux;
        return a + (b - a) * uz;   // about [-0.7, 0.7]
    }
    inline float fbm(float x, float z, uint32_t seed, int octaves) {
        float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, total = 0.0f;
        for (int i = 0; i < octaves; i++) {
            sum += gradientNoise(x * frequency, z * frequency, seed + uint32_t(i) * 101u) * amplitude;
            total += amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }
        return sum / total;
    }
    inline float smoothstep(float a, float b, float x) {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // ---- The land ----------------------------------------------------------------------------------

    // The ground's height at (x, z), in metres: hills under a falloff into the sea; the shore wavers.
    inline float heightAt(float x, float z, uint32_t seed) {
        const float dx = x - CENTRE, dz = z - CENTRE;
        const float shore = RADIUS * (1.0f + 0.22f * fbm(x * 0.012f, z * 0.012f, seed + 7u, 3));
        const float r = std::sqrt(dx * dx + dz * dz) / shore;
        const float land = 1.0f - smoothstep(0.55f, 1.05f, r);              // 1 inland, 0 out at sea
        const float hills = 0.5f + 0.5f * fbm(x * 0.008f, z * 0.008f, seed, 5);
        const float peaks = std::pow(std::max(0.0f, fbm(x * 0.017f, z * 0.017f, seed + 31u, 4)), 1.4f);
        const float h = SEA - 3.0f + land * (5.0f + 26.0f * hills * hills + 38.0f * peaks * land);
        return std::clamp(h, 1.0f, HEIGHT - 6.0f);
    }

    struct Column {
        float surface = 0.0f;   // the ground's top
        float slope = 0.0f;     // rise over run
        bool  rock = false, sand = false;
    };
    inline Column columnAt(float x, float z, uint32_t seed) {
        Column c;
        c.surface = heightAt(x, z, seed);
        const float e = 0.75f;
        c.slope = std::max(std::abs(heightAt(x + e, z, seed) - heightAt(x - e, z, seed)),
                           std::abs(heightAt(x, z + e, seed) - heightAt(x, z - e, seed))) / (2.0f * e);
        const float rockiness = fbm(x * 0.05f, z * 0.05f, seed + 53u, 2);
        c.rock = c.slope > 0.85f - 0.25f * rockiness || c.surface > 30.0f + 6.0f * rockiness;
        c.sand = !c.rock && c.surface < SEA + 1.4f + 0.8f * rockiness;
        return c;
    }

    // ---- Trees ---------------------------------------------------------------------------------------

    struct Tree {
        vec3  base;          // the trunk's foot, on the ground
        float height;        // trunk, m
        float trunk;         // trunk radius, m
        float crown;         // crown radius, m
        vec3  lean;          // the top's offset from the foot
        uint32_t seed;
    };
    constexpr float TREE_SPACING = 7.0f;   // one candidate per square this size

    // The trees whose voxels can reach the box [lo, hi]: candidates on a jittered lattice, kept where the
    // forest is thick and the ground is grass and gentle.
    inline std::vector<Tree> treesNear(vec3 lo, vec3 hi, uint32_t seed) {
        std::vector<Tree> out;
        const float reach = 5.0f;   // the widest crown and lean
        const int x0 = int(std::floor((lo.x - reach) / TREE_SPACING)), x1 = int(std::floor((hi.x + reach) / TREE_SPACING));
        const int z0 = int(std::floor((lo.z - reach) / TREE_SPACING)), z1 = int(std::floor((hi.z + reach) / TREE_SPACING));
        for (int gz = z0; gz <= z1; gz++)
            for (int gx = x0; gx <= x1; gx++) {
                const uint32_t h = hash(gx, gz, seed + 977u);
                const float x = (float(gx) + 0.15f + 0.7f * unit(h)) * TREE_SPACING;
                const float z = (float(gz) + 0.15f + 0.7f * unit(hash(h))) * TREE_SPACING;
                const float forest = 0.5f + 0.5f * fbm(x * 0.02f, z * 0.02f, seed + 211u, 3);
                if (unit(hash(h + 1u)) > forest * forest * 1.6f - 0.15f) continue;
                const Column c = columnAt(x, z, seed);
                if (c.rock || c.sand || c.slope > 0.45f || c.surface < SEA + 2.0f) continue;
                Tree t;
                t.base = vec3(x, c.surface - 0.5f, z);
                t.height = 3.5f + 3.5f * unit(hash(h + 2u));
                t.trunk = 0.26f + 0.16f * unit(hash(h + 3u));
                t.crown = 1.7f + 1.3f * unit(hash(h + 4u));
                const float a = unit(hash(h + 5u)) * 6.2831853f, l = 0.6f * unit(hash(h + 6u));
                t.lean = vec3(std::cos(a) * l, 0.0f, std::sin(a) * l);
                t.seed = h;
                out.push_back(t);
            }
        return out;
    }

    // What a tree puts at p, or SLOTS for nothing: its trunk (bark outside, wood in), its crown (leaves, a
    // ragged ellipsoid with holes).
    inline uint8_t treeAt(const Tree& t, vec3 p) {
        const float along = (p.y - t.base.y) / t.height;
        if (along >= 0.0f && along <= 1.0f) {
            const vec3 axis = t.base + t.lean * along;
            const float dx = p.x - axis.x, dz = p.z - axis.z;
            const float r = std::sqrt(dx * dx + dz * dz), radius = t.trunk * (1.0f - 0.35f * along);
            if (r < radius) return r < radius - 0.2f ? uint8_t(Wood) : uint8_t(Bark);
        }
        const vec3 centre = t.base + t.lean + vec3(0.0f, t.height + t.crown * 0.25f, 0.0f);
        const vec3 d = p - centre;
        const float e = std::sqrt(d.x * d.x + d.y * d.y * 1.6f + d.z * d.z) / t.crown;
        if (e > 1.0f) return SLOTS;
        const uint32_t h = hash(int(std::floor(p.x * 4.0f)) * 73 + int(std::floor(p.y * 4.0f)) * 151, int(std::floor(p.z * 4.0f)), t.seed);
        if (e > 0.75f && unit(h) < (e - 0.75f) * 3.0f) return SLOTS;   // ragged edge
        if (unit(hash(h)) < 0.08f) return SLOTS;                       // a few holes
        return uint8_t(Leaves0 + hash(h + 1u) % 3u);
    }

    // ---- The voxels ---------------------------------------------------------------------------------

    inline uint8_t groundAt(const Column& c, float y, float x, float z, uint32_t seed) {
        const float depth = c.surface - y;
        const uint32_t h = hash(int(std::floor(x * 4.0f)) * 31 + int(std::floor(y * 4.0f)), int(std::floor(z * 4.0f)), seed + 5u);
        if (c.rock || depth > 2.5f + 1.5f * unit(h >> 8)) return uint8_t(Stone0 + h % 3u);
        if (c.sand) return uint8_t(Sand0 + h % 2u);
        if (depth < 0.3f) return uint8_t(Grass0 + h % 3u);
        return Dirt;
    }

    // One cell, filled into `block`; false when it is empty. The ground and the sea by columns, then each
    // tree stamped over its own box: testing every tree at every voxel of air costs a hundred times more.
    inline bool makeCell(int cx, int cy, int cz, uint32_t seed, projv::DataBlock& block) {
        const vec3 lo(float(cx * RES) * VOXEL, float(cy * RES) * VOXEL, float(cz * RES) * VOXEL);
        const vec3 hi = lo + vec3(RES * VOXEL);
        auto map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(RES));
        bool any = false;
        for (int z = 0; z < RES; z++)
            for (int x = 0; x < RES; x++) {
                const float wx = lo.x + (float(x) + 0.5f) * VOXEL, wz = lo.z + (float(z) + 0.5f) * VOXEL;
                const Column c = columnAt(wx, wz, seed);
                const int top = std::min(RES, int(std::ceil((std::max(c.surface, SEA) - lo.y) / VOXEL)));
                for (int y = 0; y < top; y++) {
                    const float wy = lo.y + (float(y) + 0.5f) * VOXEL;
                    uint8_t slot = SLOTS;
                    if (wy < c.surface) slot = groundAt(c, wy, wx, wz, seed);
                    else if (wy < SEA && wy >= SEA - VOXEL) slot = Water;   // the sea: one voxel of it, at its surface
                    if (slot == SLOTS) continue;
                    projv::utils::brickMapSetVoxel(*map, x, y, z, slot);
                    any = true;
                }
            }
        for (const Tree& t : treesNear(lo, hi, seed)) {
            const float reach = std::max(t.crown, t.trunk) + 0.7f;
            const vec3 top = t.base + t.lean + vec3(0.0f, t.height + t.crown * 1.1f, 0.0f);
            const vec3 boxLo(std::min(t.base.x, top.x) - reach, t.base.y, std::min(t.base.z, top.z) - reach);
            const vec3 boxHi(std::max(t.base.x, top.x) + reach, top.y, std::max(t.base.z, top.z) + reach);
            auto index = [&](float w, float origin) { return int(std::floor((w - origin) / VOXEL)); };
            const int x0 = std::max(0, index(boxLo.x, lo.x)), x1 = std::min(RES - 1, index(boxHi.x, lo.x));
            const int y0 = std::max(0, index(boxLo.y, lo.y)), y1 = std::min(RES - 1, index(boxHi.y, lo.y));
            const int z0 = std::max(0, index(boxLo.z, lo.z)), z1 = std::min(RES - 1, index(boxHi.z, lo.z));
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) {
                        const vec3 p(lo.x + (float(x) + 0.5f) * VOXEL, lo.y + (float(y) + 0.5f) * VOXEL, lo.z + (float(z) + 0.5f) * VOXEL);
                        const uint8_t slot = treeAt(t, p);
                        if (slot == SLOTS) continue;
                        projv::utils::brickMapSetVoxel(*map, x, y, z, slot);
                        any = true;
                    }
        }
        if (!any) return false;
        block.gridX = cx;
        block.gridY = cy;
        block.gridZ = cz;
        block.geometry = projv::utils::buildTree64FromBrickMap(*map, RES);
        projv::utils::bakeMaterialsFromBrickMap(block.geometry, block.materialIDs, *map);
        return true;
    }

    inline std::string paletteJson() {
        std::string out;
        for (int i = 0; i < SLOTS; i++) {
            const Shade& s = PALETTE[i];
            out += std::string(i ? ",\n" : "") + "        { \"name\": \"" + s.name + "\", \"color\": [" + std::to_string(s.r) + ", " +
                   std::to_string(s.g) + ", " + std::to_string(s.b) + "], \"physics\": \"" + s.physics + "\"" +
                   (i == Water ? ", \"glossiness\": 0.9" : "") + " }";
        }
        return out;
    }

    // Makes the island for `seed` and writes it to `folder` (compose.json, island.data), on every core
    // but one. `progress`, if given, counts cells done (of CELLS * LAYERS * CELLS).
    inline bool generate(const std::filesystem::path& folder, uint32_t seed, std::atomic<int>* progress = nullptr) {
        const int total = CELLS * LAYERS * CELLS;
        std::vector<projv::DataBlock> blocks(static_cast<size_t>(total));
        std::vector<char> filled(static_cast<size_t>(total), 0);
        std::atomic<int> next{0};
        auto work = [&] {
            for (int i; (i = next.fetch_add(1)) < total;) {
                const int cx = i % CELLS, cy = (i / CELLS) % LAYERS, cz = i / (CELLS * LAYERS);
                filled[size_t(i)] = makeCell(cx, cy, cz, seed, blocks[size_t(i)]) ? 1 : 0;
                if (progress) progress->fetch_add(1);
            }
        };
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency() - 1u);
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++) pool.emplace_back(work);
        for (std::thread& t : pool) t.join();

        projv::DataFile file;
        file.resolution = RES;
        file.voxelScale = VOXEL;
        for (int i = 0; i < total; i++)
            if (filled[size_t(i)]) file.blocks.push_back(std::move(blocks[size_t(i)]));
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (!projv::utils::writeDataFile((folder / "island.data").string(), file)) return false;
        std::ofstream(folder / "compose.json") << "{\n    \"version\": 1,\n    \"name\": \"island\",\n    \"components\": [\n"
                                                  "      { \"type\": \"data\", \"source\": \"island.data\", \"id\": 1, \"name\": \"island\",\n"
                                                  "        \"palette\": [\n" << paletteJson() << "\n        ] }\n    ]\n}\n";
        projv::core::info("island: seed {}: {} cells of {} written to {}", seed, file.blocks.size(), total, folder.string());
        return true;
    }

    // Where to start: the highest grass near the middle, clear of trees above it.
    inline vec3 spawnPoint(uint32_t seed) {
        vec3 best(CENTRE, heightAt(CENTRE, CENTRE, seed) + 2.0f, CENTRE);
        float bestScore = -1e9f;
        for (int i = 0; i < 400; i++) {
            const float a = unit(hash(i, 1, seed)) * 6.2831853f, r = 40.0f * std::sqrt(unit(hash(i, 2, seed)));
            const float x = CENTRE + std::cos(a) * r, z = CENTRE + std::sin(a) * r;
            const Column c = columnAt(x, z, seed);
            if (c.rock || c.sand || c.slope > 0.4f) continue;
            if (!treesNear(vec3(x - 1.0f, 0.0f, z - 1.0f), vec3(x + 1.0f, HEIGHT, z + 1.0f), seed).empty()) continue;
            const float score = -r * 0.2f + c.surface * 0.05f;
            if (score > bestScore) {
                bestScore = score;
                best = vec3(x, c.surface + 1.5f, z);
            }
        }
        return best;
    }
}

#endif
