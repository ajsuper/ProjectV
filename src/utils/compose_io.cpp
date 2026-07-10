#include "utils/compose_io.h"

#include <fstream>
#include <filesystem>
#include <functional>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/quaternion.hpp>

#include "utils/voxel_management.h"
#include "utils/streaming.h"
#include "nlohmann/json.hpp"

namespace projv::utils {
    namespace {
        constexpr char PVDT_MAGIC[4] = {'P', 'V', 'D', 'T'};
        constexpr uint32_t PVDT_VERSION = 1;
        constexpr uint32_t COMPOSE_VERSION = 1;
        // With geometry instancing, a repeated .data block costs only one shared pool entry plus a
        // cheap per-instance header at each level (not a full geometry copy), so geometry no longer
        // explodes with depth. Header/grid count, however, still grows ~(branch factor)^depth for a
        // *branching* cyclic scene, so a depth bound remains the safety net. 6 keeps a 5-way cyclic
        // scene (like the SponzaScene testbed) near ~1M chunks / a few hundred MB. Raise for shallow
        // or non-branching graphs; a total-instance budget would be the robust fix for deep branches.
        constexpr int MAX_RECURSION_DEPTH = 6;

        constexpr uint32_t HEADER_SIZE = 24;   // magic + version + flags + blockCount + resolution + voxelScale
        constexpr uint32_t BLOCK_ENTRY_SIZE = 40;

        template<typename T>
        void writePod(std::ofstream& out, const T& value) {
            out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        template<typename T>
        T readPod(std::ifstream& in) {
            T value{};
            in.read(reinterpret_cast<char*>(&value), sizeof(T));
            return value;
        }

        bool isIdentityQuat(const core::quat& q) {
            constexpr float eps = 1e-5f;
            return std::abs(q.w - 1.0f) < eps && std::abs(q.x) < eps &&
                   std::abs(q.y) < eps && std::abs(q.z) < eps;
        }
    }

    void writeDataFile(const std::string& path, const DataFile& data) {
        core::info("writeDataFile: Writing .data container with {} block(s) to: {}", data.blocks.size(), path);

        std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream out(path, std::ios::binary);
        if (!out) {
            core::error("writeDataFile: Failed to open file for writing: {}", path);
            return;
        }

        const uint32_t blockCount = static_cast<uint32_t>(data.blocks.size());
        const uint32_t flags = data.hasVoxelTypeData ? 1u : 0u;

        // Compute blob-region offsets. The blob starts right after the block table.
        uint64_t cursor = HEADER_SIZE + static_cast<uint64_t>(blockCount) * BLOCK_ENTRY_SIZE;
        struct BlobLoc { uint64_t geomOff; uint32_t geomLen; uint64_t typeOff; uint32_t typeLen; };
        std::vector<BlobLoc> locs(blockCount);
        for (uint32_t i = 0; i < blockCount; i++) {
            const DataBlock& b = data.blocks[i];
            locs[i].geomLen = static_cast<uint32_t>(b.geometry.size());
            locs[i].geomOff = cursor;
            cursor += static_cast<uint64_t>(locs[i].geomLen) * sizeof(uint32_t);

            if (!b.voxelTypeData.empty()) {
                locs[i].typeLen = static_cast<uint32_t>(b.voxelTypeData.size());
                locs[i].typeOff = cursor;
                cursor += static_cast<uint64_t>(locs[i].typeLen) * sizeof(uint32_t);
            } else {
                locs[i].typeLen = 0;
                locs[i].typeOff = 0;
            }
        }

        // Header.
        out.write(PVDT_MAGIC, 4);
        writePod(out, PVDT_VERSION);
        writePod(out, flags);
        writePod(out, blockCount);
        writePod(out, data.resolution);
        writePod(out, data.voxelScale);

        // Block table (40 bytes each: 3xint32 grid, 4 pad, u64 geomOff, u32 geomLen, u64 typeOff, u32 typeLen).
        for (uint32_t i = 0; i < blockCount; i++) {
            const DataBlock& b = data.blocks[i];
            writePod(out, b.gridX);
            writePod(out, b.gridY);
            writePod(out, b.gridZ);
            const uint32_t pad = 0;
            writePod(out, pad);
            writePod(out, locs[i].geomOff);
            writePod(out, locs[i].geomLen);
            writePod(out, locs[i].typeOff);
            writePod(out, locs[i].typeLen);
        }

        // Blob region, in the same order the offsets were computed.
        for (uint32_t i = 0; i < blockCount; i++) {
            const DataBlock& b = data.blocks[i];
            if (!b.geometry.empty()) {
                out.write(reinterpret_cast<const char*>(b.geometry.data()),
                          b.geometry.size() * sizeof(uint32_t));
            }
            if (!b.voxelTypeData.empty()) {
                out.write(reinterpret_cast<const char*>(b.voxelTypeData.data()),
                          b.voxelTypeData.size() * sizeof(uint32_t));
            }
        }

        out.close();
        core::info("writeDataFile: Successfully wrote {} block(s)", blockCount);
    }

