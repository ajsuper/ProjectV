#ifndef PROJECTV_MINECRAFT_IMPORT_HPP
#define PROJECTV_MINECRAFT_IMPORT_HPP

// Minecraft (Java Edition) world import.
//
// A saved world is not a mesh, it is already a voxel grid — so this path bypasses triangle
// rasterization entirely and produces colored voxels directly. One Minecraft block becomes one
// ProjectV voxel.
//
// The storage format is Anvil. A world directory holds `region/r.<X>.<Z>.mca` files, each covering
// 32x32 chunks (512x512 blocks). A region file opens with a 4 KiB location table of 1024 entries —
// a 3-byte sector offset and a 1-byte sector count each — followed by 4 KiB of timestamps. Each
// chunk is then a 4-byte length, a 1-byte compression id, and a compressed NBT document.
//
// Inside a chunk, blocks live in 16^3 sections, each carrying a *palette* of distinct block states
// and an array of indices into it, bit-packed into 64-bit words at ceil(log2(paletteSize)) bits per
// entry (minimum 4). Two details make this messier than it sounds, and both are handled below:
//
//   - Before 1.16 the indices ran as a continuous bit stream across word boundaries; from 1.16 on
//     each word is padded so no index spans two words. Which applies is decided by DataVersion.
//   - The chunk NBT was restructured in 1.18: sections moved from `Level.Sections` to `sections`,
//     and the palette from `Palette`/`BlockStates` to `block_states.palette`/`block_states.data`.
//
// Worlds predating 1.13 stored numeric block ids instead of a palette and are rejected with a clear
// message rather than being silently misread.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <zlib.h>

#include "core/log.h"
#include "core/math.h"
#include "utils/material.h"

#include "minecraft_blocks.hpp"
#include "nbt.hpp"
#include "stb_image.h"

namespace minecraft {

constexpr int SECTION_SIZE = 16;
constexpr int CHUNKS_PER_REGION_AXIS = 32;
constexpr int BLOCKS_PER_REGION_AXIS = CHUNKS_PER_REGION_AXIS * SECTION_SIZE;
constexpr int32_t FIRST_PALETTED_DATA_VERSION = 1451; // 1.13, where the block palette was introduced
constexpr int32_t PADDED_PACKING_DATA_VERSION = 2529; // 1.16, where indices stopped spanning words

struct ImportOptions {
    std::string dimension = "overworld";      // overworld | nether | end
    bool hasBounds = false;
    int minX = 0, minZ = 0, maxX = 0, maxZ = 0; // Inclusive block coordinates.
    int minY = -2048, maxY = 2047;
    int autoBoundsLimit = 512;                // Horizontal cap, in blocks, when bounds are inferred.
    size_t maxVoxels = 30000000;              // Refuses to allocate past this; ~500 MB of samples.
    std::filesystem::path resourcePack;       // Optional; sampled for exact block colors.
    bool includeWater = true;
};

struct Voxel {
    projv::core::ivec3 position; // Absolute world block coordinates.
    projv::Color color;
};

struct ImportedWorld {
    std::vector<Voxel> voxels;
    projv::core::ivec3 boundsMin{0, 0, 0};
    projv::core::ivec3 boundsMax{0, 0, 0};
    size_t regionsRead = 0;
    size_t chunksRead = 0;
    size_t chunksFailed = 0;
    size_t blocksSkipped = 0;         // Air and other blocks with nothing to draw.
    bool hitVoxelLimit = false;
    std::vector<std::string> unknownBlocks; // Distinct block names that resolved to no color.
    size_t texturesSampled = 0;
};

namespace detail {

/**
 * Inflates a zlib or gzip stream. The header is auto-detected, which covers both compression ids
 * region files use in practice.
 * @param data Compressed bytes.
 * @param size Number of compressed bytes.
 * @param out Receives the decompressed bytes.
 * @return bool True if the stream decompressed cleanly.
 */
inline bool inflateBuffer(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 32) != Z_OK) return false;

    stream.next_in = const_cast<Bytef*>(data);
    stream.avail_in = uInt(size);

