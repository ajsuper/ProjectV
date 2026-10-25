#include "writer.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "data_structures/compose.h"
#include "utils/compose_io.h"
#include "utils/material.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"

namespace fs = std::filesystem;

namespace teardown {

namespace {
    // ---- Palettes ---------------------------------------------------------------------------
    //
    // A ProjectV component indexes at most 255 materials (MAX_PALETTE_ENTRIES: 255 means none). A Teardown level mixes hundreds of vox
    // palettes, so the world usually has thousands; they are clustered down with k-means in Oklab,
    // weighted by voxel count, with transparency, emission and metalness as extra dimensions so a
    // pane of glass or a lamp never merges into the wall around it.

    float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
    float linearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

    struct Feature { float v[6]; };

    Feature feature(const VoxMaterial& m) {
        const float r = srgbToLinear(m.r / 255.0f), g = srgbToLinear(m.g / 255.0f), b = srgbToLinear(m.b / 255.0f);
        const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
        const float mm = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
        const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
        Feature f;
        f.v[0] = 0.2104542553f * l + 0.7936177850f * mm - 0.0040720468f * s;
        f.v[1] = 1.9779984951f * l - 2.4285922050f * mm + 0.4505937099f * s;
        f.v[2] = 0.0259040371f * l + 0.7827717662f * mm - 0.8086757660f * s;
        f.v[3] = (255 - m.alpha) / 255.0f * 2.0f;
        f.v[4] = m.emissive > 0 ? 2.0f + m.emissive / 255.0f : 0.0f;
        f.v[5] = m.metal / 255.0f * 0.5f;
        return f;
    }

    float dist2(const Feature& a, const Feature& b) {
        float d = 0;
        for (int i = 0; i < 6; i++) d += (a.v[i] - b.v[i]) * (a.v[i] - b.v[i]);
        return d;
    }

    struct Palette {
        std::vector<VoxMaterial> entries;
        std::unordered_map<MatId, uint8_t> slot;
        double maxError = 0;
    };

    Palette buildPalette(const std::unordered_map<MatId, uint64_t>& counts) {
        Palette p;
        std::vector<std::pair<MatId, uint64_t>> items(counts.begin(), counts.end());
        // Most used first, so the order is stable and slot 0 is the commonest material.
        std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        if (items.size() <= projv::MAX_PALETTE_ENTRIES) {
            for (size_t i = 0; i < items.size(); i++) {
                p.entries.push_back(materials().get(items[i].first));
                p.slot[items[i].first] = uint8_t(i);
            }
            return p;
        }

        const size_t K = projv::MAX_PALETTE_ENTRIES;   // slot 255 is INVALID_MATERIAL, not a slot
        std::vector<Feature> feats;
        for (auto& [id, n] : items) feats.push_back(feature(materials().get(id)));
        // k-means++ seeding, weighted by count, deterministic.
        std::mt19937_64 rng(12345);
        std::vector<Feature> centres{feats[0]};
        std::vector<float> best(items.size(), 1e30f);
        while (centres.size() < K) {
            double total = 0;
            for (size_t i = 0; i < items.size(); i++) {
                best[i] = std::min(best[i], dist2(feats[i], centres.back()));
                total += best[i] * std::sqrt(double(items[i].second));
            }
            if (total <= 0) break;
            double pick = std::uniform_real_distribution<double>(0, total)(rng);
            size_t chosen = items.size() - 1;
            for (size_t i = 0; i < items.size(); i++) {
                pick -= best[i] * std::sqrt(double(items[i].second));
                if (pick <= 0) { chosen = i; break; }
            }
            centres.push_back(feats[chosen]);
        }
        std::vector<int> assign(items.size(), 0);
        for (int iter = 0; iter < 12; iter++) {
            for (size_t i = 0; i < items.size(); i++) {
                float bd = 1e30f;
                for (size_t c = 0; c < centres.size(); c++) {
                    float d = dist2(feats[i], centres[c]);
                    if (d < bd) { bd = d; assign[i] = int(c); }
                }
            }
            std::vector<std::array<double, 6>> sum(centres.size(), std::array<double, 6>{});
            std::vector<double> w(centres.size(), 0);
            for (size_t i = 0; i < items.size(); i++) {
                double wi = double(items[i].second);
                for (int k = 0; k < 6; k++) sum[size_t(assign[i])][size_t(k)] += feats[i].v[k] * wi;
                w[size_t(assign[i])] += wi;
            }
            for (size_t c = 0; c < centres.size(); c++)
                if (w[c] > 0)
                    for (int k = 0; k < 6; k++) centres[c].v[k] = float(sum[c][size_t(k)] / w[c]);
        }
        // Each cluster's entry: the weighted mean colour (in linear light), with the substance and
        // surface of its commonest member.
        struct Acc { double r = 0, g = 0, b = 0, w = 0; size_t top = SIZE_MAX; };
        std::vector<Acc> acc(centres.size());
        for (size_t i = 0; i < items.size(); i++) {
            Acc& a = acc[size_t(assign[i])];
            const VoxMaterial& m = materials().get(items[i].first);
            const double wi = double(items[i].second);
            a.r += srgbToLinear(m.r / 255.0f) * wi;
            a.g += srgbToLinear(m.g / 255.0f) * wi;
            a.b += srgbToLinear(m.b / 255.0f) * wi;
            a.w += wi;
            if (a.top == SIZE_MAX) a.top = i; // items are sorted by count
        }
        std::vector<int> slotOfCluster(centres.size(), -1);
        for (size_t c = 0; c < centres.size(); c++) {
            if (acc[c].top == SIZE_MAX) continue;
            VoxMaterial m = materials().get(items[acc[c].top].first);
            auto byte = [](double lin) { return uint8_t(std::lround(std::clamp(linearToSrgb(float(lin)), 0.0f, 1.0f) * 255.0f)); };
            m.r = byte(acc[c].r / acc[c].w);
            m.g = byte(acc[c].g / acc[c].w);
            m.b = byte(acc[c].b / acc[c].w);
            slotOfCluster[c] = int(p.entries.size());
            p.entries.push_back(m);
        }
        for (size_t i = 0; i < items.size(); i++) {
            const int s = slotOfCluster[size_t(assign[i])];
            p.slot[items[i].first] = uint8_t(s);
            Feature a = feats[i], b = feature(p.entries[size_t(s)]);
            p.maxError = std::max(p.maxError, std::sqrt(double(a.v[0] - b.v[0]) * (a.v[0] - b.v[0]) + (a.v[1] - b.v[1]) * (a.v[1] - b.v[1]) +
                                                        (a.v[2] - b.v[2]) * (a.v[2] - b.v[2])));
        }
        return p;
    }