    DataFile readDataFile(const std::string& path) {
        core::info("readDataFile: Reading .data container from: {}", path);
        DataFile data;

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            core::error("readDataFile: Failed to open file for reading: {}", path);
            return data;
        }

        char magic[4];
        in.read(magic, 4);
        if (std::memcmp(magic, PVDT_MAGIC, 4) != 0) {
            core::error("readDataFile: Bad magic in {} (expected PVDT) - not a .data container", path);
            return {};
        }

        data.version = readPod<uint32_t>(in);
        if (data.version != PVDT_VERSION) {
            core::warn("readDataFile: .data version {} does not match expected {}", data.version, PVDT_VERSION);
        }
        const uint32_t flags = readPod<uint32_t>(in);
        const uint32_t blockCount = readPod<uint32_t>(in);
        data.resolution = readPod<uint32_t>(in);
        data.voxelScale = readPod<float>(in);
        data.hasVoxelTypeData = (flags & 1u) != 0;

        // Read the block table first, then seek to each blob.
        struct Entry { int32_t gx, gy, gz; uint64_t geomOff; uint32_t geomLen; uint64_t typeOff; uint32_t typeLen; };
        std::vector<Entry> entries(blockCount);
        for (uint32_t i = 0; i < blockCount; i++) {
            entries[i].gx = readPod<int32_t>(in);
            entries[i].gy = readPod<int32_t>(in);
            entries[i].gz = readPod<int32_t>(in);
            (void)readPod<uint32_t>(in); // padding
            entries[i].geomOff = readPod<uint64_t>(in);
            entries[i].geomLen = readPod<uint32_t>(in);
            entries[i].typeOff = readPod<uint64_t>(in);
            entries[i].typeLen = readPod<uint32_t>(in);
        }

        data.blocks.resize(blockCount);
        for (uint32_t i = 0; i < blockCount; i++) {
            DataBlock& b = data.blocks[i];
            b.gridX = entries[i].gx;
            b.gridY = entries[i].gy;
            b.gridZ = entries[i].gz;

            if (entries[i].geomLen > 0) {
                b.geometry.resize(entries[i].geomLen);
                in.seekg(static_cast<std::streamoff>(entries[i].geomOff), std::ios::beg);
                in.read(reinterpret_cast<char*>(b.geometry.data()),
                        entries[i].geomLen * sizeof(uint32_t));
            }
            if (entries[i].typeLen > 0) {
                b.voxelTypeData.resize(entries[i].typeLen);
                in.seekg(static_cast<std::streamoff>(entries[i].typeOff), std::ios::beg);
                in.read(reinterpret_cast<char*>(b.voxelTypeData.data()),
                        entries[i].typeLen * sizeof(uint32_t));
            }
        }

        in.close();
        core::info("readDataFile: Successfully read {} block(s) (resolution {}, voxelScale {})",
                   blockCount, data.resolution, data.voxelScale);
        return data;
    }

    DataFileHeader readDataFileHeader(const std::string& path) {
        DataFileHeader hdr;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            core::error("readDataFileHeader: Failed to open file for reading: {}", path);
            return hdr;
        }
        char magic[4];
        in.read(magic, 4);
        if (std::memcmp(magic, PVDT_MAGIC, 4) != 0) {
            core::error("readDataFileHeader: Bad magic in {} (expected PVDT)", path);
            return {};
        }
        hdr.version = readPod<uint32_t>(in);
        if (hdr.version != PVDT_VERSION) {
            core::warn("readDataFileHeader: .data version {} does not match expected {}", hdr.version, PVDT_VERSION);
        }
        const uint32_t flags = readPod<uint32_t>(in);
        const uint32_t blockCount = readPod<uint32_t>(in);
        hdr.resolution = readPod<uint32_t>(in);
        hdr.voxelScale = readPod<float>(in);
        hdr.hasVoxelTypeData = (flags & 1u) != 0;