    out.resize(std::max<size_t>(size * 8, 64 * 1024));
    size_t written = 0;
    int status = Z_OK;
    while (status == Z_OK) {
        if (written == out.size()) out.resize(out.size() * 2);
        stream.next_out = out.data() + written;
        stream.avail_out = uInt(out.size() - written);
        status = inflate(&stream, Z_NO_FLUSH);
        written = out.size() - stream.avail_out;
    }
    inflateEnd(&stream);

    out.resize(written);
    return status == Z_STREAM_END;
}

/**
 * Unpacks a section's bit-packed palette indices.
 * @param packed The section's data words.
 * @param bitsPerIndex Width of one index in bits.
 * @param padded True for 1.16+ packing, where indices never span two words.
 * @param indices Receives 4096 indices, ordered y-major then z then x.
 * @return bool True if the data held enough bits to fill the section.
 */
inline bool unpackIndices(const std::vector<int64_t>& packed, int bitsPerIndex, bool padded,
                          std::vector<uint16_t>& indices) {
    constexpr int BLOCKS_PER_SECTION = SECTION_SIZE * SECTION_SIZE * SECTION_SIZE;
    indices.assign(BLOCKS_PER_SECTION, 0);
    const uint64_t mask = (bitsPerIndex >= 64) ? ~0ull : ((1ull << bitsPerIndex) - 1);

    if (padded) {
        const int indicesPerWord = 64 / bitsPerIndex;
        if (size_t(packed.size()) * indicesPerWord < BLOCKS_PER_SECTION) return false;
        for (int block = 0; block < BLOCKS_PER_SECTION; block++) {
            int word = block / indicesPerWord;
            int shift = (block % indicesPerWord) * bitsPerIndex;
            indices[size_t(block)] = uint16_t((uint64_t(packed[size_t(word)]) >> shift) & mask);
        }
        return true;
    }

    // Pre-1.16: one continuous bit stream, so an index can straddle two words.
    if (packed.size() * 64 < size_t(BLOCKS_PER_SECTION) * size_t(bitsPerIndex)) return false;
    for (int block = 0; block < BLOCKS_PER_SECTION; block++) {
        size_t bitOffset = size_t(block) * size_t(bitsPerIndex);
        size_t word = bitOffset >> 6;
        int shift = int(bitOffset & 63);
        uint64_t value = uint64_t(packed[word]) >> shift;
        if (shift + bitsPerIndex > 64 && word + 1 < packed.size()) {
            value |= uint64_t(packed[word + 1]) << (64 - shift);
        }
        indices[size_t(block)] = uint16_t(value & mask);
    }
    return true;
}

inline int bitsNeededFor(size_t paletteSize) {
    int bits = 4;
    while ((size_t(1) << bits) < paletteSize) bits++;
    return bits;
}

/**
 * Strips the namespace from a block id ("minecraft:oak_log" -> "oak_log"). Modded blocks keep their
 * own namespace, which the resource-pack path needs, so the namespace is returned separately.
 */
inline std::string stripNamespace(const std::string& id, std::string& namespaceOut) {
    size_t colon = id.find(':');
    if (colon == std::string::npos) {
        namespaceOut = "minecraft";
        return id;
    }
    namespaceOut = id.substr(0, colon);
    return id.substr(colon + 1);
}

/**
 * Resolves block names to colors: resource pack first when one is supplied, then the built-in table
 * and naming rules. Results are cached, so each distinct block name costs one lookup per run.
 */
class BlockColorResolver {
public:
    BlockColorResolver(std::filesystem::path resourcePack, bool includeWater)
        : resourcePack_(std::move(resourcePack)), includeWater_(includeWater) {}

