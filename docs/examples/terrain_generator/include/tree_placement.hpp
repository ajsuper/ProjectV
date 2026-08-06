#ifndef TREE_PLACEMENT_HPP
#define TREE_PLACEMENT_HPP

// Scattering pre-voxelized tree assets directly into the terrain's own voxel grid.
//
// The trees are not scene components. They are stamped into the same VoxelBrickMap the heightfield
// writes into, during chunk generation, so a tree is terrain: it lands in the terrain component's
// tree64, uses the terrain component's material palette, and is edited/streamed/LOD'd by exactly
// the machinery that already handles the ground. Nothing downstream knows a tree is a tree.
//
// The one hard requirement that follows from that is determinism. A tree is 64 voxels across, wider
// than nothing and narrower than a chunk, so a single tree routinely straddles a chunk boundary --
// and neighbouring chunks are generated independently, on different threads, in any order, possibly
// at different LODs. Placement is therefore a pure function of world position: every chunk hashes
// the same grid of candidate cells over the region its voxels could possibly be touched by, and
// arrives at the same trees. Nothing is stored between chunks and no chunk can see its neighbours.

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/math.h"
#include "data_structures/voxel.h"
#include "utils/compose_io.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

namespace trees {

// One solid voxel of a tree asset, in the asset's own local voxel space.
struct TreeVoxel {
    uint8_t x = 0, y = 0, z = 0;
    uint8_t materialID = 0;  // filled by resolveMaterials()
};

// Where a species grows. Temperature and humidity are the terrain's own climate fields (both
// roughly 0..1), so a species is described by the climate it prefers and how fussy it is about it.
// A species is picked for a spot in proportion to how well the local climate matches, which is what
// makes a region come out as one recognisable forest type rather than a fruit salad of all eight.
struct TreeSpecies {
    std::string name;
    float tempCenter = 0.5f, tempWidth = 0.30f;
    float humidCenter = 0.6f, humidWidth = 0.30f;
    float abundance = 1.0f;  // relative weight at its own climate optimum
};

// One resolution level of a tree asset. mips[0] is the asset as voxelized; each further level is
// 4x coarser per axis, which is exactly the step between the terrain's LOD rings.
//
// Precomputing these rather than decimating on the fly buys two things. It is far cheaper -- a
// distant chunk iterates the ~60 voxels of the coarse mip instead of walking all ~37,000 of the
// full-resolution tree only to collapse them onto the same 60 -- and it is the only way to get the
// colour right: collapsing on the fly leaves whichever voxel happened to be written last, so a
// distant tree could come out trunk-brown. A mip averages the voxels that merge, which is what a
// texture mip does and what the eye expects at that distance.
struct TreeMip {
    int divisor = 1;                     // asset voxels per mip voxel
    std::vector<TreeVoxel> voxels;
    std::vector<uint32_t> sourceColors;  // parallel to voxels until resolveMaterials() runs
    projv::core::ivec3 lo{0, 0, 0};      // occupied bounds in this mip's own coords, inclusive
    projv::core::ivec3 hi{0, 0, 0};
    int trunkX = 0, trunkZ = 0;          // trunk axis in this mip's coords; the stamp centres here
};

struct TreeAsset {
    std::string name;
    TreeSpecies climate;
    std::vector<TreeMip> mips;           // ascending divisor: 1, 4, 16, ...
    projv::core::ivec3 lo{0, 0, 0};      // full-resolution occupied bounds, inclusive
    projv::core::ivec3 hi{0, 0, 0};
    int trunkX = 0, trunkZ = 0;          // full-resolution trunk axis

