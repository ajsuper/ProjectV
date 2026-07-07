#include "utils/compose_io.h"

#include <fstream>
#include <filesystem>
#include <functional>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <glm/gtc/quaternion.hpp>

#include "utils/voxel_management.h"
#include "nlohmann/json.hpp"

namespace projv::utils {
    namespace {
        constexpr char PVDT_MAGIC[4] = {'P', 'V', 'D', 'T'};
        constexpr uint32_t PVDT_VERSION = 1;
        constexpr uint32_t COMPOSE_VERSION = 1;
        constexpr int MAX_RECURSION_DEPTH = 32;

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

    Scene loadComposeFromDisk(const std::string& folderPath) {
        core::info("loadComposeFromDisk: Loading compose scene from folder: {}", folderPath);
        Scene scene;

        std::unordered_map<std::string, DataFile> dataCache;
        std::vector<std::string> folderStack;
        uint32_t nextChunkID = 0;

        std::function<void(const std::string&, const core::mat4&, int, bool)> expand =
            [&](const std::string& folder, const core::mat4& parentWorld, int depth, bool ancestorRotated) {
            if (depth > MAX_RECURSION_DEPTH) {
                core::error("loadComposeFromDisk: Exceeded max recursion depth ({}) at {}", MAX_RECURSION_DEPTH, folder);
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

                    auto cacheIt = dataCache.find(resolved);
                    if (cacheIt == dataCache.end()) {
                        cacheIt = dataCache.emplace(resolved, readDataFile(resolved)).first;
                    }
                    const DataFile& dataFile = cacheIt->second;

                    if (dataFile.resolution == 0 || dataFile.blocks.empty()) {
                        core::warn("loadComposeFromDisk: .data at {} is empty or invalid - skipping", resolved);
                        continue;
                    }

                    if (rotated) {
                        core::warn("loadComposeFromDisk: Non-identity rotation reached data leaf '{}'. "
                                   "The foundations render path ignores rotation; placing axis-aligned.", resolved);
                    }

                    // World translation and (assumed uniform) scale extracted from the matrix.
                    core::vec3 worldTranslation = core::vec3(world[3]);
                    float sx = core::length(core::vec3(world[0]));
                    float sy = core::length(core::vec3(world[1]));
                    float sz = core::length(core::vec3(world[2]));
                    if (std::abs(sx - sy) > 1e-4f || std::abs(sx - sz) > 1e-4f) {
                        core::warn("loadComposeFromDisk: Non-uniform scale on data leaf '{}' "
                                   "({}, {}, {}); using X axis as uniform scale.", resolved, sx, sy, sz);
                    }
                    float uniformScale = sx;

                    // Note: createChunkScaleFromVoxelScaleAndResolution multiplies by the raw
                    // resolution value (not its log2), matching how chunk scales are computed elsewhere.
                    float localBlockScale = createChunkScaleFromVoxelScaleAndResolution(
                        dataFile.voxelScale, static_cast<int>(dataFile.resolution));

                    for (const DataBlock& block : dataFile.blocks) {
                        Chunk chunk;
                        chunk.header.chunkID = nextChunkID++;
                        chunk.header.resolution = dataFile.resolution;
                        chunk.header.voxelScale = dataFile.voxelScale * uniformScale;
                        chunk.header.scale = localBlockScale * uniformScale;
                        chunk.header.position = worldTranslation
                            + core::vec3(block.gridX, block.gridY, block.gridZ) * localBlockScale * uniformScale;
                        chunk.geometryData = block.geometry;
                        chunk.voxelTypeData = block.voxelTypeData;
                        chunk.LOD = 0;
                        scene.chunks.push_back(std::move(chunk));
                    }
                } else { // Asset
                    std::string resolved = std::filesystem::weakly_canonical(
                        std::filesystem::path(folder) / c.source).string();

                    // Cycle detection: is this folder already being expanded above us?
                    if (std::find(folderStack.begin(), folderStack.end(), resolved) != folderStack.end()) {
                        std::string chain;
                        for (const auto& f : folderStack) chain += f + " -> ";
                        chain += resolved;
                        core::error("loadComposeFromDisk: Asset cycle detected, aborting branch. Chain: {}", chain);
                        continue;
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

        core::info("loadComposeFromDisk: Loaded {} chunk(s) from compose scene", scene.chunks.size());
        return scene;
    }
}