    /**
     * @param id Full block id, namespace included.
     * @param color Receives the block's color.
     * @return bool False if the block should not produce a voxel at all.
     */
    bool resolve(const std::string& id, projv::Color& color) {
        auto cached = cache_.find(id);
        if (cached != cache_.end()) {
            color = cached->second.color;
            return cached->second.visible;
        }

        std::string blockNamespace;
        std::string name = stripNamespace(id, blockNamespace);

        Entry entry;
        entry.visible = !isSkippedBlock(name) && (includeWater_ || (name != "water" && name != "bubble_column"));

        if (entry.visible && !sampleFromResourcePack(blockNamespace, name, entry.color)) {
            if (!resolveBlockColor(name, entry.color)) {
                entry.color = {128, 128, 128};
                unknownNames_.push_back(name);
            }
        }

        cache_.emplace(id, entry);
        color = entry.color;
        return entry.visible;
    }

    const std::vector<std::string>& unknownNames() const { return unknownNames_; }
    size_t texturesSampled() const { return texturesSampled_; }

private:
    struct Entry {
        projv::Color color{128, 128, 128};
        bool visible = true;
    };

    /**
     * Averages a block's texture from a resource pack. Textures are named by face, so the top face
     * is preferred (it is what a top-down look at the world shows) with the plain and side names as
     * fallbacks.
     */
    bool sampleFromResourcePack(const std::string& blockNamespace, const std::string& name,
                                projv::Color& color) {
        if (resourcePack_.empty()) return false;

        std::filesystem::path directory =
            resourcePack_ / "assets" / blockNamespace / "textures" / "block";
        const char* suffixes[] = {"", "_top", "_all", "_side", "_front", "_still"};
        for (const char* suffix : suffixes) {
            std::filesystem::path texture = directory / (name + suffix + ".png");
            std::error_code error;
            if (!std::filesystem::is_regular_file(texture, error)) continue;
            if (!averageTexture(texture, name, color)) continue;
            texturesSampled_++;
            return true;
        }
        return false;
    }

    bool averageTexture(const std::filesystem::path& path, const std::string& name,
                        projv::Color& color) {
        int width = 0, height = 0, channels = 0;
        unsigned char* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr) return false;

        // Animated textures (water, lava, fire) are stored as a vertical strip of frames; only the
        // first frame is representative, and the strip's later frames would skew the average.
        int sampledRows = (height > width && height % width == 0) ? width : height;

        uint64_t sumR = 0, sumG = 0, sumB = 0, opaque = 0;
        for (int y = 0; y < sampledRows; y++) {
            for (int x = 0; x < width; x++) {
                const unsigned char* texel = pixels + (size_t(y) * width + x) * 4;
                if (texel[3] < 32) continue; // Cutout pixels carry no color worth averaging.
                sumR += texel[0];
                sumG += texel[1];
                sumB += texel[2];
                opaque++;
            }
        }
        stbi_image_free(pixels);
        if (opaque == 0) return false;

        color = {uint8_t(sumR / opaque), uint8_t(sumG / opaque), uint8_t(sumB / opaque)};

        // Foliage and grass textures ship grayscale and are tinted by biome at render time. Without
        // that tint they average to a dull gray, so the default (plains) tint is applied here.
        static const char* TINTED[] = {"grass", "leaves", "vine", "fern", "lily_pad", "sugar_cane"};
        for (const char* tinted : TINTED) {
            if (name.find(tinted) == std::string::npos) continue;
            constexpr float TINT_R = 0x91 / 255.0f, TINT_G = 0xBD / 255.0f, TINT_B = 0x59 / 255.0f;
            color = {uint8_t(color.r * TINT_R), uint8_t(color.g * TINT_G), uint8_t(color.b * TINT_B)};
            break;
        }
        return true;
    }

    std::filesystem::path resourcePack_;
    bool includeWater_;
    std::unordered_map<std::string, Entry> cache_;
    std::vector<std::string> unknownNames_;
    size_t texturesSampled_ = 0;
};

struct RegionCoord {
    int x, z;
};

/**
 * Parses a region filename of the form `r.<X>.<Z>.mca`.
 * @param fileName The filename to parse.
 * @param coord Receives the region coordinate.
 * @return bool True if the name matched.
 */