    const TreeMip& mipForDivisor(int decimation) const {
        size_t best = 0;
        for (size_t i = 0; i < mips.size(); ++i)
            if (mips[i].divisor <= decimation) best = i;
        return mips[best];
    }
};

// What the placer needs to know about the ground under a candidate tree. Comes straight from the
// terrain's own sample, so trees agree with the ground they stand on by construction.
struct GroundSample {
    float height = 0.0f;
    float temp = 0.5f;
    float humid = 0.5f;
    // The water surface at THIS column, which is sea level almost everywhere but a river reach's
    // pool level inside a valley. Params::waterLevel is only the global floor of it, and testing
    // against that alone plants trees standing in rivers -- a river bed several hundred units above
    // sea level clears the global test comfortably.
    float waterTop = 0.0f;
};

struct TreeLibrary {
    std::vector<TreeAsset> assets;
    bool empty() const { return assets.empty(); }
};

// Collapses a tree by `divisor` per axis, averaging the colours of every voxel that merges into a
// cell. Occupancy is a plain OR: a cell survives if anything at all was in it, so a coarse tree
// keeps its silhouette instead of dissolving into holes the way point-sampling would leave it.
inline void buildMips(TreeAsset& asset, int levels) {
    // Reserve before taking `base`: each level push_backs into the same vector, and a reallocation
    // partway through would leave that reference dangling.
    asset.mips.reserve(size_t(levels > 1 ? levels : 1));
    const TreeMip& base = asset.mips[0];
    for (int level = 1; level < levels; ++level) {
        int divisor = 1;
        for (int i = 0; i < level; ++i) divisor *= 4;

        std::unordered_map<uint32_t, std::array<uint64_t, 4>> cells;  // key -> {n, sumR, sumG, sumB}
        for (size_t i = 0; i < base.voxels.size(); ++i) {
            const TreeVoxel& v = base.voxels[i];
            uint32_t cx = uint32_t(v.x) / uint32_t(divisor);
            uint32_t cy = uint32_t(v.y) / uint32_t(divisor);
            uint32_t cz = uint32_t(v.z) / uint32_t(divisor);
            uint32_t key = (cx << 20) | (cy << 10) | cz;
            projv::Color c = projv::unpackColor(base.sourceColors[i]);
            auto& acc = cells[key];
            acc[0] += 1; acc[1] += c.r; acc[2] += c.g; acc[3] += c.b;
        }

        TreeMip mip;
        mip.divisor = divisor;
        mip.lo = projv::core::ivec3(1 << 20);
        mip.hi = projv::core::ivec3(-(1 << 20));
        mip.voxels.reserve(cells.size());
        mip.sourceColors.reserve(cells.size());
        for (const auto& kv : cells) {
            uint32_t key = kv.first;
            const auto& acc = kv.second;
            projv::core::ivec3 p(int((key >> 20) & 0x3FF), int((key >> 10) & 0x3FF), int(key & 0x3FF));
            TreeVoxel v;
            v.x = uint8_t(p.x); v.y = uint8_t(p.y); v.z = uint8_t(p.z);
            mip.voxels.push_back(v);
            mip.sourceColors.push_back(projv::packColor({uint8_t(acc[1] / acc[0]),
                                                         uint8_t(acc[2] / acc[0]),
                                                         uint8_t(acc[3] / acc[0])}));
            mip.lo = projv::core::min(mip.lo, p);
            mip.hi = projv::core::max(mip.hi, p);
        }
        // The trunk anchor has to come down with the geometry, or a coarse tree would pivot around
        // a point that no longer sits under it and slide off the spot it was planted on.
        mip.trunkX = asset.trunkX / divisor;
        mip.trunkZ = asset.trunkZ / divisor;
        asset.mips.push_back(std::move(mip));
    }
}

// Reads the .data containers the MeshVoxelizer example produces (one 64^3 block per tree), decodes
// their geometry into flat voxel lists, and builds the coarse mips. Colors are kept raw here;
// resolveMaterials() below is what turns them into palette slots, because that has to happen
// against a live Scene.
//
// A .data stores geometry plus one material byte per voxel, and the palette those bytes index sits
// in the folder's compose.json -- so both files are read, and the tree64 is walked back into a brick
// map to recover the voxel positions the container does not store explicitly.
inline bool loadTreeLibrary(TreeLibrary& lib, const std::string& folder,
                            const std::vector<TreeSpecies>& species, int mipLevels = 3) {
    for (const TreeSpecies& spec : species) {
        const std::string& name = spec.name;
        std::string path = folder + "/" + name + "/model.data";
        projv::DataFile data = projv::utils::readDataFile(path);
        if (data.blocks.empty() || data.resolution == 0) {
            projv::core::warn("[TREES] could not read {} - skipping", path);
            continue;
        }

        // What the material bytes name. One `data` component per tree, so the first entry's is it.
        std::vector<projv::Material> palette;
        projv::ComposeDoc doc = projv::utils::parseComposeJson(folder + "/" + name + "/compose.json");
        if (!doc.components.empty()) palette = doc.components.front().palette;
        if (palette.empty()) {
            projv::core::warn("[TREES] {} has no palette in its compose.json - colours will be black", name);
        }

        TreeAsset asset;
        asset.name = name;
        asset.climate = spec;
        asset.lo = projv::core::ivec3(1 << 20);
        asset.hi = projv::core::ivec3(-(1 << 20));
        asset.mips.resize(1);
        TreeMip& base = asset.mips[0];
        base.divisor = 1;

        for (const projv::DataBlock& block : data.blocks) {
            projv::core::ivec3 blockOrigin(block.gridX, block.gridY, block.gridZ);
            blockOrigin *= int(data.resolution);

            // The tree64 encodes position structurally, in the descent path; brickMapFromTree64 is
            // what turns that back into coordinates, and it carries the material bytes across with it.
            std::unique_ptr<projv::VoxelBrickMap> brickMap =
                projv::utils::brickMapFromTree64(block.geometry, block.materialIDs, data.resolution);

            for (uint32_t brickIndex = 0; brickIndex < brickMap->totalBricks; brickIndex++) {
                const projv::BrickData* brick = brickMap->bricks[brickIndex].get();
                if (brick == nullptr) continue;
                projv::core::ivec3 brickOrigin =
                    projv::utils::reverseZOrderIndex(brickIndex) * int(projv::BRICK_SIZE);

                for (uint32_t row = 0; row < projv::BRICK_MASK_ROWS; row++) {
                    uint64_t rowBits = brick->mask[row];
                    while (rowBits != 0) {
                        // Tree64 convention: bit 63 is Z-order position 0 within the row.
                        int leadingZeros = __builtin_clzll(rowBits);
                        rowBits &= ~(1ull << (63 - leadingZeros));
                        uint32_t localZOrder = row * 64 + uint32_t(leadingZeros);

                        projv::core::ivec3 p = blockOrigin + brickOrigin +
                                               projv::utils::reverseZOrderIndex(localZOrder);
                        if (p.x < 0 || p.y < 0 || p.z < 0 || p.x > 255 || p.y > 255 || p.z > 255) continue;

                        TreeVoxel v;
                        v.x = uint8_t(p.x); v.y = uint8_t(p.y); v.z = uint8_t(p.z);
                        base.voxels.push_back(v);
                        // Material::packedColor is R10G10B10, which is the raw form sourceColors
                        // wants and what resolveMaterials later interns.
                        uint8_t slot = projv::utils::brickMapGetMaterial(*brick, localZOrder);
                        base.sourceColors.push_back(slot < palette.size()
                                                    ? palette[slot].packedColor : 0u);

                        asset.lo = projv::core::min(asset.lo, p);
                        asset.hi = projv::core::max(asset.hi, p);
                    }
                }
            }
        }
        if (base.voxels.empty()) {
            projv::core::warn("[TREES] {} decoded to zero voxels - skipping", name);
            continue;
        }

        // Centre the stamp on the trunk, not on the canopy: canopies are lopsided, and a tree that
        // pivots around its canopy centroid visibly slides off the point it was planted on.
        long sumX = 0, sumZ = 0, count = 0;
        int trunkBand = asset.lo.y + 3;
        for (const TreeVoxel& v : base.voxels) {
            if (int(v.y) > trunkBand) continue;
            sumX += v.x; sumZ += v.z; ++count;
        }
        if (count > 0) {
            asset.trunkX = int(sumX / count);
            asset.trunkZ = int(sumZ / count);
        } else {
            asset.trunkX = (asset.lo.x + asset.hi.x) / 2;
            asset.trunkZ = (asset.lo.z + asset.hi.z) / 2;
        }

        base.lo = asset.lo; base.hi = asset.hi;
        base.trunkX = asset.trunkX; base.trunkZ = asset.trunkZ;
        buildMips(asset, mipLevels);

        std::string mipSizes;
        for (const TreeMip& m : asset.mips)
            mipSizes += (mipSizes.empty() ? "" : "/") + std::to_string(m.voxels.size());
        projv::core::info("[TREES] {}: extent {}x{}x{}, trunk at ({},{}), mip voxels {}",
                          asset.name,
                          asset.hi.x - asset.lo.x + 1, asset.hi.y - asset.lo.y + 1,
                          asset.hi.z - asset.lo.z + 1, asset.trunkX, asset.trunkZ, mipSizes);
        lib.assets.push_back(std::move(asset));
    }
    return !lib.assets.empty();
}

// Turns each asset's raw colors into palette slots. `internColor` must be the caller's
// quantize-then-intern step; run this once, single-threaded, before any worker starts, so the
// workers only ever read already-resolved material IDs and never touch the palette for a tree.
template <typename InternFn>
inline void resolveMaterials(TreeLibrary& lib, InternFn&& internColor) {
    for (TreeAsset& asset : lib.assets) {
        for (TreeMip& mip : asset.mips) {
            for (size_t i = 0; i < mip.voxels.size(); ++i) {
                mip.voxels[i].materialID = internColor(mip.sourceColors[i]);
            }
            mip.sourceColors.clear();
            mip.sourceColors.shrink_to_fit();
        }
    }
}

struct Params {
    // World size of one candidate cell. At most one tree per cell, so this sets the spacing floor.
    // Set below one tree width (a tree is 64 * treeVoxelWorldSize ~= 112 units across) so that a
    // fully-stocked patch overlaps into closed canopy rather than a regular orchard grid.
    float cellSize = 84.0f;
    // Fraction of cells that grow a tree where the climate is ideal and the patch field is at its
    // peak. Everything below scales this down; nothing scales it up.
    float maxDensity = 0.92f;
    // Floor the patch field applies in the gaps between stands, so forest edges thin out into
    // scattered individuals instead of ending on a hard line.
    float patchFloor = 0.12f;
    // World size of a forest patch. Sets how big a continuous stand (and the clearing next to it)
    // tends to be, independent of the climate bands, which are far larger.
    float patchSize = 2200.0f;