    projv::Material toProjv(const VoxMaterial& m, bool physicsNames) {
        projv::Material out;
        out.packedColor = projv::packRGB10(m.r / 255.0f, m.g / 255.0f, m.b / 255.0f);
        const float transparency = 1.0f - m.alpha / 255.0f;
        out.packedSurface = projv::packSurfaceWord(m.gloss / 255.0f, m.metal / 255.0f, transparency,
                                                   transparency > 0.0f ? 1.5f : 1.0f);
        out.packedExtra = projv::packExtraWord(m.emissive / 16.0f, 0.0f, 0u);
        out.name = substanceName(m.substance);
        if (physicsNames && m.substance != Substance::None) out.physics = std::string("td_") + substanceName(m.substance);
        return out;
    }

    // ---- Geometry -----------------------------------------------------------------------------

    struct BlockJob {
        ivec3 coord;                          // grid coordinate (non-negative)
        std::vector<const Canvas::Brick*> bricks;
        std::vector<ivec3> brickBase;         // voxel coordinate of each brick, block-relative
    };

    // Builds every block of a component on worker threads through the engine's tree64 builder.
    projv::DataFile buildData(const Canvas& canvas, ivec3 origin, int resolution, const Palette& palette, int threads,
                              float voxelSize = TD_VOXEL) {
        // A brick goes to every block it overlaps -- a body's origin is its first voxel, not a brick
        // corner, so bricks straddle block edges -- and each block keeps only its own voxels.
        std::map<std::tuple<int, int, int>, BlockJob> jobs;
        for (const auto& [k, brick] : canvas.bricks()) {
            const ivec3 v = Canvas::unkey(k) * Canvas::B - origin;
            const ivec3 b0(floorDiv(v.x, resolution), floorDiv(v.y, resolution), floorDiv(v.z, resolution));
            const ivec3 v1 = v + (Canvas::B - 1);
            const ivec3 b1(floorDiv(v1.x, resolution), floorDiv(v1.y, resolution), floorDiv(v1.z, resolution));
            for (int bz = b0.z; bz <= b1.z; bz++)
                for (int by = b0.y; by <= b1.y; by++)
                    for (int bx = b0.x; bx <= b1.x; bx++) {
                        const ivec3 b(bx, by, bz);
                        BlockJob& job = jobs[{b.x, b.y, b.z}];
                        job.coord = b;
                        job.bricks.push_back(brick.get());
                        job.brickBase.push_back(v - b * resolution);
                    }
        }
        std::vector<BlockJob*> list;
        for (auto& [k, j] : jobs) list.push_back(&j);

        projv::DataFile file;
        file.resolution = uint32_t(resolution);
        file.voxelScale = voxelSize;
        file.blocks.resize(list.size());
        std::atomic<size_t> next{0};
        std::atomic<size_t> totalPlaced{0};
        auto work = [&]() {
            for (size_t i; (i = next++) < list.size();) {
                const BlockJob& job = *list[i];
                auto map = projv::utils::createVoxelBrickMap(projv::utils::computeBrickDims(uint32_t(resolution)));
                size_t placed = 0;
                for (size_t b = 0; b < job.bricks.size(); b++) {
                    const Canvas::Brick& brick = *job.bricks[b];
                    const ivec3 base = job.brickBase[b];
                    for (int v = 0; v < Canvas::B3; v++) {
                        const MatId m = brick.v[v];
                        if (!m || m == MAT_HOLE) continue;
                        const ivec3 p = base + ivec3(v & 7, (v >> 3) & 7, v >> 6);
                        if (glm::any(glm::lessThan(p, ivec3(0))) || glm::any(glm::greaterThanEqual(p, ivec3(resolution)))) continue;
                        projv::utils::brickMapSetVoxel(*map, p.x, p.y, p.z, palette.slot.at(m));
                        placed++;
                    }
                }
                if (!placed) continue; // only clipped edges of bricks that belong to a neighbour
                totalPlaced += placed;
                projv::DataBlock& block = file.blocks[i];
                block.gridX = job.coord.x;
                block.gridY = job.coord.y;
                block.gridZ = job.coord.z;
                block.geometry = projv::utils::buildTree64FromBrickMap(*map, resolution);
                projv::utils::bakeMaterialsFromBrickMap(block.geometry, block.materialIDs, *map);
            }
        };
        std::vector<std::thread> pool;
        for (int t = 0; t < std::max(1, threads); t++) pool.emplace_back(work);
        for (auto& t : pool) t.join();
        std::erase_if(file.blocks, [](const projv::DataBlock& b) { return b.geometry.empty() && b.tiles.texels.empty(); });
        size_t expected = 0;
        canvas.forEach([&](int, int, int, MatId m) { expected += m != MAT_HOLE; });
        if (expected != totalPlaced) logWarn("placed %zu of %zu voxels (res %d)", totalPlaced.load(), expected, resolution);
        return file;
    }