        hdr.blocks.resize(blockCount);
        for (uint32_t i = 0; i < blockCount; i++) {
            BlockEntry& e = hdr.blocks[i];
            e.gridX = readPod<int32_t>(in);
            e.gridY = readPod<int32_t>(in);
            e.gridZ = readPod<int32_t>(in);
            (void)readPod<uint32_t>(in); // padding
            e.geometryOffset = readPod<uint64_t>(in);
            e.geometryLength = readPod<uint32_t>(in);
            e.voxelTypeOffset = readPod<uint64_t>(in);
            e.voxelTypeLength = readPod<uint32_t>(in);
        }
        return hdr;
    }

    DataBlock readDataBlock(const std::string& path, const BlockEntry& entry) {
        DataBlock b;
        b.gridX = entry.gridX;
        b.gridY = entry.gridY;
        b.gridZ = entry.gridZ;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            core::error("readDataBlock: Failed to open file for reading: {}", path);
            return b;
        }
        if (entry.geometryLength > 0) {
            b.geometry.resize(entry.geometryLength);
            in.seekg(static_cast<std::streamoff>(entry.geometryOffset), std::ios::beg);
            in.read(reinterpret_cast<char*>(b.geometry.data()), entry.geometryLength * sizeof(uint32_t));
        }
        if (entry.voxelTypeLength > 0) {
            b.voxelTypeData.resize(entry.voxelTypeLength);
            in.seekg(static_cast<std::streamoff>(entry.voxelTypeOffset), std::ios::beg);
            in.read(reinterpret_cast<char*>(b.voxelTypeData.data()), entry.voxelTypeLength * sizeof(uint32_t));
        }
        return b;
    }

    DataBlock readDataBlock(const std::string& path, uint32_t blockIndex) {
        DataFileHeader hdr = readDataFileHeader(path);
        if (blockIndex >= hdr.blocks.size()) {
            core::error("readDataBlock: block index {} out of range ({} blocks) in {}",
                        blockIndex, hdr.blocks.size(), path);
            return {};
        }
        return readDataBlock(path, hdr.blocks[blockIndex]);
    }

    ComposeDoc parseComposeJson(const std::string& composeJsonPath) {
        core::info("parseComposeJson: Parsing {}", composeJsonPath);
        ComposeDoc doc;
        doc.version = 0;

        std::ifstream in(composeJsonPath);
        if (!in) {
            core::error("parseComposeJson: Failed to open {}", composeJsonPath);
            return doc;
        }
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        // ignore_comments = true: permit // and /* */ comments for authoring convenience.
        nlohmann::json json = nlohmann::json::parse(contents, nullptr, false, true);
        if (json.is_discarded()) {
            core::error("parseComposeJson: Malformed JSON in {}", composeJsonPath);
            return doc;
        }

        doc.version = json.value("version", 0u);
        if (doc.version != COMPOSE_VERSION) {
            core::error("parseComposeJson: Unsupported compose version {} in {} (expected {})",
                        doc.version, composeJsonPath, COMPOSE_VERSION);
            doc.version = 0;
            return doc;
        }
        doc.name = json.value("name", std::string(""));

        if (!json.contains("components") || !json["components"].is_array()) {
            core::error("parseComposeJson: Missing or invalid 'components' array in {}", composeJsonPath);
            doc.version = 0;
            return doc;
        }

        for (const auto& jc : json["components"]) {
            ComposeComponent c;

            std::string typeStr = jc.value("type", std::string(""));
            if (typeStr == "data") {
                c.type = ComponentType::Data;
            } else if (typeStr == "asset") {
                c.type = ComponentType::Asset;
            } else {
                core::error("parseComposeJson: Component with invalid/missing 'type' in {} - skipping", composeJsonPath);
                continue;
            }

            if (!jc.contains("source") || !jc["source"].is_string()) {
                core::error("parseComposeJson: Component missing 'source' in {} - skipping", composeJsonPath);
                continue;
            }
            c.source = jc["source"].get<std::string>();

            if (jc.contains("position") && jc["position"].is_array() && jc["position"].size() == 3) {
                c.position = core::vec3(jc["position"][0].get<float>(),
                                        jc["position"][1].get<float>(),
                                        jc["position"][2].get<float>());
            }

            if (jc.contains("rotation") && jc["rotation"].is_array()) {
                const auto& r = jc["rotation"];
                if (r.size() == 3) {
                    // Euler degrees, applied intrinsically X -> Y -> Z.
                    core::vec3 euler(r[0].get<float>(), r[1].get<float>(), r[2].get<float>());
                    c.rotation = core::quat(core::radians(euler));
                } else if (r.size() == 4) {
                    // Quaternion [x, y, z, w]; glm::quat constructor is (w, x, y, z).
                    c.rotation = core::quat(r[3].get<float>(), r[0].get<float>(),
                                            r[1].get<float>(), r[2].get<float>());
                } else {
                    core::warn("parseComposeJson: 'rotation' must have 3 or 4 elements in {} - using identity", composeJsonPath);
                }
            }

            if (jc.contains("scale")) {
                const auto& s = jc["scale"];
                if (s.is_number()) {
                    c.scale = core::vec3(s.get<float>());
                } else if (s.is_array() && s.size() == 3) {
                    c.scale = core::vec3(s[0].get<float>(), s[1].get<float>(), s[2].get<float>());
                } else {
                    core::warn("parseComposeJson: 'scale' must be a number or 3-array in {} - using 1.0", composeJsonPath);
                }
            }

            std::string mutStr = jc.value("mutability", std::string("locked"));
            if (mutStr == "direct") c.mutability = Mutability::Direct;
            else if (mutStr == "copy") c.mutability = Mutability::Copy;
            else c.mutability = Mutability::Locked;

            doc.components.push_back(std::move(c));
        }

        core::info("parseComposeJson: Parsed '{}' with {} component(s)", doc.name, doc.components.size());
        return doc;
    }

    Scene loadComposeFromDisk(const std::string& folderPath, StreamingContext* streaming) {
        core::info("loadComposeFromDisk: Loading compose scene from folder: {} ({} mode)",
                   folderPath, streaming ? "streaming" : "eager");
        Scene scene;

        std::unordered_map<std::string, DataFile> dataCache;
        // Shared geometry pool: each unique (resolved .data path + block grid coords) is stored once
        // and referenced by every instance chunk via Chunk.geometryPoolIndex.
        std::vector<GeometryBlob> geometryPool;
        std::unordered_map<std::string, int32_t> poolKeyToIndex;
        std::vector<std::string> folderStack;
        // Distinct cyclic dependencies already warned about, so a cycle logs once (not per level).
        std::unordered_set<std::string> warnedCycles;
        uint32_t nextChunkID = 0;

        // Loose (transform-placed) leaves and grid volumes are collected separately, then
        // assembled loose-first so the shader's brute-force loop covers only [0, looseChunkCount).
        std::vector<Chunk> looseChunks;
        struct PendingGrid { SceneGrid grid; std::vector<Chunk> blocks; std::string sourcePath; };
        std::vector<PendingGrid> pendingGrids;
        // Streaming mode: grid volumes become descriptor-only (no chunks); their streaming records are
        // collected here and pushed into `streaming` at assembly so they stay parallel to Scene.grids.
        struct PendingStreamGrid { SceneGrid grid; GridStreamRecord record; };
        std::vector<PendingStreamGrid> pendingStreamGrids;

        std::function<void(const std::string&, const core::mat4&, int, bool)> expand =
            [&](const std::string& folder, const core::mat4& parentWorld, int depth, bool ancestorRotated) {
            if (depth > MAX_RECURSION_DEPTH) {
                // Expected terminus for intentional cyclic dependencies (a warning was already
                // logged where the cycle was entered); also the safety bound for pathological depth.
                core::debug("loadComposeFromDisk: Recursion depth cap ({}) reached at {}", MAX_RECURSION_DEPTH, folder);
                return;
            }

            std::string composeJsonPath = (std::filesystem::path(folder) / "compose.json").string();
            ComposeDoc doc = parseComposeJson(composeJsonPath);
            if (doc.version == 0) {
                core::error("loadComposeFromDisk: Failed to load compose.json at {}", folder);
                return;
            }

            for (const ComposeComponent& c : doc.components) {
                // Local transform: T * R * S (scale first, then rotate, then translate).
                core::mat4 local = glm::translate(core::mat4(1.0f), c.position)
                                 * glm::mat4_cast(c.rotation)
                                 * glm::scale(core::mat4(1.0f), c.scale);
                core::mat4 world = parentWorld * local;
                bool rotated = ancestorRotated || !isIdentityQuat(c.rotation);

                if (c.type == ComponentType::Data) {
                    std::string resolved = std::filesystem::weakly_canonical(
                        std::filesystem::path(folder) / c.source).string();

                    // Streaming mode: a grid volume (multi-block .data) is deferred to descriptor-only —
                    // read just the block table, build the SceneGrid + streaming record, load NO geometry
                    // and create NO chunks. A single-block asset falls through to the eager load below.
                    if (streaming) {
                        DataFileHeader hdr = readDataFileHeader(resolved);
                        if (hdr.resolution == 0 || hdr.blocks.empty()) {
                            core::warn("loadComposeFromDisk: .data at {} is empty or invalid - skipping", resolved);
                            continue;
                        }
                        if (hdr.blocks.size() > 1) {
                            // Same world-matrix decomposition as the eager path (uniform scale + rotation).
                            core::vec3 sc0 = core::vec3(world[0]);
                            core::vec3 sc1 = core::vec3(world[1]);
                            core::vec3 sc2 = core::vec3(world[2]);
                            float usx = core::length(sc0), usy = core::length(sc1), usz = core::length(sc2);
                            if (std::abs(usx - usy) > 1e-4f || std::abs(usx - usz) > 1e-4f) {
                                core::warn("loadComposeFromDisk: Non-uniform scale on streamed grid '{}' "
                                           "({}, {}, {}); using X axis as uniform scale.", resolved, usx, usy, usz);
                            }
                            float uniformScale = usx;
                            core::mat3 gridRotMat = core::mat3(sc0 / usx, sc1 / usy, sc2 / usz);
                            core::quat gridRotation = glm::quat_cast(gridRotMat);
                            float localBlockScale = createChunkScaleFromVoxelScaleAndResolution(
                                hdr.voxelScale, static_cast<int>(hdr.resolution));

                            PendingStreamGrid psg;
                            psg.grid.origin = core::vec3(world[3]);
                            psg.grid.cellSize = localBlockScale * uniformScale;
                            psg.grid.rotation = gridRotation;
                            core::ivec3 dims(0);
                            for (const BlockEntry& e : hdr.blocks) {
                                dims.x = std::max(dims.x, e.gridX + 1);
                                dims.y = std::max(dims.y, e.gridY + 1);
                                dims.z = std::max(dims.z, e.gridZ + 1);
                            }
                            psg.grid.dims = dims;
                            psg.grid.cellToChunk.assign(
                                static_cast<size_t>(dims.x) * dims.y * dims.z, -1);
                            psg.record = GridStreamRecord{resolved, hdr.resolution, hdr.voxelScale,
                                                          uniformScale, c.mutability};
                            pendingStreamGrids.push_back(std::move(psg));

                            // Seed the block-table cache (warm, LRU-evictable — not pinned).
                            streaming->blockTables.lastUsed[resolved] = ++streaming->blockTables.tick;
                            streaming->blockTables.tables[resolved] = std::move(hdr);
                            continue;
                        }
                        // Single-block asset: fall through to the eager load path below.
                    }

                    auto cacheIt = dataCache.find(resolved);
                    if (cacheIt == dataCache.end()) {
                        cacheIt = dataCache.emplace(resolved, readDataFile(resolved)).first;
                    }
                    const DataFile& dataFile = cacheIt->second;

                    if (dataFile.resolution == 0 || dataFile.blocks.empty()) {
                        core::warn("loadComposeFromDisk: .data at {} is empty or invalid - skipping", resolved);
                        continue;
                    }

                    (void)rotated;

                    // Decompose the world matrix into scale (per basis vector) + pure rotation.
                    // Uniform scale is assumed; the shader ray transform is uniform-scale + rotation.
                    core::vec3 col0 = core::vec3(world[0]);
                    core::vec3 col1 = core::vec3(world[1]);
                    core::vec3 col2 = core::vec3(world[2]);
                    float sx = core::length(col0);
                    float sy = core::length(col1);
                    float sz = core::length(col2);
                    if (std::abs(sx - sy) > 1e-4f || std::abs(sx - sz) > 1e-4f) {
                        core::warn("loadComposeFromDisk: Non-uniform scale on data leaf '{}' "
                                   "({}, {}, {}); using X axis as uniform scale.", resolved, sx, sy, sz);
                    }
                    float uniformScale = sx;

                    // Pure world rotation: strip per-axis scale from the basis, then cast to a quaternion.
                    core::mat3 rotMat = core::mat3(col0 / sx, col1 / sy, col2 / sz);
                    core::quat worldRotation = glm::quat_cast(rotMat);

                    // Note: createChunkScaleFromVoxelScaleAndResolution multiplies by the raw
                    // resolution value (not its log2), matching how chunk scales are computed elsewhere.
                    float localBlockScale = createChunkScaleFromVoxelScaleAndResolution(
                        dataFile.voxelScale, static_cast<int>(dataFile.resolution));

                    // Builds one chunk from a block, placing its local corner through the full
                    // world matrix (T * R * S) so rotation about the grid/asset origin is baked here.
                    auto makeChunk = [&](const DataBlock& block) {
                        Chunk chunk;
                        chunk.header.chunkID = nextChunkID++;
                        chunk.header.resolution = dataFile.resolution;
                        chunk.header.voxelScale = dataFile.voxelScale * uniformScale;
                        chunk.header.scale = localBlockScale * uniformScale;
                        core::vec3 blockLocalCorner =
                            core::vec3(block.gridX, block.gridY, block.gridZ) * localBlockScale;
                        chunk.header.position = core::vec3(world * core::vec4(blockLocalCorner, 1.0f));
                        chunk.header.rotation = worldRotation;
                        chunk.mutability = c.mutability;
                        // Share geometry across instances: dedup this block into the pool by its
                        // canonical path + grid coords + mutability. Same source + same policy shares
                        // one buffer; the same source referenced with a *different* policy lands in a
                        // separate entry (a forced copy) so, e.g., a Direct edit never touches a Locked
                        // sibling's buffer. The chunk references the pool entry and keeps its own
                        // geometryData/voxelTypeData empty (populated only for legacy chunks).
                        std::string poolKey = resolved + "#" + std::to_string(block.gridX) + "_" +
                                              std::to_string(block.gridY) + "_" + std::to_string(block.gridZ) +
                                              "#" + std::to_string(static_cast<int>(c.mutability));
                        auto poolIt = poolKeyToIndex.find(poolKey);
                        if (poolIt == poolKeyToIndex.end()) {
                            int32_t idx = static_cast<int32_t>(geometryPool.size());
                            geometryPool.push_back(GeometryBlob{
                                block.geometry, block.voxelTypeData, resolved,
                                core::ivec3(block.gridX, block.gridY, block.gridZ)});
                            poolIt = poolKeyToIndex.emplace(poolKey, idx).first;
                        }
                        chunk.geometryPoolIndex = poolIt->second;
                        // One more live instance references this blob (drives GPU range lifetime).
                        geometryPool[chunk.geometryPoolIndex].refCount++;
                        chunk.LOD = 0;
                        return chunk;
                    };

                    // A multi-block .data is a grid volume (top-level DDA); a single block is a
                    // loose transform-placed asset (brute-force loop).
                    if (dataFile.blocks.size() > 1) {
                        PendingGrid pg;
                        pg.sourcePath = resolved;
                        pg.grid.origin = core::vec3(world[3]);
                        pg.grid.cellSize = localBlockScale * uniformScale;
                        pg.grid.rotation = worldRotation;
                        core::ivec3 dims(0);
                        for (const DataBlock& b : dataFile.blocks) {
                            dims.x = std::max(dims.x, b.gridX + 1);
                            dims.y = std::max(dims.y, b.gridY + 1);
                            dims.z = std::max(dims.z, b.gridZ + 1);
                        }
                        pg.grid.dims = dims;
                        pg.grid.cellToChunk.assign(
                            static_cast<size_t>(dims.x) * dims.y * dims.z, -1);
                        for (const DataBlock& block : dataFile.blocks) {
                            if (block.gridX < 0 || block.gridY < 0 || block.gridZ < 0) {
                                core::warn("loadComposeFromDisk: negative grid coord in '{}' - skipping block", resolved);
                                continue;
                            }
                            // Store the block's ordinal within this grid; rebased to an absolute
                            // Scene.chunks index after traversal (see assembly below).
                            int lin = block.gridX + dims.x * (block.gridY + dims.y * block.gridZ);
                            pg.grid.cellToChunk[lin] = static_cast<int32_t>(pg.blocks.size());
                            pg.blocks.push_back(makeChunk(block));
                            // Residency key: this block fills cell `lin`. gridIndex is stamped at
                            // assembly (final grid index isn't known until grids are appended).
                            pg.blocks.back().cellIndex = lin;
                        }
                        pendingGrids.push_back(std::move(pg));
                    } else {
                        // Single-block .data -> exactly one loose chunk -> exactly one component.
                        // The final Scene.chunks index is predictable here: loose chunks are moved
                        // into Scene.chunks first, in this exact order, at assembly below.
                        ComponentHandle componentHandle = static_cast<ComponentHandle>(scene.components.size());
                        ChunkHandle predictedHandle = static_cast<ChunkHandle>(looseChunks.size());
                        scene.components.push_back(ComponentRecord{
                            ComponentKind::Chunk, predictedHandle, -1, resolved});
                        for (const DataBlock& block : dataFile.blocks) {
                            Chunk chunk = makeChunk(block);
                            chunk.componentHandle = componentHandle;
                            looseChunks.push_back(std::move(chunk));
                        }
                    }
                } else { // Asset
                    std::string resolved = std::filesystem::weakly_canonical(
                        std::filesystem::path(folder) / c.source).string();

                    // Cyclic asset dependencies are allowed — they produce fun repeated/fractal
                    // structures as each level re-applies the asset transform — and are kept safe
                    // by the depth cap above. Warn once per distinct cycle so the log isn't spammed
                    // at every level of the descent.
                    if (std::find(folderStack.begin(), folderStack.end(), resolved) != folderStack.end()) {
                        if (warnedCycles.insert(resolved).second) {
                            core::warn("loadComposeFromDisk: Cyclic asset dependency at '{}' — allowed, "
                                       "but recursion will be capped at depth {}.", resolved, MAX_RECURSION_DEPTH);
                        }
                    }

                    folderStack.push_back(resolved);
                    expand(resolved, world, depth + 1, rotated);
                    folderStack.pop_back();
                }
            }
        };

        std::string rootCanonical = std::filesystem::weakly_canonical(folderPath).string();
        folderStack.push_back(rootCanonical);
        expand(rootCanonical, core::mat4(1.0f), 0, false);
        folderStack.pop_back();

        // Assemble loose-first, then each grid's blocks. Handles are just Scene.chunks indices, so we
        // keep the loose-first order, but nothing depends on it anymore: the loose set is an explicit
        // handle list and grid cells store handles. Rebase every grid's cellToChunk from per-grid
        // ordinals to absolute handles, and stamp each grid block's residency (gridIndex, cellIndex).
        scene.chunks = std::move(looseChunks);
        scene.looseChunkCount = static_cast<uint32_t>(scene.chunks.size());
        scene.looseChunks.resize(scene.looseChunkCount);
        for (ChunkHandle h = 0; h < scene.looseChunkCount; h++) scene.looseChunks[h] = h;
        for (PendingGrid& pg : pendingGrids) {
            int32_t gridIndex = static_cast<int32_t>(scene.grids.size());
            int32_t base = static_cast<int32_t>(scene.chunks.size());
            for (Chunk& c : pg.blocks) {
                c.gridIndex = gridIndex; // cellIndex was set at block creation
                scene.chunks.push_back(std::move(c));
            }
            for (int32_t& idx : pg.grid.cellToChunk) {
                if (idx >= 0) idx += base;
            }
            // One component owns every cell of this grid (gridIndex is only known here).
            ComponentHandle componentHandle = static_cast<ComponentHandle>(scene.components.size());
            scene.components.push_back(ComponentRecord{ComponentKind::Grid, 0, gridIndex, pg.sourcePath});
            pg.grid.componentHandle = componentHandle;
            scene.grids.push_back(std::move(pg.grid));
            // Keep streaming->gridRecords parallel to scene.grids even for eager grids (default record;
            // an eager grid is already fully resident, so it is never materialized from disk).
            if (streaming) streaming->gridRecords.push_back(GridStreamRecord{});
        }
        // Streamed grid volumes: descriptor only, no chunks; their cells materialize on demand.
        for (PendingStreamGrid& psg : pendingStreamGrids) {
            int32_t gridIndex = static_cast<int32_t>(scene.grids.size());
            ComponentHandle componentHandle = static_cast<ComponentHandle>(scene.components.size());
            scene.components.push_back(ComponentRecord{
                ComponentKind::Grid, 0, gridIndex, psg.record.sourceDataPath});
            psg.grid.componentHandle = componentHandle;
            scene.grids.push_back(std::move(psg.grid));
            if (streaming) streaming->gridRecords.push_back(std::move(psg.record));
        }

        // Pool indices are absolute and stable (never rebased, unlike cellToChunk), so we can hand
        // the pool over as-is.
        scene.geometryPool = std::move(geometryPool);

        core::info("loadComposeFromDisk: Loaded {} chunk(s) ({} loose, {} grid(s), {} unique geometry blob(s)) from compose scene",
                   scene.chunks.size(), scene.looseChunkCount, scene.grids.size(), scene.geometryPool.size());
        return scene;
    }

    int32_t makeChunkGeometryWritable(Scene& scene, Chunk& chunk) {
        int32_t idx = chunk.geometryPoolIndex;
        if (idx < 0 || idx >= static_cast<int32_t>(scene.geometryPool.size())) {
            core::warn("makeChunkGeometryWritable: chunk {} is not pooled; nothing to make writable", chunk.header.chunkID);
            return -1;
        }
        // Copy-on-write: a Copy instance forks a private duplicate the first time it is made writable.
        // Locked/Direct edit their shared blob directly (visible to same-policy instances); cross-policy
        // instances are already separate pool entries (mutability is part of the load-time dedup key).
        if (chunk.mutability == Mutability::Copy && !chunk.copyDiverged) {
            GeometryBlob fork = scene.geometryPool[idx]; // deep copy: geometry + voxelType + provenance
            // The fork still points at the inherited original source; it doesn't own a file until its
            // first persist writes one. (Guards against writing edits back to the shared original.)
            fork.ownsSourceFile = false;
            fork.refCount = 1;                       // this one chunk now references the private blob
            if (scene.geometryPool[idx].refCount > 0) // ...and no longer the shared original
                scene.geometryPool[idx].refCount--;
            int32_t newIdx = poolInsertBlob(scene, std::move(fork));
            chunk.geometryPoolIndex = newIdx;
            chunk.copyDiverged = true;
            core::info("makeChunkGeometryWritable: chunk {} (copy) forked geometry blob {} -> {}", chunk.header.chunkID, idx, newIdx);
            return newIdx;
        }
        return chunk.geometryPoolIndex;
    }

    void editChunkVoxels(Scene& scene, Chunk& chunk) {
        if (chunk.geometryPoolIndex < 0) {
            core::warn("editChunkVoxels: chunk {} is not pooled; skipping", chunk.header.chunkID);
            return;
        }
        if (chunk.chunkQueue.empty()) {
            core::warn("editChunkVoxels: chunk {} has an empty edit queue; nothing to do", chunk.header.chunkID);
            return;
        }
        int32_t idx = makeChunkGeometryWritable(scene, chunk);
        if (idx < 0) return;
        // Regenerate geometry from the queue via the standard chunk path, then move the result into the
        // pool blob so the chunk's own vectors stay empty (the pooled invariant). Full regeneration.
        updateChunkFromItsVoxelBatch(chunk); // fills chunk.geometryData / voxelTypeData from chunkQueue
        GeometryBlob& blob = scene.geometryPool[idx];
        blob.geometry = std::move(chunk.geometryData);
        blob.voxelTypeData = std::move(chunk.voxelTypeData);
        chunk.geometryData.clear();
        chunk.voxelTypeData.clear();
        core::info("editChunkVoxels: chunk {} regenerated geometry into pool blob {} ({} values)", chunk.header.chunkID, idx, blob.geometry.size());
    }

    bool persistChunkData(Scene& scene, Chunk& chunk, const std::string& copyDestPath) {
        if (chunk.geometryPoolIndex < 0) {
            core::warn("persistChunkData: chunk {} is not pooled; skipping", chunk.header.chunkID);
            return false;
        }
        if (chunk.mutability == Mutability::Locked) {
            core::info("persistChunkData: chunk {} is Locked; edits are in-memory only, not persisted", chunk.header.chunkID);
            return false;
        }

        // For Copy, ensure the instance owns a private blob before we touch provenance/geometry.
        int32_t idx = chunk.mutability == Mutability::Copy ? makeChunkGeometryWritable(scene, chunk)
                                                           : chunk.geometryPoolIndex;
        if (idx < 0) return false;
        GeometryBlob& blob = scene.geometryPool[idx];

        // Read the source .data (so sibling blocks are preserved), swap in this block's current geometry.
        DataFile data = readDataFile(blob.sourceDataPath);
        if (data.blocks.empty()) {
            core::error("persistChunkData: could not read source .data '{}' for chunk {}", blob.sourceDataPath, chunk.header.chunkID);
            return false;
        }
        bool replaced = false;
        for (DataBlock& b : data.blocks) {
            if (b.gridX == blob.sourceBlockCoord.x && b.gridY == blob.sourceBlockCoord.y &&
                b.gridZ == blob.sourceBlockCoord.z) {
                b.geometry = blob.geometry;
                b.voxelTypeData = blob.voxelTypeData;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            core::error("persistChunkData: block ({},{},{}) not found in '{}'", blob.sourceBlockCoord.x,
                        blob.sourceBlockCoord.y, blob.sourceBlockCoord.z, blob.sourceDataPath);
            return false;
        }

        std::string dest;
        if (chunk.mutability == Mutability::Direct) {
            dest = blob.sourceDataPath; // in place
        } else { // Copy
            if (blob.ownsSourceFile) {
                dest = blob.sourceDataPath; // already owns its copy file; write in place
            } else {
                // First persist of this copy: write a new file (never the inherited original), then own it.
                dest = copyDestPath;
                if (dest.empty()) {
                    std::filesystem::path src(blob.sourceDataPath);
                    dest = (src.parent_path() / (src.stem().string() + "_copy" +
                            std::to_string(chunk.header.chunkID) + ".data")).string();
                }
                blob.sourceDataPath = dest;
                blob.ownsSourceFile = true;
            }
        }
        writeDataFile(dest, data);
        core::info("persistChunkData: chunk {} ({}) persisted to '{}'", chunk.header.chunkID,
                   chunk.mutability == Mutability::Direct ? "direct" : "copy", dest);
        return true;
    }
}