    // --- climate response -------------------------------------------------------------------
    //
    // These thresholds are tuned to the ranges this terrain actually produces on plantable ground,
    // not to a nominal 0..1. Measured over the map: temperature spans about 0.1-0.8 (peaking near
    // 0.65) and humidity only about 0.2-0.7 (peaking near 0.45). Thresholds picked for a full 0..1
    // sweep would sit almost entirely outside the data -- a humidity gate opening at 0.8 would
    // simply switch every forest off.
    //
    // Humidity is the main gate: below humidDry nothing grows, above humidWet the ground is fully
    // stocked, and it ramps between. Positioned so the ramp straddles the 0.3-0.45 band where a
    // quarter of the land sits (thinning it into dry woodland) and the wetter ~70% above it closes
    // into full forest. Sliding the ramp even 0.1 higher lands it on the distribution's peak and
    // halves the tree count -- this gate is by far the most sensitive constant here.
    float humidDry = 0.28f, humidWet = 0.46f;
    // Cold gate: nothing grows on ice, ramping in across the sub-polar band (~19% of land).
    float tempFrozen = 0.12f, tempCool = 0.28f;
    // Hot-dry gate: at the warm end, trees survive only where it is also humid, so hot+dry thins to
    // scrub and hot+wet stays dense.
    float tempScorch = 0.74f, tempInferno = 0.92f;
    float scorchRescueHumid = 0.55f;
    // World size of one tree-asset voxel. Fixes the real-world size of a tree independent of the
    // resolution of the chunk it is being stamped into.
    float treeVoxelWorldSize = 1.75f;
    float waterLevel = 400.0f;
    float minAltitudeAboveWater = 6.0f;   // keep trunks out of the shallows
    // Backstop only. The real tree line is climatic and comes from climateStocking against a
    // temperature that now falls with altitude (see the lapse rate in terrain_noise::sampleTerrain),
    // which puts it at a different height in a cold belt than in a warm one -- as it should be.
    // This is just a hard cap so nothing turns up on a summit. It was 1500, set when that was near
    // the top of the world; leaving it there once peaks reached 5000+ would have cut every forest
    // off at a flat contour line straight across the mountains.
    float treeLineAltitude = 6000.0f;
    // Max height change (world units) across a probe of ±cellSize/4 before the ground is called too
    // steep to hold a tree. Stops trees growing out of cliff faces at right angles.
    float maxSlope = 90.0f;
    // How far to sink the trunk, in tree voxels. Absorbs the small height difference between the
    // octave count used for placement and the one a coarser chunk's ground was generated with, so a
    // tree never hovers.
    int embedVoxels = 2;
    uint32_t seed = 133;
};

// One planted tree, in world space. Chunk-independent by construction.
struct TreeInstance {
    const TreeAsset* asset = nullptr;
    float worldX = 0.0f, worldZ = 0.0f;
    float baseWorldY = 0.0f;
};

namespace detail {

inline uint32_t hashCell(int x, int z, uint32_t salt) {
    uint32_t h = uint32_t(x) * 0x8DA6B343u ^ uint32_t(z) * 0xD8163841u ^ salt * 0xCB1AB31Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
inline float hashUnit(int x, int z, uint32_t salt) {
    return float(hashCell(x, z, salt) >> 8) * (1.0f / 16777216.0f);
}
inline int floorDiv(int a, int b) {
    int q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

inline float smoothstep(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
    float t = (x - edge0) / (edge1 - edge0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// Smooth value noise on a lattice of `size` world units. Used for the forest-patch field, which
// wants gentle blobs rather than the per-cell white noise everything else here uses -- a hash
// straight off the candidate cell would give a uniform sprinkle, not stands with clearings between
// them. Kept local so tree placement stays a pure function of position with no shared generator.
inline float valueNoise(float x, float z, float size, uint32_t salt) {
    float fx = x / size, fz = z / size;
    int ix = int(std::floor(fx)), iz = int(std::floor(fz));
    float tx = fx - float(ix), tz = fz - float(iz);
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    float n00 = hashUnit(ix, iz, salt);
    float n10 = hashUnit(ix + 1, iz, salt);
    float n01 = hashUnit(ix, iz + 1, salt);
    float n11 = hashUnit(ix + 1, iz + 1, salt);
    float a = n00 + (n10 - n00) * tx;
    float b = n01 + (n11 - n01) * tx;
    return a + (b - a) * tz;
}

}  // namespace detail

// How thoroughly a climate stocks the ground with trees, in [0,1]. Pure function of the terrain's
// own temperature/humidity fields, so forests follow the same climate bands that colour the ground:
// wet temperate belts close into continuous canopy, deserts and ice come out bare, and hot ground
// is forested only where it is also wet.
inline float climateStocking(const Params& p, float temp, float humid) {
    float wet    = detail::smoothstep(p.humidDry, p.humidWet, humid);
    float notIce = detail::smoothstep(p.tempFrozen, p.tempCool, temp);
    // Above tempScorch, dryness starts killing everything off; humidity buys it back (jungle).
    float scorch = detail::smoothstep(p.tempScorch, p.tempInferno, temp);
    float rescue = detail::smoothstep(p.humidDry, p.scorchRescueHumid, humid);
    float notDesert = 1.0f - scorch * (1.0f - rescue);
    return wet * notIce * notDesert;
}

// Relative likelihood of one species at a given climate: a separable falloff around the species'
// preferred temperature and humidity. Never returns exactly zero, so a species can still turn up as
// a stray outside its band rather than stopping at a hard boundary.
inline float speciesAffinity(const TreeSpecies& s, float temp, float humid) {
    float dt = (temp - s.tempCenter) / (s.tempWidth > 1e-4f ? s.tempWidth : 1e-4f);
    float dh = (humid - s.humidCenter) / (s.humidWidth > 1e-4f ? s.humidWidth : 1e-4f);
    return s.abundance * std::exp(-0.5f * (dt * dt + dh * dh)) + 1e-4f;
}

// Collects every tree whose trunk falls in the world XZ rectangle. Callers pass a rectangle grown
// by the canopy radius, so a tree rooted outside a chunk still gets a chance to reach into it.
//
// A cell survives four independent filters, cheapest first:
//   1. the forest-patch field, which carves stands and clearings out of otherwise uniform ground,
//   2. the local climate, which decides how thoroughly this latitude stocks itself at all,
//   3. altitude -- out of the water, below the tree line,
//   4. slope -- no trees growing sideways out of cliffs.
// Species is then drawn from the eight, weighted by how well each one likes the local climate.
//
// `sampleGround(x, z)` must be LOD-independent -- the same function for every chunk -- or the same
// tree would be planted at two different heights in two neighbouring chunks.
// Per-filter rejection counts. Optional; pass one when tuning forest density, so it is obvious
// which filter is actually doing the culling rather than guessing at the constants.
struct CollectStats {
    size_t cells = 0, rejectedPatch = 0, rejectedWater = 0, rejectedTreeLine = 0,
           rejectedClimate = 0, rejectedSlope = 0, planted = 0;
};

template <typename GroundFn>
inline void collectTrees(const TreeLibrary& lib, const Params& p,
                         float minX, float maxX, float minZ, float maxZ,
                         GroundFn&& sampleGround, std::vector<TreeInstance>& out,
                         CollectStats* stats = nullptr) {
    if (lib.empty()) return;

    int cellX0 = int(std::floor(minX / p.cellSize));
    int cellX1 = int(std::floor(maxX / p.cellSize));
    int cellZ0 = int(std::floor(minZ / p.cellSize));
    int cellZ1 = int(std::floor(maxZ / p.cellSize));

    float affinity[16];
    const size_t speciesCount = lib.assets.size() < 16 ? lib.assets.size() : 16;

    for (int cz = cellZ0; cz <= cellZ1; ++cz) {
        for (int cx = cellX0; cx <= cellX1; ++cx) {
            // Jitter inside the cell, keeping a margin so two neighbouring trees cannot end up
            // fully overlapping.
            float jx = 0.2f + 0.6f * detail::hashUnit(cx, cz, p.seed + 2u);
            float jz = 0.2f + 0.6f * detail::hashUnit(cx, cz, p.seed + 3u);
            float wx = (float(cx) + jx) * p.cellSize;
            float wz = (float(cz) + jz) * p.cellSize;
            if (wx < minX || wx > maxX || wz < minZ || wz > maxZ) continue;
            if (stats) stats->cells++;

            // Patch field first: it is two hashes and rejects most of the map's clearings before
            // anything touches the (much more expensive) terrain noise.
            float patch = detail::valueNoise(wx, wz, p.patchSize, p.seed + 7u);
            patch = p.patchFloor + (1.0f - p.patchFloor) * detail::smoothstep(0.30f, 0.72f, patch);

            float roll = detail::hashUnit(cx, cz, p.seed + 1u);
            if (roll > p.maxDensity * patch) { if (stats) stats->rejectedPatch++; continue; }

            GroundSample g = sampleGround(wx, wz);
            float waterHere = std::max(g.waterTop, p.waterLevel);
            if (g.height < waterHere + p.minAltitudeAboveWater) { if (stats) stats->rejectedWater++; continue; }
            if (g.height > p.treeLineAltitude) { if (stats) stats->rejectedTreeLine++; continue; }

            // Re-roll against the climate on the same draw, rescaled into the range the patch test
            // already admitted. Using the same `roll` keeps this one hash per cell rather than two.
            float stocking = climateStocking(p, g.temp, g.humid);
            if (roll > p.maxDensity * patch * stocking) { if (stats) stats->rejectedClimate++; continue; }

            // Slope probe: four taps around the trunk.
            float probe = p.cellSize * 0.25f;
            float hxm = sampleGround(wx - probe, wz).height;
            float hxp = sampleGround(wx + probe, wz).height;
            float hzm = sampleGround(wx, wz - probe).height;
            float hzp = sampleGround(wx, wz + probe).height;
            float lowest = std::min(std::min(hxm, hxp), std::min(hzm, hzp));
            float highest = std::max(std::max(hxm, hxp), std::max(hzm, hzp));
            if (highest - lowest > p.maxSlope) { if (stats) stats->rejectedSlope++; continue; }

            // Weighted species draw. Neighbouring trees see near-identical climate, so they draw
            // from near-identical weights -- which is what makes a stand come out as one species
            // with a few strays rather than a random mix.
            float total = 0.0f;
            for (size_t i = 0; i < speciesCount; ++i) {
                affinity[i] = speciesAffinity(lib.assets[i].climate, g.temp, g.humid);
                total += affinity[i];
            }
            float pick = detail::hashUnit(cx, cz, p.seed + 4u) * total;
            size_t chosen = speciesCount - 1;
            for (size_t i = 0; i < speciesCount; ++i) {
                pick -= affinity[i];
                if (pick <= 0.0f) { chosen = i; break; }
            }

            TreeInstance inst;
            inst.asset = &lib.assets[chosen];
            inst.worldX = wx;
            inst.worldZ = wz;
            inst.baseWorldY = g.height;
            out.push_back(inst);
            if (stats) stats->planted++;
        }
    }
}

// World-space AABB a planted tree occupies. Used both to grow the collection rectangle and to
// reject trees that cannot reach a given chunk.
inline void treeWorldBounds(const TreeInstance& inst, const Params& p,
                            projv::core::vec3& lo, projv::core::vec3& hi) {
    const TreeAsset& a = *inst.asset;
    float s = p.treeVoxelWorldSize;
    lo.x = inst.worldX + float(a.lo.x - a.trunkX) * s;
    hi.x = inst.worldX + float(a.hi.x - a.trunkX + 1) * s;
    lo.z = inst.worldZ + float(a.lo.z - a.trunkZ) * s;
    hi.z = inst.worldZ + float(a.hi.z - a.trunkZ + 1) * s;
    lo.y = inst.baseWorldY - float(p.embedVoxels) * s;
    hi.y = lo.y + float(a.hi.y - a.lo.y + 1) * s;
}

// Widest canopy half-extent in the library, in world units. The margin a chunk has to look beyond
// its own bounds to find every tree that can reach it.
inline float canopyReach(const TreeLibrary& lib, const Params& p) {
    int widest = 0;
    for (const TreeAsset& a : lib.assets) {
        widest = std::max(widest, std::max(a.hi.x - a.trunkX, a.trunkX - a.lo.x));
        widest = std::max(widest, std::max(a.hi.z - a.trunkZ, a.trunkZ - a.lo.z));
    }
    return float(widest + 1) * p.treeVoxelWorldSize;
}

// Tallest tree in the library, in world units above the ground it is rooted in. The vertical
// counterpart of canopyReach: how far above the terrain surface a chunk slab has to be before
// nothing rooted under it can possibly reach in.
inline float canopyHeight(const TreeLibrary& lib, const Params& p) {
    int tallest = 0;
    for (const TreeAsset& a : lib.assets) tallest = std::max(tallest, a.hi.y - a.lo.y + 1);
    return float(tallest + 1) * p.treeVoxelWorldSize;
}

// Writes one tree's voxels into a chunk's brick map. Only the voxels that land inside the chunk are
// written; the rest belong to a neighbour, which is stamping the same tree from its own side.
//
// A chunk whose voxels are coarser than a tree voxel (any LOD ring below the finest) reads a coarse
// mip instead, so the tree keeps the same physical size at every resolution -- which is the whole
// reason treeVoxelWorldSize is fixed rather than derived from the chunk. `residual` covers the case
// where the ring's step is not exactly one of the prebuilt mip levels; it is 1 whenever they line
// up, which for the terrain's 4x rings they always do.
inline void stampTree(const TreeInstance& inst, const Params& p, projv::VoxelBrickMap& map,
                      projv::core::ivec3 chunkCoord, int res, float voxelScale) {
    const TreeAsset& a = *inst.asset;

    int decim = std::max(1, int(std::lround(voxelScale / p.treeVoxelWorldSize)));
    const TreeMip& mip = a.mipForDivisor(decim);
    int residual = std::max(1, decim / mip.divisor);

    // Global voxel coordinate (at this chunk's scale) of the trunk base.
    int gx = int(std::floor(inst.worldX / voxelScale));
    int gz = int(std::floor(inst.worldZ / voxelScale));
    int gy = int(std::floor(inst.baseWorldY / voxelScale))
           - int(std::lround(float(p.embedVoxels) * p.treeVoxelWorldSize / voxelScale));

    int baseX = chunkCoord.x * res;
    int baseY = chunkCoord.y * res;
    int baseZ = chunkCoord.z * res;

    for (const TreeVoxel& v : mip.voxels) {
        int tx = gx + detail::floorDiv(int(v.x) - mip.trunkX, residual);
        int ty = gy + detail::floorDiv(int(v.y) - mip.lo.y, residual);
        int tz = gz + detail::floorDiv(int(v.z) - mip.trunkZ, residual);

        int lx = tx - baseX;
        if (lx < 0 || lx >= res) continue;
        int ly = ty - baseY;
        if (ly < 0 || ly >= res) continue;
        int lz = tz - baseZ;
        if (lz < 0 || lz >= res) continue;

        projv::utils::brickMapSetVoxel(map, lx, ly, lz, v.materialID);
    }
}

}  // namespace trees

#endif
