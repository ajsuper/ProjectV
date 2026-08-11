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
#include "utils/scene_query.h"
#include "nlohmann/json.hpp"

namespace projv::utils {
    namespace {
        constexpr char PVDT_MAGIC[4] = {'P', 'V', 'D', 'T'};
        constexpr uint32_t PVDT_VERSION = 2;
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
        const uint32_t flags = 0u;   // Reserved. v1's only flag said whether voxelTypeData was present.

        // Compute blob-region offsets. The blob starts right after the block table.
        uint64_t cursor = HEADER_SIZE + static_cast<uint64_t>(blockCount) * BLOCK_ENTRY_SIZE;
        struct BlobLoc { uint64_t geomOff; uint32_t geomLen; uint64_t matOff; uint32_t matLen; };
        std::vector<BlobLoc> locs(blockCount);
        for (uint32_t i = 0; i < blockCount; i++) {
            const DataBlock& b = data.blocks[i];
            locs[i].geomLen = static_cast<uint32_t>(b.geometry.size());
            locs[i].geomOff = cursor;
            cursor += static_cast<uint64_t>(locs[i].geomLen) * sizeof(uint32_t);

            if (!b.materialIDs.empty()) {
                // Bytes, not words: materialIDs is one uint8 per solid voxel.
                locs[i].matLen = static_cast<uint32_t>(b.materialIDs.size());
                locs[i].matOff = cursor;
                cursor += locs[i].matLen;
            } else {
                locs[i].matLen = 0;
                locs[i].matOff = 0;
            }
        }

        // Header.
        out.write(PVDT_MAGIC, 4);
        writePod(out, PVDT_VERSION);
        writePod(out, flags);
        writePod(out, blockCount);
        writePod(out, data.resolution);
        writePod(out, data.voxelScale);

        // Block table (40 bytes each: 3xint32 grid, 4 pad, u64 geomOff, u32 geomLen, u64 matOff, u32 matLen).
        for (uint32_t i = 0; i < blockCount; i++) {
            const DataBlock& b = data.blocks[i];
            writePod(out, b.gridX);
            writePod(out, b.gridY);
            writePod(out, b.gridZ);
            const uint32_t pad = 0;
            writePod(out, pad);
            writePod(out, locs[i].geomOff);
            writePod(out, locs[i].geomLen);
            writePod(out, locs[i].matOff);
            writePod(out, locs[i].matLen);
        }

