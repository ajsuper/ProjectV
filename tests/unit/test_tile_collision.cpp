// What collision reads from a tile table (utils/tile_collision.h), against a plain voxel grid: the boxes
// cover exactly the solid voxels, each of one slot; a ray stops where a voxel-by-voxel walk does; the
// surface normal is the face's.

#include "doctest/doctest.h"

#include <cmath>
#include <random>
#include <vector>

#include "utils/blob_voxels.h"
#include "utils/tile_collision.h"
#include "utils/tile_table.h"

namespace {
    using projv::core::ivec3;
    using projv::core::vec3;
    namespace utils = projv::utils;

    // A grid and the same voxels as a table: ground with hills and caves, scattered voxels and pillars, in
    // three slots -- so tables have full blocks, full bricks, uniform and mixed leaves.
    struct World {
        int res = 0;
        std::vector<int16_t> slots;   // -1 empty
        utils::TileTable table;
        int16_t at(ivec3 v) const {
            if (glm::any(glm::lessThan(v, ivec3(0))) || glm::any(glm::greaterThanEqual(v, ivec3(res)))) return -1;
            return slots[size_t(v.x + res * (v.y + res * v.z))];
        }
    };

    World makeWorld(int res, uint32_t seed) {
        World w;
        w.res = res;
        w.slots.assign(size_t(res) * res * res, -1);
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> any(0, res - 1);
        std::vector<utils::TileEdit> edits;
        auto set = [&](ivec3 v, int slot) {
            w.slots[size_t(v.x + res * (v.y + res * v.z))] = int16_t(slot);
            edits.push_back({v, true, uint8_t(slot)});
        };
        const float f = 6.2831853f / float(res);
        for (int z = 0; z < res; z++)
            for (int x = 0; x < res; x++) {
                const int height = int(float(res) * (0.3f + 0.1f * std::sin(float(x) * f * 2.0f) * std::cos(float(z) * f * 3.0f)));
                for (int y = 0; y < height; y++) {
                    const vec3 c = vec3(x, y, z) - vec3(0.5f * float(res), 0.2f * float(res), 0.4f * float(res));
                    if (glm::length(c) < 0.12f * float(res)) continue;   // a cave
                    set({x, y, z}, y < height - 3 ? 0 : (x / 8 + z / 8) % 2 + 1);
                }
            }
        for (int i = 0; i < res * 4; i++) set({any(rng), any(rng), any(rng)}, i % 3);
        for (int i = 0; i < 6; i++) {
            const int x = any(rng), z = any(rng);
            for (int y = 0; y < res; y++) set({x, y, z}, 2);
        }
        w.table.resolution = uint32_t(res);
        utils::applyTileEdits(w.table, edits);
        return w;
    }

    // The first solid voxel along the ray, one voxel at a time (Amanatides-Woo), the cube's voxels only.
    bool referenceRay(const World& w, vec3 o, vec3 d, float maxT, float& t, ivec3& voxel) {
        float t0 = 0.0f, t1 = maxT;
        for (int a = 0; a < 3; a++) {
            if (d[a] == 0.0f) { if (o[a] < 0.0f || o[a] > float(w.res)) return false; continue; }
            float n = (0.0f - o[a]) / d[a], f = (float(w.res) - o[a]) / d[a];
            if (n > f) std::swap(n, f);
            t0 = std::max(t0, n);
            t1 = std::min(t1, f);
        }
        if (t0 > t1) return false;
        ivec3 cell = glm::clamp(ivec3(glm::floor(o + d * t0)), ivec3(0), ivec3(w.res - 1));
        vec3 next, delta;
        ivec3 step;
        for (int a = 0; a < 3; a++) {
            step[a] = d[a] > 0 ? 1 : d[a] < 0 ? -1 : 0;
            next[a] = d[a] > 0 ? (float(cell[a] + 1) - o[a]) / d[a] : d[a] < 0 ? (float(cell[a]) - o[a]) / d[a] : INFINITY;
            delta[a] = d[a] != 0 ? std::abs(1.0f / d[a]) : INFINITY;
        }
        float now = t0;
        while (now <= t1) {
            if (w.at(cell) >= 0) { t = now; voxel = cell; return true; }
            const int a = next.x <= next.y ? (next.x <= next.z ? 0 : 2) : (next.y <= next.z ? 1 : 2);
            now = next[a];
            cell[a] += step[a];
            next[a] += delta[a];
            if (cell[a] < 0 || cell[a] >= w.res) return false;
        }
        return false;
    }
}