    std::unordered_map<MatId, uint64_t> countMaterials(const Canvas& c) {
        std::unordered_map<MatId, uint64_t> counts;
        for (const auto& [k, brick] : c.bricks())
            for (int i = 0; i < Canvas::B3; i++)
                if (brick->v[i] && brick->v[i] != MAT_HOLE) counts[brick->v[i]]++;
        return counts;
    }

    // An order-independent fingerprint of a body's voxels relative to its corner, so that a prefab
    // placed fifty times is written once.
    uint64_t fingerprint(const Canvas& c, ivec3 lo) {
        uint64_t h = 0, n = 0;
        c.forEach([&](int x, int y, int z, MatId m) {
            if (m == MAT_HOLE) return;
            uint64_t k = uint64_t(uint32_t(x - lo.x)) * 0x9E3779B97F4A7C15ull ^ uint64_t(uint32_t(y - lo.y)) * 0xC2B2AE3D27D4EB4Full ^
                         uint64_t(uint32_t(z - lo.z)) * 0x165667B19E3779F9ull ^ uint64_t(m) * 0x27D4EB2F165667C5ull;
            k ^= k >> 29;
            k *= 0xBF58476D1CE4E5B9ull;
            k ^= k >> 32;
            h += k;
            n++;
        });
        return h ^ (n * 0x94D049BB133111EBull);
    }

    int singleBlockResolution(ivec3 extent) {
        int r = 4;
        while (r < extent.x || r < extent.y || r < extent.z) r *= 4;
        return r;
    }

    std::string safeName(std::string s) {
        for (char& c : s)
            if (!std::isalnum(uint8_t(c)) && c != '-' && c != '_') c = '_';
        return s.empty() ? "body" : s;
    }