        // Blob region, in the same order the offsets were computed.
        for (uint32_t i = 0; i < blockCount; i++) {
            const DataBlock& b = data.blocks[i];
            if (!b.geometry.empty()) {
                out.write(reinterpret_cast<const char*>(b.geometry.data()),
                          b.geometry.size() * sizeof(uint32_t));
            }
            if (!b.materialIDs.empty()) {
                out.write(reinterpret_cast<const char*>(b.materialIDs.data()), b.materialIDs.size());
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
        // Refused, not warned through. v1 stored per-voxel voxelTypeData and no material bytes, so
        // there is nothing to hand the GPU without rebuilding the whole material system at load --
        // which is exactly what v2 exists to stop doing. Re-voxelize v1 content to upgrade it.
        if (data.version != PVDT_VERSION) {
            core::error("readDataFile: {} is .data version {}, and only version {} is supported. "
                        "Re-voxelize this asset.", path, data.version, PVDT_VERSION);
            return {};
        }
        (void)readPod<uint32_t>(in);   // flags, reserved
        const uint32_t blockCount = readPod<uint32_t>(in);
        data.resolution = readPod<uint32_t>(in);
        data.voxelScale = readPod<float>(in);

        // Read the block table first, then seek to each blob.
        std::vector<BlockEntry> entries(blockCount);
        for (uint32_t i = 0; i < blockCount; i++) {
            entries[i].gridX = readPod<int32_t>(in);
            entries[i].gridY = readPod<int32_t>(in);
            entries[i].gridZ = readPod<int32_t>(in);
            (void)readPod<uint32_t>(in); // padding
            entries[i].geometryOffset = readPod<uint64_t>(in);
            entries[i].geometryLength = readPod<uint32_t>(in);
            entries[i].materialOffset = readPod<uint64_t>(in);
            entries[i].materialLength = readPod<uint32_t>(in);
        }

        data.blocks.resize(blockCount);
        for (uint32_t i = 0; i < blockCount; i++) {
            DataBlock& b = data.blocks[i];
            b.gridX = entries[i].gridX;
            b.gridY = entries[i].gridY;
            b.gridZ = entries[i].gridZ;

            if (entries[i].geometryLength > 0) {
                b.geometry.resize(entries[i].geometryLength);
                in.seekg(static_cast<std::streamoff>(entries[i].geometryOffset), std::ios::beg);
                in.read(reinterpret_cast<char*>(b.geometry.data()),
                        entries[i].geometryLength * sizeof(uint32_t));
            }
            if (entries[i].materialLength > 0) {
                b.materialIDs.resize(entries[i].materialLength);
                in.seekg(static_cast<std::streamoff>(entries[i].materialOffset), std::ios::beg);
                in.read(reinterpret_cast<char*>(b.materialIDs.data()), entries[i].materialLength);
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
            core::error("readDataFileHeader: {} is .data version {}, and only version {} is supported. "
                        "Re-voxelize this asset.", path, hdr.version, PVDT_VERSION);
            return {};
        }
        (void)readPod<uint32_t>(in);   // flags, reserved
        const uint32_t blockCount = readPod<uint32_t>(in);
        hdr.resolution = readPod<uint32_t>(in);
        hdr.voxelScale = readPod<float>(in);

        hdr.blocks.resize(blockCount);
        for (uint32_t i = 0; i < blockCount; i++) {
            BlockEntry& e = hdr.blocks[i];
            e.gridX = readPod<int32_t>(in);
            e.gridY = readPod<int32_t>(in);
            e.gridZ = readPod<int32_t>(in);
            (void)readPod<uint32_t>(in); // padding
            e.geometryOffset = readPod<uint64_t>(in);
            e.geometryLength = readPod<uint32_t>(in);
            e.materialOffset = readPod<uint64_t>(in);
            e.materialLength = readPod<uint32_t>(in);
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
        if (entry.materialLength > 0) {
            b.materialIDs.resize(entry.materialLength);
            in.seekg(static_cast<std::streamoff>(entry.materialOffset), std::ios::beg);
            in.read(reinterpret_cast<char*>(b.materialIDs.data()), entry.materialLength);
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

            // Absent means `none`, which is what every compose.json written before this field
            // existed means -- a pure placement list. An unrecognised value is warned about and read
            // as `none` rather than guessed at: reading it as a boolean would fold a component into
            // its parent on the strength of a typo, which is a destructive way to be wrong.
            std::string opStr = jc.value("op", std::string("none"));
            if (opStr == "union") c.op = BooleanOp::Union;
            else if (opStr == "subtract") c.op = BooleanOp::Subtract;
            else if (opStr == "intersect") c.op = BooleanOp::Intersect;
            else {
                if (opStr != "none") {
                    core::warn("parseComposeJson: '{}' has unknown op \"{}\" in {} - reading it as "
                               "\"none\" (placed)", c.source, opStr, composeJsonPath);
                }
                c.op = BooleanOp::None;
            }

            // The palette, in slot order -- the .data's material bytes are indices into it, so the
            // order is data and a reordering here recolours the geometry. Colours are [R, G, B] at
            // 10 bits each (0-1023), which is the precision Material::packedColor holds and the GPU
            // reads; writing them as three plain numbers keeps a hand-edit of a colour possible,
            // which a packed integer would not.
            if (jc.contains("palette") && jc["palette"].is_array()) {
                for (const auto& jm : jc["palette"]) {
                    Material material;
                    if (jm.contains("name") && jm["name"].is_string()) {
                        material.name = jm["name"].get<std::string>();
                    }
                    if (jm.contains("color") && jm["color"].is_array() && jm["color"].size() == 3) {
                        uint32_t red   = jm["color"][0].get<uint32_t>() & 0x3FFu;
                        uint32_t green = jm["color"][1].get<uint32_t>() & 0x3FFu;
                        uint32_t blue  = jm["color"][2].get<uint32_t>() & 0x3FFu;
                        material.packedColor = (red << 20) | (green << 10) | blue;
                    }

                    // The non-colour properties. Every one of them is optional and every one
                    // defaults to the value that makes the entry behave exactly as a colour-only
                    // entry always has (see the zero rule on Material in scene.h), so a palette
                    // written before these existed parses to the same bits it used to.
                    //
                    // They go out as floats in their natural units rather than as the packed bytes
                    // the GPU reads, for the same reason the colour goes out as three numbers: the
                    // file is meant to survive a hand-edit. The quantization back to bytes here is
                    // the same one the editor's sliders go through, so a load/save round trip is a
                    // fixed point rather than a slow drift.
                    if (jm.contains("emission") && jm["emission"].is_array() && jm["emission"].size() == 3) {
                        uint32_t red   = jm["emission"][0].get<uint32_t>() & 0x3FFu;
                        uint32_t green = jm["emission"][1].get<uint32_t>() & 0x3FFu;
                        uint32_t blue  = jm["emission"][2].get<uint32_t>() & 0x3FFu;
                        material.packedEmission = (red << 20) | (green << 10) | blue;
                    }
                    float glossiness   = jm.value("glossiness", 0.0f);
                    float metallic     = jm.value("metallic", 0.0f);
                    float transparency = jm.value("transparency", 0.0f);
                    float ior          = jm.value("ior", 1.0f);
                    material.packedSurface = packSurfaceWord(glossiness, metallic, transparency, ior);

                    float emissiveStrength = jm.value("emissiveStrength", 0.0f);
                    float transmission     = jm.value("transmission", 0.0f);
                    uint32_t flags         = jm.value("flags", 0u);
                    material.packedExtra = packExtraWord(emissiveStrength, transmission, flags);
                    if (c.palette.size() >= MAX_MATERIALS_PER_COMPONENT) {
                        core::warn("parseComposeJson: '{}' has more than {} materials in {} - "
                                   "the rest are dropped", c.source, MAX_MATERIALS_PER_COMPONENT,
                                   composeJsonPath);
                        break;
                    }
                    c.palette.push_back(std::move(material));
                }
            }

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
                // Carried straight through, so an assembly saved as a compose folder re-opens as the
                // editable stack of parts that produced it rather than as finished geometry. Nothing
                // in the loader acts on it -- resolving the fold is the editor's job -- so a runtime
                // that only places components reads a composed asset as its parts, which is the
                // degraded-but-coherent picture the default of None is chosen to give.
                rec.op             = c.op;
                // The palette the .data's material bytes index into. It travels in compose.json, so
                // it lands here directly rather than being interned voxel by voxel out of the geometry
                // -- and two components instancing one .data can carry different colours for it.
                rec.materialPalette = c.palette;
                // Version 1, not the default 0, matching what addComponent stamps on the default
                // palette it creates. A component that has a palette has had one set, so 0 -- the
                // "never touched" value -- was the wrong number to report regardless. It also
                // happened to be the number that made a freshly loaded scene's versions sum to
                // exactly what a fresh GPUData stores, which the palette upload read as "already
                // uploaded"; that guard is fixed at its own end in rebuildGlobalPaletteTexture, and
                // this is here because two paths that build a palette should agree on the count.
                rec.paletteVersion = 1;

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
                            // Both arrays go in as they came off disk. v1 had to rebuild the material
                            // system here -- intern every voxel's colour, regenerate the tree64, rebake
                            // the material bytes -- and v2 exists to delete exactly that: the file
                            // already holds what the GPU reads. The brick map stays null until the
                            // first edit asks for one (ensureBrickMapExists).
                            gb.geometry         = block.geometry;
                            gb.materialIDs      = block.materialIDs;
                            gb.sourceDataPath   = resolved;
                            gb.sourceBlockCoord = core::ivec3(block.gridX, block.gridY, block.gridZ);

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
                    // An `asset` entry *is* a reference: the file says "the contents of that folder
                    // go here". Marking it keeps that true across a save, so a document that
                    // referenced a shared asset still references it afterwards instead of being
                    // silently flattened into a private copy of it.
                    rec.externalSource = true;

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

    // --- Writing -------------------------------------------------------------------------------
    //
    // The inverse of everything above, and deliberately shaped as its mirror: writeComposeJson
    // answers parseComposeJson field for field, dataFileFromComponent answers the makeChunk lambda,
    // and saveComposeToDisk answers `expand`. A load followed by a save is meant to be a fixed point,
    // so wherever the two disagree the loader is the specification.

    namespace {
        // nlohmann's pretty printer puts every array element on its own line, which turns a colour
        // into six lines and a 256-entry palette into eighteen hundred. This collapses arrays whose
        // contents are purely numeric back onto one line, so `[512, 480, 470]` stays readable and the
        // arrays of *objects* -- components, the palette itself -- keep their indentation.
        //
        // A scanner rather than a regex, because a string literal in the document may contain
        // brackets and must not be looked inside.
        std::string compactNumericArrays(const std::string& text) {
            std::string out;
            out.reserve(text.size());

            for (size_t i = 0; i < text.size(); i++) {
                char c = text[i];
                if (c == '"') {
                    // Copy the whole literal, honouring backslash escapes.
                    size_t start = i++;
                    while (i < text.size() && text[i] != '"') i += (text[i] == '\\') ? 2 : 1;
                    out.append(text, start, std::min(i + 1, text.size()) - start);
                    continue;
                }
                if (c != '[') { out.push_back(c); continue; }

                // Look ahead for the matching ']' with nothing but numbers between.
                size_t scan = i + 1;
                bool numericOnly = true;
                while (scan < text.size() && text[scan] != ']') {
                    char inner = text[scan];
                    bool allowed = (inner >= '0' && inner <= '9') || inner == '-' || inner == '+' ||
                                   inner == '.' || inner == ',' || inner == 'e' || inner == 'E' ||
                                   inner == ' ' || inner == '\n' || inner == '\r' || inner == '\t';
                    if (!allowed) { numericOnly = false; break; }
                    scan++;
                }
                if (!numericOnly || scan >= text.size()) { out.push_back(c); continue; }

                out.push_back('[');
                bool pendingSpace = false;
                for (size_t inner = i + 1; inner < scan; inner++) {
                    char value = text[inner];
                    if (value == ' ' || value == '\n' || value == '\r' || value == '\t') {
                        pendingSpace = !out.empty() && out.back() != '[';
                        continue;
                    }
                    if (pendingSpace && value != ',') out.push_back(' ');
                    pendingSpace = false;
                    out.push_back(value);
                }
                out.push_back(']');
                i = scan;
            }
            return out;
        }
    }

    ComponentHandle instantiateComposeInto(Scene& scene, const std::string& folderPath,
                                           ComponentHandle parent,
                                           core::vec3 localPosition, core::quat localRotation,
                                           float localScale) {
        core::info("instantiateComposeInto: Grafting {} into the open scene", folderPath);

        if (parent != INVALID_COMPONENT_HANDLE &&
            (parent >= scene.components.size() ||
             scene.components[parent].kind != ComponentKind::Asset)) {
            core::error("instantiateComposeInto: parent {} is not an Asset component", parent);
            return INVALID_COMPONENT_HANDLE;
        }

        Scene loaded = loadComposeFromDisk(folderPath);
        if (loaded.components.empty()) {
            core::error("instantiateComposeInto: {} loaded no components", folderPath);
            return INVALID_COMPONENT_HANDLE;
        }

        // The bases every handle in the incoming scene is shifted by. Taken before anything is
        // appended, so they describe where the incoming rows will land rather than where they did.
        const uint32_t componentBase = static_cast<uint32_t>(scene.components.size());
        const uint32_t chunkBase     = static_cast<uint32_t>(scene.chunks.size());
        const int32_t  gridBase      = static_cast<int32_t>(scene.grids.size());

        // The node the whole folder hangs from, created first so it owns componentBase and every
        // incoming component lands after it.
        std::string name = std::filesystem::path(folderPath).filename().string();
        if (name.empty()) name = "Asset";
        ComponentHandle root = addComponent(scene, ComponentKind::Asset, name, parent, 4, 1.0f);
        if (root == INVALID_COMPONENT_HANDLE) {
            core::error("instantiateComposeInto: could not create the asset node");
            return INVALID_COMPONENT_HANDLE;
        }
        // addComponent may have grown `components`, so the incoming rows start after whatever it did.
        const uint32_t componentOffset = static_cast<uint32_t>(scene.components.size());
        (void)componentBase;

        // Geometry pool first: the chunks appended below point into it, and a blob copied in has to
        // already have its destination index before anything references it. Blobs are copied whole
        // (the copy constructor deep-copies the brick map), and their refCounts come across unchanged
        // because exactly the same set of incoming chunks will reference them.
        std::vector<int32_t> poolRemap(loaded.geometryPool.size(), -1);
        for (size_t index = 0; index < loaded.geometryPool.size(); index++) {
            GeometryBlob& source = loaded.geometryPool[index];
            if (source.refCount == 0) continue;   // A hole in the incoming pool; nothing points at it.
            source.dirty = true;                  // Nothing of it has reached this scene's GPU yet.
            poolRemap[index] = poolInsertBlob(scene, std::move(source));
        }

        auto remapComponent = [&](ComponentHandle handle) -> ComponentHandle {
            return handle == INVALID_COMPONENT_HANDLE ? INVALID_COMPONENT_HANDLE
                                                      : handle + componentOffset;
        };

        for (Chunk& chunk : loaded.chunks) {
            Chunk moved = std::move(chunk);
            if (moved.geometryPoolIndex >= 0 &&
                static_cast<size_t>(moved.geometryPoolIndex) < poolRemap.size()) {
                moved.geometryPoolIndex = poolRemap[moved.geometryPoolIndex];
            }
            moved.componentHandle = remapComponent(moved.componentHandle);
            if (moved.gridIndex >= 0) moved.gridIndex += gridBase;
            moved.headerDirty = true;
            scene.chunks.push_back(std::move(moved));
        }

        for (SceneGrid& grid : loaded.grids) {
            SceneGrid moved = std::move(grid);
            moved.componentHandle = remapComponent(moved.componentHandle);
            for (int32_t& cell : moved.cellToChunk) {
                if (cell >= 0) cell += static_cast<int32_t>(chunkBase);
            }
            scene.grids.push_back(std::move(moved));
        }

        for (ComponentRecord& record : loaded.components) {
            ComponentRecord moved = std::move(record);
            moved.parent = remapComponent(moved.parent);
            for (ComponentHandle& child : moved.children) child = remapComponent(child);
            if (moved.kind == ComponentKind::Chunk) moved.chunkHandle += chunkBase;
            if (moved.gridIndex >= 0) moved.gridIndex += gridBase;
            // dataRefID indexes the incoming scene's dataReferences, which are not carried across --
            // they are a lazily built cache, and the first edit of each component rebuilds its own.
            moved.dataRefID = -1;
            scene.components.push_back(std::move(moved));
        }

        for (ChunkHandle handle : loaded.looseChunks) {
            scene.looseChunks.push_back(handle + chunkBase);
        }
        scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());

        // The incoming roots become the new node's children. Everything below them already points at
        // its own parent, so only this one edge has to be made.
        size_t adopted = 0;
        for (size_t index = 0; index < loaded.components.size(); index++) {
            ComponentHandle handle = static_cast<ComponentHandle>(index) + componentOffset;
            if (scene.components[handle].parent != INVALID_COMPONENT_HANDLE) continue;
            scene.components[handle].parent = root;
            scene.components[root].children.push_back(handle);
            adopted++;
        }

        // Last, and through the ordinary setter: loadComposeFromDisk bakes world transforms into
        // every chunk header assuming its roots sit at the origin of the world, and this subtree no
        // longer does. setComponentTransform rebakes the whole subtree, which is exactly the
        // correction needed -- and it is needed even for an identity transform, because `parent` may
        // be somewhere else entirely.
        setComponentTransform(scene, root, localPosition, localRotation, localScale);

        core::info("instantiateComposeInto: Grafted '{}' as component {} - {} top-level, {} "
                   "component(s), {} chunk(s), {} grid(s)", name, root, adopted,
                   loaded.components.size(), loaded.chunks.size(), loaded.grids.size());
        return root;
    }

    bool writeComposeJson(const std::string& composeJsonPath, const ComposeDoc& doc) {
        core::info("writeComposeJson: Writing {} component(s) to {}", doc.components.size(), composeJsonPath);

        nlohmann::json json;
        json["version"] = COMPOSE_VERSION;
        json["name"] = doc.name;
        json["components"] = nlohmann::json::array();

        for (const ComposeComponent& c : doc.components) {
            nlohmann::json entry;
            entry["type"] = c.type == ComponentType::Data ? "data" : "asset";
            entry["source"] = c.source;
            if (!c.name.empty()) entry["name"] = c.name;
            entry["position"] = { c.position.x, c.position.y, c.position.z };
            // Four elements, so the parser takes it as a quaternion. Three would be read as Euler
            // degrees and would have to survive a conversion each way for no gain.
            entry["rotation"] = { c.rotation.x, c.rotation.y, c.rotation.z, c.rotation.w };
            if (c.scale.x == c.scale.y && c.scale.x == c.scale.z) {
                entry["scale"] = c.scale.x;
            } else {
                entry["scale"] = { c.scale.x, c.scale.y, c.scale.z };
            }
            switch (c.mutability) {
                case Mutability::Direct: entry["mutability"] = "direct"; break;
                case Mutability::Copy:   entry["mutability"] = "copy";   break;
                case Mutability::Locked: entry["mutability"] = "locked"; break;
            }
            // Written only when it is not the default, so a plain placement list comes back off this
            // writer looking exactly like the ones already on disk -- an `"op": "none"` on every
            // entry of every scene would be noise describing the absence of a feature.
            if (c.op != BooleanOp::None) entry["op"] = booleanOpName(c.op);
            // Slot order is the .data's material bytes' index space, so it is written verbatim --
            // including empty trailing slots, which still occupy an index.
            if (!c.palette.empty()) {
                nlohmann::json palette = nlohmann::json::array();
                for (const Material& material : c.palette) {
                    nlohmann::json materialJson;
                    if (!material.name.empty()) materialJson["name"] = material.name;
                    materialJson["color"] = { (material.packedColor >> 20) & 0x3FFu,
                                              (material.packedColor >> 10) & 0x3FFu,
                                              material.packedColor & 0x3FFu };
                    // Each property is written only when it is not its default, so a palette that
                    // is only colours writes byte-identical JSON to what this writer produced
                    // before the properties existed. Emitting `"glossiness": 0` on all 254 slots of
                    // every component would bury the fields that were actually set.
                    if (material.packedEmission != 0u) {
                        materialJson["emission"] = { (material.packedEmission >> 20) & 0x3FFu,
                                                     (material.packedEmission >> 10) & 0x3FFu,
                                                     material.packedEmission & 0x3FFu };
                    }
                    if (materialGlossiness(material) != 0.0f)   materialJson["glossiness"] = materialGlossiness(material);
                    if (materialMetallic(material) != 0.0f)     materialJson["metallic"] = materialMetallic(material);
                    if (materialTransparency(material) != 0.0f) materialJson["transparency"] = materialTransparency(material);
                    // IOR's default is 1.0 (no refraction), not 0 -- the packed byte is what is zero.
                    if ((material.packedSurface & 0xFFu) != 0u) materialJson["ior"] = materialIOR(material);
                    if (materialEmissiveStrength(material) != 0.0f) materialJson["emissiveStrength"] = materialEmissiveStrength(material);
                    if (materialTransmission(material) != 0.0f) materialJson["transmission"] = materialTransmission(material);
                    if (materialFlags(material) != 0u)          materialJson["flags"] = materialFlags(material);
                    palette.push_back(std::move(materialJson));
                }
                entry["palette"] = std::move(palette);
            }
            json["components"].push_back(std::move(entry));
        }

        std::filesystem::path parent = std::filesystem::path(composeJsonPath).parent_path();
        if (!parent.empty()) {
            std::error_code errorCode;
            std::filesystem::create_directories(parent, errorCode);
        }

        std::ofstream out(composeJsonPath);
        if (!out) {
            core::error("writeComposeJson: Failed to open {} for writing", composeJsonPath);
            return false;
        }
        out << compactNumericArrays(json.dump(2)) << "\n";
        if (!out) {
            core::error("writeComposeJson: Failed while writing {}", composeJsonPath);
            return false;
        }
        return true;
    }

    DataFile dataFileFromComponent(const Scene& scene, ComponentHandle handle) {
        DataFile data;
        if (handle >= scene.components.size()) return data;
        const ComponentRecord& comp = scene.components[handle];

        // Appends one chunk as a block, and takes the file-wide parameters off the first one seen --
        // every block of a .data shares a resolution and a voxelScale by the format's definition.
        auto appendBlock = [&](const Chunk& chunk, core::ivec3 gridCoord) {
            if (chunk.geometryPoolIndex < 0 ||
                static_cast<size_t>(chunk.geometryPoolIndex) >= scene.geometryPool.size()) {
                return;
            }
            const GeometryBlob& blob = scene.geometryPool[chunk.geometryPoolIndex];

            DataBlock block;
            block.gridX = gridCoord.x;
            block.gridY = gridCoord.y;
            block.gridZ = gridCoord.z;
            // A straight copy of the pair the blob already holds. Every edit path ends by rebaking
            // both out of the brick map (applyEditsToChunk), so what is in the blob is what the GPU is
            // rendering, and writing anything else would be writing something nobody has seen.
            block.geometry = blob.geometry;
            block.materialIDs = blob.materialIDs;

            if (block.geometry.empty()) return;

            if (data.resolution == 0) {
                data.resolution = chunk.header.resolution;
                // nativeScale is voxelScale * resolution with the ancestor chain's scale factored
                // out; header.voxelScale has that scale multiplied in. See the header comment.
                data.voxelScale = (chunk.nativeScale > 0.0f && chunk.header.resolution > 0)
                    ? chunk.nativeScale / static_cast<float>(chunk.header.resolution)
                    : chunk.header.voxelScale;
            }
            data.blocks.push_back(std::move(block));
        };

        if (comp.kind == ComponentKind::Chunk) {
            if (comp.chunkHandle < scene.chunks.size() && scene.chunks[comp.chunkHandle].alive) {
                appendBlock(scene.chunks[comp.chunkHandle], core::ivec3(0));
            }
        } else if (comp.kind == ComponentKind::Grid) {
            if (comp.gridIndex >= 0 && static_cast<size_t>(comp.gridIndex) < scene.grids.size()) {
                const SceneGrid& grid = scene.grids[static_cast<size_t>(comp.gridIndex)];
                for (size_t cell = 0; cell < grid.cellToChunk.size(); cell++) {
                    int32_t chunkIndex = grid.cellToChunk[cell];
                    if (chunkIndex < 0 || static_cast<size_t>(chunkIndex) >= scene.chunks.size()) continue;
                    if (!scene.chunks[chunkIndex].alive) continue;
                    // The inverse of the loader's linearisation:
                    // lin = x + dims.x * (y + dims.y * z).
                    int32_t linear = static_cast<int32_t>(cell);
                    core::ivec3 coord(linear % grid.dims.x,
                                      (linear / grid.dims.x) % grid.dims.y,
                                      linear / (grid.dims.x * grid.dims.y));
                    appendBlock(scene.chunks[chunkIndex], coord);
                }
            }
        }

        return data;
    }

    namespace {
        // A component name is a user-facing string; a filename is not. Anything outside the portable
        // set becomes an underscore, so a component called "Wall (north) #2" cannot produce a path the
        // loader has to quote or a platform refuses.
        std::string sanitizeForFilename(const std::string& name) {
            std::string safe;
            safe.reserve(name.size());
            for (char c : name) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
                safe.push_back(ok ? c : '_');
            }
            // Leading dots would make a hidden file, and an all-empty name would make none at all.
            size_t firstKept = safe.find_first_not_of('.');
            if (firstKept == std::string::npos) return "component";
            return safe.substr(firstKept);
        }
    }

    bool saveComposeToDisk(const Scene& scene, ComponentHandle root, const std::string& folderPath) {
        // Which components this folder's compose.json describes: an Asset's children, or every root
        // component when there is no Asset to stand in for the whole scene.
        std::vector<ComponentHandle> members;
        if (root == INVALID_COMPONENT_HANDLE) {
            for (ComponentHandle handle = 0; handle < scene.components.size(); handle++) {
                if (scene.components[handle].parent == INVALID_COMPONENT_HANDLE) members.push_back(handle);
            }
        } else if (root < scene.components.size()) {
            members = scene.components[root].children;
        } else {
            core::error("saveComposeToDisk: root handle {} is out of range", root);
            return false;
        }

        std::error_code errorCode;
        std::filesystem::create_directories(folderPath, errorCode);
        if (errorCode) {
            core::error("saveComposeToDisk: Could not create {}: {}", folderPath, errorCode.message());
            return false;
        }

        ComposeDoc doc;
        doc.version = COMPOSE_VERSION;
        doc.name = root == INVALID_COMPONENT_HANDLE
            ? std::filesystem::path(folderPath).filename().string()
            : scene.components[root].name;

        bool allWritten = true;
        std::unordered_set<std::string> usedNames;
        // Blob -> the .data already written for it. Two components sharing a geometry pool entry are
        // two instances of one thing, and writing the geometry twice would turn an instanced scene
        // into a duplicated one the next time it loaded.
        std::unordered_map<int32_t, std::string> blobToSource;

        for (ComponentHandle handle : members) {
            if (handle >= scene.components.size()) continue;
            const ComponentRecord& comp = scene.components[handle];
            // The editor's soft delete: the record survives so handles stay stable, but it is not
            // part of the scene any more and must not be written as though it were.
            if (comp.name == "__deleted__") continue;

            ComposeComponent entry;
            entry.name = comp.name;
            entry.position = comp.localPosition;
            entry.rotation = comp.localRotation;
            entry.scale = core::vec3(comp.localScale);

            // A grid whose origin cell has moved has to be re-anchored on the way out.
            //
            // dataFileFromComponent writes each block's *index* in cellToChunk as its grid
            // coordinate, and it has to: the coordinates in the file are offsets from the
            // component's position and the loader rejects negative ones, while a grid's absolute
            // cell coordinates go negative as soon as an edit expands it downward. So index zero is
            // what the file's (0,0,0) means -- and the position written beside it must therefore be
            // where index zero *is*, not where absolute cell zero is. Writing localPosition
            // unchanged claimed the latter, so every cell the grid had grown downward came back one
            // cell too high on the next load: a component extruded past its low edge reloaded with
            // its geometry displaced by originCellCoord * cellSize and its transform no longer over
            // the thing it moves.
            //
            // Into the parent's frame, which is what localPosition is measured in: the component's
            // own rotation and scale carry a content-space offset out to it, and nativeCellSize is
            // the cell size with the whole ancestor chain's scale already factored out (see
            // SceneGrid::nativeCellSize), so localScale is the only factor left to reapply.
            //
            // The component's transform origin therefore shifts across a save/load -- it re-anchors
            // from absolute cell zero to what used to be index zero. The geometry does not move,
            // which is the property that matters; the alternative is a file the loader refuses.
            if (comp.kind == ComponentKind::Grid && comp.gridIndex >= 0 &&
                static_cast<size_t>(comp.gridIndex) < scene.grids.size()) {
                const SceneGrid& grid = scene.grids[static_cast<size_t>(comp.gridIndex)];
                if (grid.originCellCoord != core::ivec3(0)) {
                    entry.position += glm::mat3_cast(comp.localRotation) *
                                      (core::vec3(grid.originCellCoord) * grid.nativeCellSize *
                                       comp.localScale);
                    core::trace("saveComposeToDisk: '{}' grid re-anchored by cell ({},{},{})",
                                comp.name, grid.originCellCoord.x, grid.originCellCoord.y,
                                grid.originCellCoord.z);
                }
            }
            // Per component, even where the geometry is shared: two instances of one .data each write
            // their own palette, which is what lets them be coloured apart.
            entry.palette = comp.materialPalette;
            // Every kind writes the op it carries, Grid included. This used to force `none` on a
            // Grid child on the grounds that the fold would refuse to honour it, and that made the
            // file a lossy record of a document the editor could express: a Grid row set to Subtract
            // reloaded as a placement, so the hole it cut was gone. The fold now walks a grid cell by
            // cell, so the promise is one that is kept.
            entry.op = comp.op;

            std::string base = sanitizeForFilename(comp.name.empty() ? "component" : comp.name);
            std::string unique = base;
            for (int suffix = 2; !usedNames.insert(unique).second; suffix++) {
                unique = base + "_" + std::to_string(suffix);
            }

            if (comp.kind == ComponentKind::Asset) {
                entry.type = ComponentType::Asset;
                // A *linked* asset is written as the bare reference it is, and its contents are not
                // descended into: the whole meaning of a link is that those contents belong to the
                // folder at sourcePath and are edited there. Rewriting them here would produce a
                // private copy under a name that still claims to be a reference -- the document would
                // look linked and behave copied, and nobody would find out until they edited the
                // original and nothing moved.
                if (comp.externalSource && !comp.sourcePath.empty()) {
                    // Relative whenever one can be formed, including one that climbs out to a
                    // sibling (`../assets/buttress`) -- that is the ordinary shape of a project tree
                    // and keeping it relative is what lets the whole tree be moved or checked out
                    // somewhere else. relative() returns empty when the two have no common root at
                    // all (a different drive), and only then is an absolute path the honest answer.
                    std::error_code relativeError;
                    std::filesystem::path relative = std::filesystem::relative(
                        comp.sourcePath, std::filesystem::absolute(folderPath), relativeError);
                    entry.source = (!relativeError && !relative.empty())
                        ? relative.generic_string()
                        : std::filesystem::path(comp.sourcePath).generic_string();
                    core::trace("saveComposeToDisk: '{}' is linked -> {}", comp.name, entry.source);
                } else {
                    entry.source = unique;
                    if (!saveComposeToDisk(scene, handle, (std::filesystem::path(folderPath) / unique).string())) {
                        allWritten = false;
                    }
                }
            } else {
                entry.type = ComponentType::Data;

                int32_t sharedBlob = -1;
                if (comp.kind == ComponentKind::Chunk && comp.chunkHandle < scene.chunks.size()) {
                    sharedBlob = scene.chunks[comp.chunkHandle].geometryPoolIndex;
                }
                auto shared = sharedBlob >= 0 ? blobToSource.find(sharedBlob) : blobToSource.end();
                if (shared != blobToSource.end()) {
                    entry.source = shared->second;
                    // The name was reserved and then not used for a file; give it back so the next
                    // component with this name does not get an unnecessary _2.
                    usedNames.erase(unique);
                } else {
                    DataFile data = dataFileFromComponent(scene, handle);
                    if (data.blocks.empty()) {
                        core::warn("saveComposeToDisk: '{}' has no geometry to write - skipping", comp.name);
                        usedNames.erase(unique);
                        continue;
                    }
                    entry.source = unique + ".data";
                    writeDataFile((std::filesystem::path(folderPath) / entry.source).string(), data);
                    if (sharedBlob >= 0) blobToSource.emplace(sharedBlob, entry.source);
                }

                if (comp.kind == ComponentKind::Chunk && comp.chunkHandle < scene.chunks.size()) {
                    entry.mutability = scene.chunks[comp.chunkHandle].mutability;
                }
            }

            doc.components.push_back(std::move(entry));
        }

        std::string composeJsonPath = (std::filesystem::path(folderPath) / "compose.json").string();
        if (!writeComposeJson(composeJsonPath, doc)) allWritten = false;

        core::info("saveComposeToDisk: Wrote {} component(s) to {}{}",
                   doc.components.size(), folderPath, allWritten ? "" : " (with errors)");
        return allWritten;
    }

    }