TEST_CASE("tile boxes cover exactly the solid voxels, each box one slot, and a range query finds every voxel in it") {
    for (int res : {16, 64, 256}) {
        CAPTURE(res);
        const World w = makeWorld(res, uint32_t(res));
        const utils::TileLayout L = utils::tileTableLayout(w.table);
        std::vector<utils::TileBox> boxes;
        utils::tileBoxesIn(w.table, L, ivec3(0), ivec3(res - 1), boxes);
        std::vector<uint8_t> covered(w.slots.size(), 0);
        int wrongSlot = 0, overlapping = 0, empty = 0;
        for (const utils::TileBox& b : boxes)
            for (int z = b.low.z; z <= b.high.z; z++)
                for (int y = b.low.y; y <= b.high.y; y++)
                    for (int x = b.low.x; x <= b.high.x; x++) {
                        const size_t i = size_t(x + res * (y + res * z));
                        if (covered[i]++) overlapping++;
                        if (w.slots[i] < 0) empty++;
                        else if (w.slots[i] != b.slot) wrongSlot++;
                    }
        int missed = 0;
        for (size_t i = 0; i < w.slots.size(); i++) missed += w.slots[i] >= 0 && !covered[i];
        CHECK(missed == 0);
        CHECK(empty == 0);
        CHECK(overlapping == 0);
        CHECK(wrongSlot == 0);

        // A range: every solid voxel in it is in a box returned; every box returned reaches into it.
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> any(0, res - 1);
        for (int q = 0; q < 20; q++) {
            ivec3 a(any(rng), any(rng), any(rng)), c = glm::min(a + ivec3(any(rng) % 9), ivec3(res - 1));
            std::vector<utils::TileBox> in;
            utils::tileBoxesIn(w.table, L, a, c, in);
            int outside = 0, notFound = 0;
            for (const utils::TileBox& b : in)
                outside += glm::any(glm::greaterThan(b.low, c)) || glm::any(glm::lessThan(b.high, a));
            for (int z = a.z; z <= c.z; z++)
                for (int y = a.y; y <= c.y; y++)
                    for (int x = a.x; x <= c.x; x++) {
                        if (w.at({x, y, z}) < 0) continue;
                        bool found = false;
                        for (const utils::TileBox& b : in)
                            found = found || (glm::all(glm::lessThanEqual(b.low, ivec3(x, y, z))) &&
                                              glm::all(glm::lessThanEqual(ivec3(x, y, z), b.high)));
                        notFound += !found;
                    }
            CHECK(outside == 0);
            CHECK(notFound == 0);
        }
    }
}

TEST_CASE("a ray through a tile table stops at the voxel a voxel-by-voxel walk does, entering by its face") {
    for (int res : {16, 64, 256}) {
        CAPTURE(res);
        const World w = makeWorld(res, uint32_t(res) + 1);
        const utils::TileLayout L = utils::tileTableLayout(w.table);
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        int disagree = 0, badNormal = 0, hits = 0;
        for (int i = 0; i < 3000; i++) {
            vec3 o = vec3(0.5f * float(res)) + vec3(unit(rng), unit(rng), unit(rng)) * (0.9f * float(res));
            vec3 d = vec3(unit(rng), unit(rng), unit(rng));
            if (i % 5 == 0) d = vec3(0, -1, 0);   // straight down, an axis ray
            if (glm::length(d) < 0.1f) continue;
            d = glm::normalize(d) * float(2 * res);
            float tRef = 0;
            ivec3 vRef;
            const bool ref = referenceRay(w, o, d, 1.0f, tRef, vRef);
            utils::TileRayHit hit;
            const bool got = utils::tileTableRaycast(w.table, L, o, d, 1.0f, hit);
            if (ref != got) { disagree++; continue; }
            if (!ref) continue;
            hits++;
            // The same entry distance, to a hundredth of a voxel over a few hundred (the two walks round differently).
            if (std::abs(hit.t - tRef) * glm::length(d) > 1e-2f || w.at(hit.voxel) < 0) disagree++;
            // The face it came in by is open on the outside, unless it started inside.
            if (hit.t > 0.0f && (glm::length(vec3(hit.normal)) != 1.0f || w.at(hit.voxel + hit.normal) >= 0)) badNormal++;
        }
        CHECK(hits > 500);
        CHECK(disagree == 0);
        CHECK(badNormal == 0);
    }
}

TEST_CASE("the surface normal at a point on a voxel's open face is that face's") {
    const World w = makeWorld(64, 3);
    int checked = 0, wrong = 0;
    for (int z = 0; z < 64; z++)
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                const ivec3 v(x, y, z);
                if (w.at(v) < 0) continue;
                for (int a = 0; a < 3; a++)
                    for (int s : {-1, 1}) {
                        ivec3 n(0);
                        n[a] = s;
                        if (w.at(v + n) >= 0 || glm::any(glm::lessThan(v + n, ivec3(0))) || glm::any(glm::greaterThanEqual(v + n, ivec3(64))))
                            continue;
                        vec3 p = vec3(v) + vec3(0.5f) + vec3(n) * 0.5f;
                        p += vec3(0.2f, -0.15f, 0.1f) * (vec3(1.0f) - glm::abs(vec3(n)));   // off the face's centre
                        checked++;
                        wrong += utils::tileSurfaceNormal(w.table, p) != n;
                    }
            }
    CHECK(checked > 1000);
    CHECK(wrong == 0);
}

TEST_CASE("a shared blob table stays as it was when the blob is edited, and an unshared one is edited in place") {
    // Copy-on-write (GeometryBlob::tiles): what a physics shape holds is never changed under it.
    const World w = makeWorld(16, 5);
    projv::GeometryBlob blob;
    utils::setBlobTiles(blob, w.table);
    const utils::TileTable* own = &utils::blobTiles(blob);
    utils::applyTileEdits(utils::editBlobTiles(blob, 16), {{{0, 0, 0}, false, 0}});
    CHECK(&utils::blobTiles(blob) == own);   // nothing else held it: no copy

    std::shared_ptr<const utils::TileTable> shared = utils::shareBlobTiles(blob);
    REQUIRE(shared);
    const std::vector<uint32_t> before = shared->texels;
    const uint64_t voxels = utils::tileTableVoxelCount(*shared);
    utils::applyTileEdits(utils::editBlobTiles(blob, 16), {{{5, 1, 5}, false, 0}, {{6, 1, 5}, false, 0}});
    CHECK(shared->texels == before);
    CHECK(utils::tileTableVoxelCount(*shared) == voxels);
    CHECK(utils::tileTableVoxelCount(utils::blobTiles(blob)) == voxels - 2);
    CHECK(&utils::blobTiles(blob) != shared.get());
    CHECK_FALSE(utils::shareBlobTiles(projv::GeometryBlob{}));   // empty: nothing to share
}