    size_t fileSize(const fs::path& p) {
        std::error_code ec;
        auto s = fs::file_size(p, ec);
        return ec ? 0 : size_t(s);
    }

    void writePhysicsLibrary(const fs::path& path) {
        struct Row { const char* name; float density, friction, strength; const char* fracture; float shard; bool anchor; };
        // Teardown's substances, by feel: what breaks how, and what does not break at all.
        const Row rows[] = {
            {"glass", 2500, 0.4f, 300, "shatter", 0.2f, false},
            {"foliage", 300, 0.8f, 50, "crumble", 0.3f, false},
            {"dirt", 1500, 0.9f, 400, "crumble", 0.4f, false},
            {"rock", 2600, 0.8f, 0, "shatter", 0.8f, true},
            {"wood", 600, 0.6f, 1500, "splinter", 0.6f, false},
            {"masonry", 2400, 0.8f, 20000, "shatter", 0.5f, false},
            {"brick", 1900, 0.7f, 8000, "seams", 0.4f, false},
            {"plaster", 1200, 0.7f, 800, "crumble", 0.3f, false},
            {"metal", 7800, 0.5f, 30000, "shatter", 0.4f, false},
            {"heavymetal", 7800, 0.5f, 200000, "shatter", 0.6f, false},
            {"plastic", 1000, 0.5f, 2000, "shatter", 0.3f, false},
            {"hardmetal", 7800, 0.5f, 0, "shatter", 0.5f, true},
            {"hardmasonry", 2400, 0.8f, 0, "shatter", 0.5f, true},
            {"ice", 917, 0.02f, 500, "shatter", 0.3f, false},
            {"unphysical", 100, 0.6f, 50, "crumble", 0.2f, false},
        };
        nlohmann::json lib;
        lib["v"] = 1;
        nlohmann::json list = nlohmann::json::array();
        for (const Row& r : rows) {
            nlohmann::json m;
            m["name"] = std::string("td_") + r.name;
            m["density"] = r.density;
            m["friction"] = r.friction;
            m["restitution"] = 0.0;
            if (r.strength > 0) {
                m["strength"] = r.strength;
                m["fracture"] = r.fracture;
                m["shardSize"] = r.shard;
            }
            if (r.anchor) m["anchor"] = true;
            list.push_back(m);
        }
        lib["materials"] = list;
        std::ofstream(path) << lib.dump(2) << "\n";
    }
}

bool rescaleCompose(const std::string& folder, float factor) {
    const std::string path = (fs::path(folder) / "compose.json").string();
    projv::ComposeDoc doc = projv::utils::parseComposeJson(path);
    if (doc.version == 0) return false;
    for (projv::ComposeComponent& c : doc.components) {
        c.position *= factor;
        c.scale *= factor;
    }
    return projv::utils::writeComposeJson(path, doc);
}

bool writeLevel(const Level& level, const LevelSource& src, const std::string& folderStr, const WriteOptions& options,
                WriteReport& report) {
    const fs::path folder(folderStr);
    std::error_code ec;
    fs::create_directories(folder / "bodies", ec);
    const int threads = options.threads > 0 ? options.threads : std::max(1, int(std::thread::hardware_concurrency()));

    projv::ComposeDoc doc;
    doc.name = src.title.empty() ? src.id : src.title;
    nlohmann::json entities = nlohmann::json::array();
    uint32_t nextId = 1;

    auto addComponent = [&](const std::string& name, const std::string& source, const Transform& placement,
                            const Palette& palette) -> uint32_t {
        projv::ComposeComponent c;
        c.type = projv::ComponentType::Data;
        c.name = name;
        c.source = source;
        c.id = nextId++;
        c.position = placement.pos * options.scale;
        c.rotation = placement.rot;
        c.scale = vec3(options.scale);
        c.mutability = projv::Mutability::Locked;
        for (const VoxMaterial& m : palette.entries) c.palette.push_back(toProjv(m, options.physicsNames));
        doc.components.push_back(std::move(c));
        return doc.components.back().id;
    };

    // ---- The static world: one grid -----------------------------------------------------------
    ivec3 lo, hi;
    if (level.world.bounds(lo, hi)) {
        const int res = options.blockResolution;
        const ivec3 origin(floorDiv(lo.x, res) * res, floorDiv(lo.y, res) * res, floorDiv(lo.z, res) * res);
        auto counts = countMaterials(level.world);
        report.worldMaterials = counts.size();
        Palette palette = buildPalette(counts);
        report.worldPaletteError = palette.maxError;
        projv::DataFile data = buildData(level.world, origin, res, palette, threads);
        report.worldBlocks = data.blocks.size();
        report.worldVoxels = 0;
        for (auto& [m, n] : counts) report.worldVoxels += n;
        if (!projv::utils::writeDataFile((folder / "world.data").string(), data)) return false;
        Transform placement;
        placement.pos = vec3(origin) * TD_VOXEL;
        uint32_t id = addComponent("world", "world.data", placement, palette);
        entities.push_back({{"name", "world"}, {"link", id}, {"components", {{"physics.static", {{"v", 1}}}}}});
    }

    // ---- Water: a translucent surface of coarse voxels, with no physics -----------------------
    if (level.water.bounds(lo, hi)) {
        const int res = options.blockResolution;
        const ivec3 origin(floorDiv(lo.x, res) * res, floorDiv(lo.y, res) * res, floorDiv(lo.z, res) * res);
        Palette palette = buildPalette(countMaterials(level.water));
        projv::DataFile data = buildData(level.water, origin, res, palette, threads, WATER_VOXEL);
        if (!projv::utils::writeDataFile((folder / "water.data").string(), data)) return false;
        Transform placement;
        placement.pos = vec3(origin) * WATER_VOXEL;
        addComponent("water", "water.data", placement, palette);
        report.waterVoxels = level.water.voxelCount();
    }

    // ---- Dynamic bodies: one loose chunk each, identical ones sharing a file -------------------
    struct Written { std::string source; Palette palette; size_t voxels = 0; };
    std::unordered_map<uint64_t, Written> written;
    std::set<std::string> usedNames;
    for (const auto& body : level.bodies) {
        if (!body->voxels.bounds(lo, hi)) continue;
        const ivec3 extent = hi - lo;
        const uint64_t key = fingerprint(body->voxels, lo) ^ uint64_t(extent.x) << 40 ^ uint64_t(extent.y) << 20 ^ uint64_t(extent.z);
        auto it = written.find(key);
        if (it == written.end()) {
            // Unique against every name taken so far: "door_1" may be a body's own name.
            const std::string base = safeName(body->name);
            std::string stem = base;
            for (int n = 1; !usedNames.insert(stem).second; n++) stem = base + "_" + std::to_string(n);
            std::string source = "bodies/" + stem + ".data";
            Palette palette = buildPalette(countMaterials(body->voxels));
            // A body larger than a 1024^3 tree becomes a grid of 64^3 blocks like the world.
            const int maxExtent = std::max({extent.x, extent.y, extent.z});
            const int res = maxExtent <= 1024 ? singleBlockResolution(extent) : 64;
            projv::DataFile data = buildData(body->voxels, lo, res, palette, threads);
            if (!projv::utils::writeDataFile((folder / source).string(), data)) return false;
            report.bodyFiles++;
            logVerbose("   file %s %zu", source.c_str(), body->voxels.voxelCount());
            it = written.emplace(key, Written{source, std::move(palette), body->voxels.voxelCount()}).first;
        } else if (it->second.voxels != body->voxels.voxelCount()) {
            logWarn("dedup mismatch %s: %zu vs %zu", body->name.c_str(), it->second.voxels, body->voxels.voxelCount());
        }
        report.bodies++;
        for (auto& [m, n] : countMaterials(body->voxels)) report.bodyVoxels += n;
        Transform placement = body->frame;
        placement.pos = body->frame.apply(vec3(lo) * TD_VOXEL);
        uint32_t id = addComponent(body->name, it->second.source, placement, it->second.palette);
        entities.push_back({{"name", body->name}, {"link", id},
                            {"components", {{"physics.rigidbody", {{"v", 1}, {"breaksOnImpact", true}}}}}});
    }

    if (!projv::utils::writeComposeJson((folder / "compose.json").string(), doc)) return false;
    if (options.entities) {
        nlohmann::json e;
        e["version"] = 1;
        e["entities"] = entities;
        std::ofstream(folder / "entities.json") << e.dump(1) << "\n";
    }
    writePhysicsLibrary(folder / "physics_materials.json");

    for (const auto& entry : fs::recursive_directory_iterator(folder, ec))
        if (entry.is_regular_file()) report.bytes += fileSize(entry.path());
    return true;
}

} // namespace teardown
