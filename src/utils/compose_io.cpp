#include "utils/compose_io.h"
#include "utils/material.h"

#include <chrono>
#include <fstream>
#include <filesystem>
#include <functional>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/quaternion.hpp>

#include "utils/voxel_management.h"
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

            // Optional human-readable local name.
            if (jc.contains("name") && jc["name"].is_string()) {
                c.name = jc["name"].get<std::string>();
            }

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

    Scene loadComposeFromDisk(const std::string& folderPath) {
        core::info("loadComposeFromDisk: Loading compose scene from folder: {}", folderPath);
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

        // P6: expand now creates component records for every compose.json entry, including
        // Asset folders, and links parent/children. Chunks are created directly in scene.chunks.
        std::function<void(const std::string&, const core::mat4&,
                           ComponentHandle, int, std::unordered_set<std::string>)> expand =
            [&](const std::string& folder, const core::mat4& parentWorld,
                ComponentHandle parentHandle, int depth,
                std::unordered_set<std::string> siblingNames) {
            if (depth > MAX_RECURSION_DEPTH) {
                core::trace("loadComposeFromDisk: Recursion depth cap ({}) reached at {}", MAX_RECURSION_DEPTH, folder);
                return;
            }

            std::string composeJsonPath = (std::filesystem::path(folder) / "compose.json").string();
            core::trace("loadComposeFromDisk: processing folder=\"{}\" depth={}", folder, depth);
            ComposeDoc doc = parseComposeJson(composeJsonPath);
            if (doc.version == 0) {
                core::error("loadComposeFromDisk: Failed to load compose.json at {}", folder);
                return;
            }

            for (ComposeComponent& c : doc.components) {
                core::trace("loadComposeFromDisk:   component type={} source=\"{}\" name=\"{}\"",
                            c.type == ComponentType::Data ? "data" : "asset",
                            c.source, c.name.empty() ? "(auto)" : c.name);
                // P6.2b: Auto-generate name if absent.
                if (c.name.empty()) {
                    if (c.type == ComponentType::Data) {
                        std::filesystem::path p(c.source);
                        c.name = p.stem().string();
                    } else {
                        std::filesystem::path p(c.source);
                        c.name = p.filename().string();
                    }
                }
                // Disambiguate siblings at the same level.
                std::string disambiguated = c.name;
                for (int suffix = 2; !siblingNames.insert(disambiguated).second; ++suffix) {
                    disambiguated = c.name + "_" + std::to_string(suffix);
                }

                // Local transform: T * R * S (scale first, then rotate, then translate).
                core::mat4 local = glm::translate(core::mat4(1.0f), c.position)
                                 * glm::mat4_cast(c.rotation)
                                 * glm::scale(core::mat4(1.0f), c.scale);
                core::mat4 world = parentWorld * local;

                // P6.3b: Always create a ComponentRecord.
                ComponentHandle myHandle = static_cast<ComponentHandle>(scene.components.size());
                scene.components.push_back(ComponentRecord{});
                ComponentRecord& rec = scene.components.back();
                rec.name           = disambiguated;
                rec.sourcePath     = "";
                rec.localPosition  = c.position;
                rec.localRotation  = c.rotation;
                rec.localScale     = c.scale.x; // uniform scale, v0.0
                rec.parent         = parentHandle;

                // Link to parent.
                if (parentHandle != INVALID_COMPONENT_HANDLE) {
                    scene.components[parentHandle].children.push_back(myHandle);
                }

                if (c.type == ComponentType::Data) {
                    std::string resolved = std::filesystem::weakly_canonical(
                        std::filesystem::path(folder) / c.source).string();
                    rec.sourcePath = resolved;

                    auto cacheIt = dataCache.find(resolved);
                    if (cacheIt == dataCache.end()) {
                        cacheIt = dataCache.emplace(resolved, readDataFile(resolved)).first;
                    }
                    const DataFile& dataFile = cacheIt->second;

                    if (dataFile.resolution == 0 || dataFile.blocks.empty()) {
                        core::warn("loadComposeFromDisk: .data at {} is empty or invalid - skipping", resolved);
                        continue;
                    }

                    // Decompose the world matrix into scale (per basis vector) + pure rotation.
                    core::vec3 col0 = core::vec3(world[0]);
                    core::vec3 col1 = core::vec3(world[1]);
                    core::vec3 col2 = core::vec3(world[2]);
                    float sx = core::length(col0);
                    float sy = core::length(col1);
                    float sz = core::length(col2);
                    if (std::abs(sx - sy) > 1e-4f || std::abs(sx - sz) > 1e-4f) {
                        core::error("loadComposeFromDisk: Non-uniform scale on data leaf '{}' "
                                    "({}, {}, {}); rejected. Use uniform scale on data leaves (v0.0).",
                                    resolved, sx, sy, sz);
                        continue;
                    }
                    float uniformScale = sx;

                    core::mat3 rotMat = core::mat3(col0 / sx, col1 / sy, col2 / sz);
                    core::quat worldRotation = glm::quat_cast(rotMat);

                    float localBlockScale = createChunkScaleFromVoxelScaleAndResolution(
                        dataFile.voxelScale, static_cast<int>(dataFile.resolution));

                    auto makeChunk = [&](const DataBlock& block) {
                        Chunk chunk;
                        chunk.header.chunkID = nextChunkID++;
                        chunk.header.resolution = dataFile.resolution;
                        chunk.header.voxelScale = dataFile.voxelScale * uniformScale;
                        chunk.header.scale = localBlockScale * uniformScale;
                        chunk.nativeScale = localBlockScale;
                        core::vec3 blockLocalCorner =
                            core::vec3(block.gridX, block.gridY, block.gridZ) * localBlockScale;
                        chunk.header.position = core::vec3(world * core::vec4(blockLocalCorner, 1.0f));
                        chunk.header.rotation = worldRotation;
                        chunk.mutability = c.mutability;
                        std::string poolKey = resolved + "#" + std::to_string(block.gridX) + "_" +
                                              std::to_string(block.gridY) + "_" + std::to_string(block.gridZ) +
                                              "#" + std::to_string(static_cast<int>(c.mutability));
                        auto poolIt = poolKeyToIndex.find(poolKey);
                        if (poolIt == poolKeyToIndex.end()) {
                            int32_t idx = static_cast<int32_t>(geometryPool.size());
                            GeometryBlob gb;
                            gb.geometry         = block.geometry;
                            gb.sourceDataPath   = resolved;
                            gb.sourceBlockCoord = core::ivec3(block.gridX, block.gridY, block.gridZ);

                            // Convert old voxelTypeData to material format.
                            if (!block.voxelTypeData.empty()) {
                                core::ivec3 brickDims = computeBrickDims(dataFile.resolution);
                                auto brickMap = createVoxelBrickMap(brickDims);
                                brickMapFromVoxelTypeData(scene, *brickMap, block.voxelTypeData, rec);
                                gb.geometry = buildTree64FromBrickMap(*brickMap, static_cast<int>(dataFile.resolution));
                                gb.brickMap = std::move(brickMap);
                                bakeMaterialsFromBrickMap(gb.geometry, gb.materialIDs, *gb.brickMap);
                            } else {
                                gb.materialIDs.clear();
                            }

                            geometryPool.push_back(std::move(gb));
                            poolIt = poolKeyToIndex.emplace(poolKey, idx).first;
                        }
                        chunk.geometryPoolIndex = poolIt->second;
                        geometryPool[chunk.geometryPoolIndex].refCount++;
                        chunk.requestedLOD = 0;
                        return chunk;
                    };

                    // P6.3c: Create chunks directly in scene.chunks (tree order).
                    if (dataFile.blocks.size() > 1) {
                        rec.kind = ComponentKind::Grid;
                        rec.gridIndex = static_cast<int32_t>(scene.grids.size());

                        SceneGrid grid;
                        grid.origin    = core::vec3(world[3]);
                        grid.cellSize  = localBlockScale * uniformScale;
                        grid.nativeCellSize = localBlockScale;
                        grid.rotation  = worldRotation;
                        core::ivec3 dims(0);
                        for (const DataBlock& b : dataFile.blocks) {
                            dims.x = std::max(dims.x, b.gridX + 1);
                            dims.y = std::max(dims.y, b.gridY + 1);
                            dims.z = std::max(dims.z, b.gridZ + 1);
                        }
                        grid.dims = dims;
                        grid.cellToChunk.assign(
                            static_cast<size_t>(dims.x) * dims.y * dims.z, -1);
                        grid.componentHandle = myHandle;
                        grid.originCellCoord = core::ivec3(0);

                        for (const DataBlock& block : dataFile.blocks) {
                            if (block.gridX < 0 || block.gridY < 0 || block.gridZ < 0) {
                                core::warn("loadComposeFromDisk: negative grid coord in '{}' - skipping block", resolved);
                                continue;
                            }
                            int lin = block.gridX + dims.x * (block.gridY + dims.y * block.gridZ);
                            Chunk chunk = makeChunk(block);
                            chunk.gridIndex       = rec.gridIndex;
                            chunk.cellIndex       = lin;
                            chunk.componentHandle = myHandle;
                            grid.cellToChunk[lin] = static_cast<int32_t>(scene.chunks.size());
                            scene.chunks.push_back(std::move(chunk));
                        }
                        scene.grids.push_back(std::move(grid));
                    } else {
                        rec.kind = ComponentKind::Chunk;
                        for (const DataBlock& block : dataFile.blocks) {
                            Chunk chunk = makeChunk(block);
                            chunk.componentHandle = myHandle;
                            rec.chunkHandle = static_cast<ChunkHandle>(scene.chunks.size());
                            scene.chunks.push_back(std::move(chunk));
                        }
                    }
                } else { // Asset
                    rec.kind = ComponentKind::Asset;
                    std::string resolved = std::filesystem::weakly_canonical(
                        std::filesystem::path(folder) / c.source).string();
                    rec.sourcePath = resolved;

                    if (std::find(folderStack.begin(), folderStack.end(), resolved) != folderStack.end()) {
                        if (warnedCycles.insert(resolved).second) {
                            core::warn("loadComposeFromDisk: Cyclic asset dependency at '{}' — allowed, "
                                       "but recursion will be capped at depth {}.", resolved, MAX_RECURSION_DEPTH);
                        }
                    }

                    folderStack.push_back(resolved);
                    expand(resolved, world, myHandle, depth + 1,
                           std::unordered_set<std::string>{});
                    folderStack.pop_back();
                }
            }
        };

        std::string rootCanonical = std::filesystem::weakly_canonical(folderPath).string();
        folderStack.push_back(rootCanonical);
        expand(rootCanonical, core::mat4(1.0f), INVALID_COMPONENT_HANDLE, 0,
               std::unordered_set<std::string>{});
        folderStack.pop_back();

        // P6.3c: Rebuild loose list from tree (chunks with gridIndex < 0).
        for (ChunkHandle h = 0; h < scene.chunks.size(); ++h) {
            if (scene.chunks[h].alive && scene.chunks[h].gridIndex < 0)
                scene.looseChunks.push_back(h);
        }
        scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());

        // Pool indices are absolute and stable.
        scene.geometryPool = std::move(geometryPool);

        core::info("loadComposeFromDisk: Loaded {} chunk(s) ({} loose, {} grid(s), {} unique geometry blob(s), {} component(s)) from compose scene",
                   scene.chunks.size(), scene.looseChunkCount, scene.grids.size(),
                   scene.geometryPool.size(), scene.components.size());
        return scene;
    }

    }