inline bool parseRegionFileName(const std::string& fileName, RegionCoord& coord) {
    if (fileName.size() < 8 || fileName.compare(0, 2, "r.") != 0) return false;
    if (fileName.compare(fileName.size() - 4, 4, ".mca") != 0) return false;

    std::string middle = fileName.substr(2, fileName.size() - 6);
    size_t dot = middle.find('.');
    if (dot == std::string::npos) return false;

    try {
        coord.x = std::stoi(middle.substr(0, dot));
        coord.z = std::stoi(middle.substr(dot + 1));
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

/**
 * Reports a world stored in the pre-1.13 numeric-block-id format, which this importer does not read.
 * @param dataVersion The chunk's DataVersion, or 0 when the world predates the tag entirely (1.9).
 */
inline void reportLegacyWorld(int32_t dataVersion) {
    std::string version = dataVersion == 0 ? "no DataVersion tag, so older than 1.9"
                                           : ("DataVersion " + std::to_string(dataVersion));
    projv::core::error("This world predates Minecraft 1.13 ({}), which stored numeric block ids "
                       "instead of a block palette. Open it once in a modern Minecraft version to "
                       "convert it, then re-run.", version);
}

inline void reportMissingRegionDirectory(const std::filesystem::path& worldPath,
                                        const std::string& dimension) {
    projv::core::error("No region files found for dimension '{}' under '{}'. Point -f at a world "
                       "directory (the one holding level.dat), a region directory, or a single .mca file.",
                       dimension, worldPath.string());
}

/**
 * Locates the region directory for the requested dimension. Accepts a world directory, a `region`
 * directory, or a single `.mca` file, because all three are things a user reasonably points at.
 */
inline bool findRegionFiles(const std::filesystem::path& worldPath, const std::string& dimension,
                            std::vector<std::filesystem::path>& files) {
    using namespace projv::core;
    std::error_code error;

    if (std::filesystem::is_regular_file(worldPath, error) && worldPath.extension() == ".mca") {
        files.push_back(worldPath);
        return true;
    }

    std::vector<std::filesystem::path> candidates;
    if (worldPath.filename() == "region") {
        candidates.push_back(worldPath);
    } else {
        if (dimension == "nether") {
            candidates.push_back(worldPath / "DIM-1" / "region");
        } else if (dimension == "end") {
            candidates.push_back(worldPath / "DIM1" / "region");
        } else {
            candidates.push_back(worldPath / "region");
        }
    }

    for (const std::filesystem::path& directory : candidates) {
        if (!std::filesystem::is_directory(directory, error)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            RegionCoord coord;
            if (entry.is_regular_file(error) && parseRegionFileName(entry.path().filename().string(), coord)) {
                files.push_back(entry.path());
            }
        }
    }

    if (files.empty()) {
        reportMissingRegionDirectory(worldPath, dimension);
        return false;
    }
    std::sort(files.begin(), files.end());
    return true;
}

} // namespace detail

/**
 * Reads a Minecraft world into colored voxels.
 *
 * @param worldPath World directory, region directory, or a single .mca file.
 * @param options Selection bounds, dimension, and color sources.
 * @param world Receives the voxels and import statistics.
 * @return bool True if at least one block was read.
 */
inline bool importWorld(const std::filesystem::path& worldPath, const ImportOptions& options,
                        ImportedWorld& world) {
    using namespace projv::core;

    std::vector<std::filesystem::path> regionFiles;
    if (!detail::findRegionFiles(worldPath, options.dimension, regionFiles)) return false;
    info("  {} region file(s) found.", regionFiles.size());

    // Work out which blocks to read. Without explicit bounds the extent of the region files is used,
    // clamped to a box around its center — a saved world is routinely tens of thousands of blocks
    // across, which is not something anyone wants voxelized by accident.
    int minX = options.minX, maxX = options.maxX, minZ = options.minZ, maxZ = options.maxZ;
    if (!options.hasBounds) {
        int regionMinX = INT_MAX, regionMaxX = INT_MIN, regionMinZ = INT_MAX, regionMaxZ = INT_MIN;
        for (const std::filesystem::path& file : regionFiles) {
            detail::RegionCoord coord;
            if (!detail::parseRegionFileName(file.filename().string(), coord)) continue;
            regionMinX = std::min(regionMinX, coord.x * BLOCKS_PER_REGION_AXIS);
            regionMaxX = std::max(regionMaxX, coord.x * BLOCKS_PER_REGION_AXIS + BLOCKS_PER_REGION_AXIS - 1);
            regionMinZ = std::min(regionMinZ, coord.z * BLOCKS_PER_REGION_AXIS);
            regionMaxZ = std::max(regionMaxZ, coord.z * BLOCKS_PER_REGION_AXIS + BLOCKS_PER_REGION_AXIS - 1);
        }
        if (regionMinX > regionMaxX) return false;

        minX = regionMinX; maxX = regionMaxX;
        minZ = regionMinZ; maxZ = regionMaxZ;

        int limit = std::max(16, options.autoBoundsLimit);
        if (maxX - minX + 1 > limit || maxZ - minZ + 1 > limit) {
            int centerX = (minX + maxX) / 2, centerZ = (minZ + maxZ) / 2;
            minX = centerX - limit / 2; maxX = minX + limit - 1;
            minZ = centerZ - limit / 2; maxZ = minZ + limit - 1;
            projv::core::warn("World spans more than {} blocks per axis; importing a {}x{} block area "
                              "centered at ({}, {}). Use --mc-bounds to choose a different area.",
                              limit, limit, limit, centerX, centerZ);
        }
    }
    info("  Importing blocks X [{}, {}], Z [{}, {}], Y [{}, {}].", minX, maxX, minZ, maxZ,
        options.minY, options.maxY);

    detail::BlockColorResolver resolver(options.resourcePack, options.includeWater);

    ivec3 boundsMin{INT_MAX, INT_MAX, INT_MAX};
    ivec3 boundsMax{INT_MIN, INT_MIN, INT_MIN};
    bool reportedLegacyWorld = false;

    for (const std::filesystem::path& file : regionFiles) {
        detail::RegionCoord region;
        if (!detail::parseRegionFileName(file.filename().string(), region)) continue;

        // Skip regions that cannot overlap the selection.
        int regionMinX = region.x * BLOCKS_PER_REGION_AXIS;
        int regionMinZ = region.z * BLOCKS_PER_REGION_AXIS;
        if (regionMinX > maxX || regionMinX + BLOCKS_PER_REGION_AXIS - 1 < minX) continue;
        if (regionMinZ > maxZ || regionMinZ + BLOCKS_PER_REGION_AXIS - 1 < minZ) continue;

        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            projv::core::warn("  Could not open region file '{}'.", file.filename().string());
            continue;
        }
        std::vector<uint8_t> regionData((std::istreambuf_iterator<char>(stream)),
                                        std::istreambuf_iterator<char>());
        if (regionData.size() < 8192) {
            projv::core::warn("  Region file '{}' is truncated.", file.filename().string());
            continue;
        }
        world.regionsRead++;
        info("  Reading {} ...", file.filename().string());

        for (int localChunkZ = 0; localChunkZ < CHUNKS_PER_REGION_AXIS; localChunkZ++) {
            for (int localChunkX = 0; localChunkX < CHUNKS_PER_REGION_AXIS; localChunkX++) {
                int chunkBlockX = regionMinX + localChunkX * SECTION_SIZE;
                int chunkBlockZ = regionMinZ + localChunkZ * SECTION_SIZE;
                if (chunkBlockX > maxX || chunkBlockX + SECTION_SIZE - 1 < minX) continue;
                if (chunkBlockZ > maxZ || chunkBlockZ + SECTION_SIZE - 1 < minZ) continue;

                size_t tableIndex = size_t(localChunkX + localChunkZ * CHUNKS_PER_REGION_AXIS) * 4;
                uint32_t sectorOffset = (uint32_t(regionData[tableIndex]) << 16) |
                                        (uint32_t(regionData[tableIndex + 1]) << 8) |
                                         uint32_t(regionData[tableIndex + 2]);
                uint32_t sectorCount = regionData[tableIndex + 3];
                if (sectorOffset == 0 || sectorCount == 0) continue; // Chunk never generated.

                size_t chunkStart = size_t(sectorOffset) * 4096;
                if (chunkStart + 5 > regionData.size()) {
                    world.chunksFailed++;
                    continue;
                }
                uint32_t length = (uint32_t(regionData[chunkStart]) << 24) |
                                  (uint32_t(regionData[chunkStart + 1]) << 16) |
                                  (uint32_t(regionData[chunkStart + 2]) << 8) |
                                   uint32_t(regionData[chunkStart + 3]);
                uint8_t compression = regionData[chunkStart + 4];
                if (length == 0 || chunkStart + 4 + length > regionData.size()) {
                    world.chunksFailed++;
                    continue;
                }

                const uint8_t* payload = regionData.data() + chunkStart + 5;
                size_t payloadSize = length - 1;

                std::vector<uint8_t> decompressed;
                const uint8_t* nbtData = nullptr;
                size_t nbtSize = 0;
                if (compression == 1 || compression == 2) {
                    if (!detail::inflateBuffer(payload, payloadSize, decompressed)) {
                        world.chunksFailed++;
                        continue;
                    }
                    nbtData = decompressed.data();
                    nbtSize = decompressed.size();
                } else if (compression == 3) {
                    nbtData = payload;
                    nbtSize = payloadSize;
                } else {
                    world.chunksFailed++; // LZ4 and custom schemes are not handled.
                    continue;
                }

                nbt::Value root;
                if (!nbt::parse(nbtData, nbtSize, root)) {
                    world.chunksFailed++;
                    continue;
                }

                // 1.18 moved the section list to the root and renamed the palette fields; before
                // that everything lived under `Level`.
                const nbt::Compound* chunkRoot = &root.compound;
                const nbt::Value* level = nbt::find(root.compound, "Level", nbt::TagType::Compound);
                int32_t dataVersion = int32_t(nbt::number(root.compound, "DataVersion", 0));
                if (level != nullptr) chunkRoot = &level->compound;

                if (dataVersion != 0 && dataVersion < FIRST_PALETTED_DATA_VERSION) {
                    if (!reportedLegacyWorld) {
                        detail::reportLegacyWorld(dataVersion);
                        reportedLegacyWorld = true;
                    }
                    world.chunksFailed++;
                    continue;
                }

                const nbt::Value* sections = nbt::find(*chunkRoot, "sections", nbt::TagType::List);
                if (sections == nullptr) sections = nbt::find(*chunkRoot, "Sections", nbt::TagType::List);
                if (sections == nullptr) continue;

                // DataVersion itself only exists from 1.9 on, so a world old enough cannot be
                // identified by it at all — and those are exactly the worlds most likely to be
                // handed to this tool. Detect the legacy layout structurally instead: a section
                // carrying a `Blocks` byte array of numeric ids and no palette. Without this the
                // section loop below would find no palette, skip every section, and the run would
                // end with a misleading "no blocks found in the selected area".
                if (!reportedLegacyWorld && !sections->list.empty()) {
                    const nbt::Value& firstSection = sections->list[0];
                    if (firstSection.type == nbt::TagType::Compound &&
                        nbt::find(firstSection.compound, "Blocks", nbt::TagType::ByteArray) != nullptr &&
                        nbt::find(firstSection.compound, "Palette") == nullptr) {
                        detail::reportLegacyWorld(dataVersion);
                        reportedLegacyWorld = true;
                    }
                }
                if (reportedLegacyWorld) {
                    world.chunksFailed++;
                    continue;
                }

                world.chunksRead++;
                const bool paddedPacking = dataVersion >= PADDED_PACKING_DATA_VERSION;

                for (const nbt::Value& section : sections->list) {
                    if (section.type != nbt::TagType::Compound) continue;
                    int sectionY = int(nbt::number(section.compound, "Y", 0));
                    int sectionBaseY = sectionY * SECTION_SIZE;
                    if (sectionBaseY > options.maxY || sectionBaseY + SECTION_SIZE - 1 < options.minY) continue;

                    // Modern layout first, then the pre-1.18 field names.
                    const nbt::Value* palette = nullptr;
                    const nbt::Value* packed = nullptr;
                    const nbt::Value* blockStates =
                        nbt::find(section.compound, "block_states", nbt::TagType::Compound);
                    if (blockStates != nullptr) {
                        palette = nbt::find(blockStates->compound, "palette", nbt::TagType::List);
                        packed = nbt::find(blockStates->compound, "data", nbt::TagType::LongArray);
                    } else {
                        palette = nbt::find(section.compound, "Palette", nbt::TagType::List);
                        packed = nbt::find(section.compound, "BlockStates", nbt::TagType::LongArray);
                    }
                    if (palette == nullptr || palette->list.empty()) continue;

                    // Resolve the section's palette once, then index it per block.
                    std::vector<projv::Color> paletteColors(palette->list.size());
                    std::vector<bool> paletteVisible(palette->list.size(), false);
                    bool anyVisible = false;
                    for (size_t entry = 0; entry < palette->list.size(); entry++) {
                        const nbt::Value& block = palette->list[entry];
                        if (block.type != nbt::TagType::Compound) continue;
                        std::string id = nbt::text(block.compound, "Name");
                        if (id.empty()) continue;
                        projv::Color color;
                        paletteVisible[entry] = resolver.resolve(id, color);
                        paletteColors[entry] = color;
                        anyVisible = anyVisible || paletteVisible[entry];
                    }
                    if (!anyVisible) continue;

                    // A single-entry palette needs no index array: the whole section is that block.
                    std::vector<uint16_t> indices;
                    if (packed == nullptr || packed->longArray.empty()) {
                        if (palette->list.size() != 1) continue;
                        indices.assign(SECTION_SIZE * SECTION_SIZE * SECTION_SIZE, 0);
                    } else {
                        int bits = detail::bitsNeededFor(palette->list.size());
                        if (!detail::unpackIndices(packed->longArray, bits, paddedPacking, indices)) {
                            world.chunksFailed++;
                            continue;
                        }
                    }

                    for (int y = 0; y < SECTION_SIZE && !world.hitVoxelLimit; y++) {
                        int blockY = sectionBaseY + y;
                        if (blockY < options.minY || blockY > options.maxY) continue;
                        for (int z = 0; z < SECTION_SIZE && !world.hitVoxelLimit; z++) {
                            int blockZ = chunkBlockZ + z;
                            if (blockZ < minZ || blockZ > maxZ) continue;
                            for (int x = 0; x < SECTION_SIZE; x++) {
                                int blockX = chunkBlockX + x;
                                if (blockX < minX || blockX > maxX) continue;

                                uint16_t index = indices[size_t((y * SECTION_SIZE + z) * SECTION_SIZE + x)];
                                if (index >= paletteVisible.size() || !paletteVisible[index]) {
                                    world.blocksSkipped++;
                                    continue;
                                }
                                if (world.voxels.size() >= options.maxVoxels) {
                                    world.hitVoxelLimit = true;
                                    break;
                                }

                                world.voxels.push_back({{blockX, blockY, blockZ}, paletteColors[index]});
                                boundsMin = min(boundsMin, ivec3{blockX, blockY, blockZ});
                                boundsMax = max(boundsMax, ivec3{blockX, blockY, blockZ});
                            }
                        }
                    }
                    if (world.hitVoxelLimit) break;
                }
                if (world.hitVoxelLimit) break;
            }
            if (world.hitVoxelLimit) break;
        }
        if (world.hitVoxelLimit) break;
    }

    if (world.voxels.empty()) {
        projv::core::error("No blocks found in the selected area. Check --mc-bounds and --mc-dimension.");
        return false;
    }
    if (world.hitVoxelLimit) {
        projv::core::warn("Stopped at the {} voxel limit; the imported area is incomplete. Narrow the "
                          "selection with --mc-bounds or raise --mc-max-voxels.", options.maxVoxels);
    }

    world.boundsMin = boundsMin;
    world.boundsMax = boundsMax;
    world.texturesSampled = resolver.texturesSampled();
    world.unknownBlocks = resolver.unknownNames();

    return true;
}

} // namespace minecraft

#endif // PROJECTV_MINECRAFT_IMPORT_HPP
