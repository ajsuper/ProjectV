// ProjectV OBJ Voxelizer
// A command-line tool that converts Wavefront OBJ models into ProjectV's Compose scene format.
// It parses mesh geometry and material data, samples diffuse textures at each voxel's surface
// intersection, and writes the result as a single grid-volume `.data` container plus the
// `compose.json` that places it.
//
// ProjectV Engine Features Used:
//   - Core Math       : vec2/vec3/ivec3, dot, cross, min/max/clamp — used for intersection math and UV sampling
//   - Logging         : info/warn/error via spdlog wrapper for structured, leveled output
//   - Compose I/O     : writeDataFile (PVDT `.data` container) + a hand-written compose.json
//   - Voxel Mgmt      : ChunkHeader, createChunk, the brick map API (createVoxelBrickMap /
//                       brickMapSetVoxel), and updateChunkFromBrickMap to build the tree64
//   - Materials       : per-component material palettes — voxels carry an 8-bit material ID, so
//                       sampled colors are quantized into a palette the engine can hold (see below)
//   - Z-Order Indexing: createZOrderIndex / reverseZOrderIndex for Morton-coded spatial layout within chunks

#include "core/math.h"
#include "core/log.h"
#include "utils/voxel_management.h"
#include "utils/voxel_math.h"
#include "utils/material.h"
#include "utils/compose_io.h"
#include "nlohmann/json.hpp"

#define TINYOBJLOADER_IMPLEMENTATION
#include "include/tiny_obj_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "include/stb_image.h"

#include "include/CLI.hpp"

#include <algorithm>
#include <iostream>
#include <string>
using namespace projv::core;

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 texCoord;
    std::string texture;
    vec3 materialColor{200.0f, 200.0f, 200.0f}; // diffuse fallback (0-255 range)
};

struct Triangle {
    vec3 v0, v1, v2;
};

struct AABB {
    vec3 min;
    vec3 max;
};

bool triAABBIntersect(const Triangle& tri, const AABB& box) {
    // Compute box center and half extents
    vec3 c = (box.min + box.max) * 0.5f;
    vec3 e = (box.max - box.min) * 0.5f;

    vec3 triangleMax = max(max(tri.v0, tri.v1), tri.v2);
    vec3 triangleMin = min(min(tri.v0, tri.v1), tri.v2);

    // Early AABB vs AABB rejection
    if (triangleMax.x < box.min.x || triangleMin.x > box.max.x ||
        triangleMax.y < box.min.y || triangleMin.y > box.max.y ||
        triangleMax.z < box.min.z || triangleMin.z > box.max.z) {
        return false; // No overlap → no intersection
    }

    // Move triangle to box-centered space
    vec3 v0 = tri.v0 - c;
    vec3 v1 = tri.v1 - c;
    vec3 v2 = tri.v2 - c;

    // Triangle edges
    vec3 f0 = v1 - v0;
    vec3 f1 = v2 - v1;
    vec3 f2 = v0 - v2;

    // === 1. Cross product axes (9 tests) ===
    vec3 axes[9] = {
        cross(vec3(1,0,0), f0), cross(vec3(1,0,0), f1), cross(vec3(1,0,0), f2),
        cross(vec3(0,1,0), f0), cross(vec3(0,1,0), f1), cross(vec3(0,1,0), f2),
        cross(vec3(0,0,1), f0), cross(vec3(0,0,1), f1), cross(vec3(0,0,1), f2)
    };

    for (int i = 0; i < 9; ++i) {
        vec3 axis = axes[i];

        // Skip near-zero axis (parallel edges)
        if (length(axis) < 1e-8f) continue;

        float p0 = dot(v0, axis);
        float p1 = dot(v1, axis);
        float p2 = dot(v2, axis);

        float r = e.x * abs(axis.x) + e.y * abs(axis.y) + e.z * abs(axis.z);

        float minP = std::min({p0, p1, p2});
        float maxP = std::max({p0, p1, p2});

        if (minP > r || maxP < -r)
            return false;
    }

    // === 2. AABB face axes ===
    if (std::min({v0.x, v1.x, v2.x}) > e.x || std::max({v0.x, v1.x, v2.x}) < -e.x) return false;
    if (std::min({v0.y, v1.y, v2.y}) > e.y || std::max({v0.y, v1.y, v2.y}) < -e.y) return false;
    if (std::min({v0.z, v1.z, v2.z}) > e.z || std::max({v0.z, v1.z, v2.z}) < -e.z) return false;

    // === 3. Triangle normal ===
    vec3 normal = cross(f0, f1);

    float d = -dot(normal, v0);

    vec3 vmin, vmax;
    for (int i = 0; i < 3; ++i) {
        if (normal[i] > 0.0f) {
            vmin[i] = -e[i];
            vmax[i] =  e[i];
        } else {
            vmin[i] =  e[i];
            vmax[i] = -e[i];
        }
    }

    if (dot(normal, vmin) + d > 0.0f) return false;
    if (dot(normal, vmax) + d < 0.0f) return false;

    return true;
}

struct Texture {
    unsigned char *image;
    ivec2 size;
    uint channels;
};

// Fetches the color a barycentric position on a triangle resolves to: the diffuse texel it maps
// to, or the material's flat diffuse color when the triangle has no usable texture. Shared by the
// palette pre-pass and the voxel loop so both see exactly the same colors.
//
// `flipV` selects the texture-coordinate convention. OBJ nominally puts v=0 at the *bottom* of the
// image while stb_image hands back the top row first, so flipping is the right default — but plenty
// of models (particularly ones converted out of 3ds Max) carry unflipped coordinates, and sampling
// those with the flip on lands on whatever happens to occupy the mirrored half of the atlas.
projv::Color sampleFaceColor(const Texture* texture, bool flipV,
                             const vec2& texCoord0, const vec2& texCoord1, const vec2& texCoord2,
                             float bary0, float bary1, float bary2,
                             const vec3& fallbackColor) {
    if (texture == nullptr) {
        return {uint8_t(clamp(fallbackColor.x, 0.0f, 255.0f)),
                uint8_t(clamp(fallbackColor.y, 0.0f, 255.0f)),
                uint8_t(clamp(fallbackColor.z, 0.0f, 255.0f))};
    }

    vec2 uv = bary0 * texCoord0 + bary1 * texCoord1 + bary2 * texCoord2;
    if (flipV) uv.y = 1.0f - uv.y;

    // Wrap (fract-equivalent, handles tiling and values outside [0,1]).
    uv.x -= std::floor(uv.x);
    uv.y -= std::floor(uv.y);

    int tx = clamp(int(uv.x * texture->size.x), 0, texture->size.x - 1);
    int ty = clamp(int(uv.y * texture->size.y), 0, texture->size.y - 1);
    int index = (ty * texture->size.x + tx) * int(texture->channels);

    return {texture->image[index + 0], texture->image[index + 1], texture->image[index + 2]};
}

// ---- Material palette ------------------------------------------------------------------------
//
// Voxels no longer carry a color: a voxel stores an 8-bit material ID into its component's
// material palette, which is capped at MAX_MATERIALS_PER_COMPONENT entries (255 usable — 255 is
// INVALID_MATERIAL). loadComposeFromDisk interns every distinct color it finds in the .data into
// that one palette, for the whole model, so a textured mesh with thousands of distinct sampled
// colors would overflow it and be rejected. The voxelizer therefore has to hand the engine a color
// set that already fits.
//
// The palette is built by a cheap pre-pass over the parsed triangles, which samples each one the
// way voxelization will and weights it by 3D surface area — voxel counts follow area, so this
// approximates the distribution of colors that will actually reach a voxel. That matters more than
// it sounds: sampling whole texture images instead spends the budget on unused materials and on
// dead space in an atlas (one of the tree textures in the wild is >50% unsampled filler), which
// starves the colors the model really uses. Colors are accumulated into a 32^3 RGB histogram,
// median-cut down to PALETTE_CAPACITY boxes, and a 32^3 lookup table maps any sampled color to its
// palette slot in O(1) — so voxelization itself stays a single pass.

constexpr int PALETTE_CAPACITY = 255;                    // projv::MAX_MATERIALS_PER_COMPONENT - 1
constexpr int HISTOGRAM_BITS = 5;                        // bits kept per channel
constexpr int HISTOGRAM_AXIS = 1 << HISTOGRAM_BITS;      // 32 levels per channel
constexpr int HISTOGRAM_SIZE = HISTOGRAM_AXIS * HISTOGRAM_AXIS * HISTOGRAM_AXIS;
// Total surface samples spread across the mesh when building the palette. Large enough that even
// a small material gets a fair vote, small enough to stay far cheaper than voxelization itself.
constexpr int PALETTE_SAMPLE_BUDGET = 2000000;

inline int histogramIndex(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> (8 - HISTOGRAM_BITS)) << (2 * HISTOGRAM_BITS))
         | ((g >> (8 - HISTOGRAM_BITS)) << HISTOGRAM_BITS)
         |  (b >> (8 - HISTOGRAM_BITS));
}

// One populated histogram cell: how many source pixels landed in it and their color sum, so a
// merged box can report the true mean of its contents rather than the mean of its cells.
struct ColorBucket {
    uint64_t count = 0;
    uint64_t sumR = 0, sumG = 0, sumB = 0;
    uint8_t r = 0, g = 0, b = 0; // the cell's mean color; the median-cut split key
};

struct ColorPalette {
    std::vector<projv::Color> colors;
    std::vector<uint8_t> lookup; // HISTOGRAM_SIZE entries: quantized color -> palette slot

    uint8_t idFor(uint8_t r, uint8_t g, uint8_t b) const {
        return lookup[histogramIndex(r, g, b)];
    }
};

class ColorHistogram {
public:
    ColorHistogram() : cells_(HISTOGRAM_SIZE) {}

    void add(uint8_t r, uint8_t g, uint8_t b, uint64_t weight = 1) {
        ColorBucket& cell = cells_[histogramIndex(r, g, b)];
        cell.count += weight;
        cell.sumR += uint64_t(r) * weight;
        cell.sumG += uint64_t(g) * weight;
        cell.sumB += uint64_t(b) * weight;
    }

    // Drops the empty cells and stamps each survivor with its mean color.
    std::vector<ColorBucket> populatedCells() const {
        std::vector<ColorBucket> populated;
        for (const ColorBucket& cell : cells_) {
            if (cell.count == 0) continue;
            ColorBucket entry = cell;
            entry.r = uint8_t(entry.sumR / entry.count);
            entry.g = uint8_t(entry.sumG / entry.count);
            entry.b = uint8_t(entry.sumB / entry.count);
            populated.push_back(entry);
        }
        return populated;
    }

private:
    std::vector<ColorBucket> cells_;
};

// Median cut: start with one box holding every populated cell, then repeatedly split the box with
// the widest spread along that box's widest channel, at the pixel-count-weighted median, until the
// palette is full or nothing can be split further. Each surviving box contributes one palette entry
// equal to the mean color of the source pixels inside it.
ColorPalette buildColorPalette(std::vector<ColorBucket> cells) {
    ColorPalette palette;

    struct Box { size_t begin, end; };
    std::vector<Box> boxes;
    if (!cells.empty()) boxes.push_back({0, cells.size()});

    auto channelOf = [](const ColorBucket& cell, int axis) -> uint8_t {
        return axis == 0 ? cell.r : (axis == 1 ? cell.g : cell.b);
    };

    while (int(boxes.size()) < PALETTE_CAPACITY) {
        int targetBox = -1, targetAxis = 0, widestSpread = 0;
        for (size_t i = 0; i < boxes.size(); i++) {
            if (boxes[i].end - boxes[i].begin < 2) continue;
            uint8_t low[3] = {255, 255, 255}, high[3] = {0, 0, 0};
            for (size_t c = boxes[i].begin; c < boxes[i].end; c++) {
                for (int axis = 0; axis < 3; axis++) {
                    uint8_t value = channelOf(cells[c], axis);
                    low[axis] = std::min(low[axis], value);
                    high[axis] = std::max(high[axis], value);
                }
            }
            for (int axis = 0; axis < 3; axis++) {
                int spread = int(high[axis]) - int(low[axis]);
                if (spread > widestSpread) {
                    widestSpread = spread;
                    targetAxis = axis;
                    targetBox = int(i);
                }
            }
        }
        if (targetBox < 0) break; // every box is a single cell — the palette is as fine as the data

        Box box = boxes[targetBox];
        std::sort(cells.begin() + box.begin, cells.begin() + box.end,
                  [&](const ColorBucket& a, const ColorBucket& b) {
                      return channelOf(a, targetAxis) < channelOf(b, targetAxis);
                  });

        uint64_t total = 0;
        for (size_t c = box.begin; c < box.end; c++) total += cells[c].count;
        uint64_t running = 0;
        size_t split = box.begin + 1;
        for (size_t c = box.begin; c + 1 < box.end; c++) {
            running += cells[c].count;
            split = c + 1;
            if (running * 2 >= total) break;
        }

        boxes[targetBox] = {box.begin, split};
        boxes.push_back({split, box.end});
    }

    for (const Box& box : boxes) {
        uint64_t count = 0, sumR = 0, sumG = 0, sumB = 0;
        for (size_t c = box.begin; c < box.end; c++) {
            count += cells[c].count;
            sumR += cells[c].sumR;
            sumG += cells[c].sumG;
            sumB += cells[c].sumB;
        }
        if (count == 0) continue;
        palette.colors.push_back({uint8_t(sumR / count), uint8_t(sumG / count), uint8_t(sumB / count)});
    }
    if (palette.colors.empty()) palette.colors.push_back({200, 200, 200});

    // Resolve every quantized color to its nearest palette entry once, so the per-voxel lookup
    // during voxelization is a single array index.
    palette.lookup.assign(HISTOGRAM_SIZE, 0);
    for (int index = 0; index < HISTOGRAM_SIZE; index++) {
        int levelR = (index >> (2 * HISTOGRAM_BITS)) & (HISTOGRAM_AXIS - 1);
        int levelG = (index >> HISTOGRAM_BITS) & (HISTOGRAM_AXIS - 1);
        int levelB = index & (HISTOGRAM_AXIS - 1);
        int r = levelR * 255 / (HISTOGRAM_AXIS - 1);
        int g = levelG * 255 / (HISTOGRAM_AXIS - 1);
        int b = levelB * 255 / (HISTOGRAM_AXIS - 1);

        int bestSlot = 0, bestDistance = INT_MAX;
        for (size_t slot = 0; slot < palette.colors.size(); slot++) {
            int dr = r - int(palette.colors[slot].r);
            int dg = g - int(palette.colors[slot].g);
            int db = b - int(palette.colors[slot].b);
            int distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance) {
                bestDistance = distance;
                bestSlot = int(slot);
            }
        }
        palette.lookup[index] = uint8_t(bestSlot);
    }

    return palette;
}

// Flattens a finished brick map back into the .data container's `voxelTypeData` array: three
// uint32s per voxel — (chunk-space Z-order, R10G10B10 color, packed normal). This is what
// loadComposeFromDisk reads to rebuild the brick map and intern the material palette, so it is the
// on-disk home of the colors the palette IDs stand for. The normal slot is left at zero; the
// voxelizer stores no per-voxel normals and the renderer derives them from the tree64.
//
// Bricks are visited in ascending brick Z-order and, within a brick, in ascending local Z-order.
// Morton codes nest, so that visits voxels in ascending chunk-space Z-order — the order the format
// expects — without a sort.
std::vector<uint32_t> buildVoxelTypeData(const projv::VoxelBrickMap& brickMap, const ColorPalette& palette) {
    std::vector<uint32_t> voxelTypeData;

    for (uint32_t brickIndex = 0; brickIndex < brickMap.totalBricks; brickIndex++) {
        const projv::BrickData* brick = brickMap.bricks[brickIndex].get();
        if (brick == nullptr) continue;

        ivec3 brickCoord = projv::utils::reverseZOrderIndex(brickIndex);

        for (uint32_t row = 0; row < projv::BRICK_MASK_ROWS; row++) {
            uint64_t rowBits = brick->mask[row];
            while (rowBits != 0) {
                // Tree64 convention: bit 63 is Z-order position 0 within the row.
                int leadingZeros = __builtin_clzll(rowBits);
                rowBits &= ~(1ull << (63 - leadingZeros));

                uint32_t localZOrder = row * 64 + uint32_t(leadingZeros);
                ivec3 localPosition = projv::utils::reverseZOrderIndex(localZOrder);
                ivec3 voxelPosition = brickCoord * int(projv::BRICK_SIZE) + localPosition;

                uint8_t materialID = projv::utils::brickMapGetMaterial(*brick, localZOrder);
                projv::Color color = palette.colors[materialID < palette.colors.size() ? materialID : 0];

                uint32_t serializedColor = (uint32_t(color.r) * 4 << 20)
                                         | (uint32_t(color.g) * 4 << 10)
                                         | (uint32_t(color.b) * 4);

                voxelTypeData.push_back(uint32_t(projv::utils::createZOrderIndex(voxelPosition)));
                voxelTypeData.push_back(serializedColor);
                voxelTypeData.push_back(0);
            }
        }
    }

    return voxelTypeData;
}

void voxelizeObjFile(std::filesystem::path modelObjDirectory, std::string modelMtlDirectory, std::string textureDirectory, int voxelizationResolution, std::string outputDirectory, bool flipTextureV) {
    using namespace projv::core;
    info("--------------------------------------------");
    info("  ProjectV Voxelizer");
    info("  Input:      {}", modelObjDirectory.string());
    info("  Output:     {}", outputDirectory);
    info("  Resolution: {} (rounded up to whole chunks; chunks are 256^3, or 64^3 at r<=64)", voxelizationResolution);
    info("  Flip V:     {}", flipTextureV ? "yes (OBJ/OpenGL convention)" : "no");
    info("--------------------------------------------");

    info("Loading OBJ model...");
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    tinyobj::attrib_t attributes;
    std::string warnings;
    std::string errors;
    bool success = tinyobj::LoadObj(&attributes, &shapes, &materials, &warnings, &errors, modelObjDirectory.c_str(), modelMtlDirectory.c_str(), true);

    if (!success) {
        projv::core::error("Failed to load OBJ: {}", errors);
        return;
    }
    if (!warnings.empty()) {
        projv::core::warn("OBJ loader warnings: {}", warnings);
    }
    info("  {} shape(s), {} material(s) found.", shapes.size(), materials.size());

    // Pre-load all textures referenced by materials
    std::unordered_map<std::string, Texture> textures;
    std::filesystem::path textureDirPath = textureDirectory;
    int texturesLoaded = 0, texturesFailed = 0;
    if (!materials.empty()) info("Loading textures...");
    for (const auto& mat : materials) {
        std::string texName = mat.diffuse_texname;
        if (texName.empty() || textures.count(texName) > 0) continue;

        // Normalize path separators (handle Windows-style backslashes in .mtl files)
        std::string normalizedName = texName;
        std::replace(normalizedName.begin(), normalizedName.end(), '\\', '/');

        std::filesystem::path texPath = textureDirPath / normalizedName;
        int width, height, channels;
        Texture texture;
        texture.image = stbi_load(texPath.c_str(), &width, &height, &channels, 4);
        if (texture.image == nullptr) {
            projv::core::warn("  [MISSING] '{}' - {}", normalizedName, stbi_failure_reason());
            texture.size = {0, 0};
            texture.channels = 0;
            texturesFailed++;
        } else {
            texture.size = {width, height};
            texture.channels = 4;
            projv::core::info("  [OK] '{}' ({}x{})", normalizedName, width, height);
            texturesLoaded++;
        }
        textures[texName] = texture; // store even on failure so we don't retry
    }

    info("Parsing geometry...");
    std::vector<Vertex> vertices;

    // IMPORTANT: initialize AABB correctly
    vec3 verticesMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    vec3 verticesMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (const auto& shape : shapes) {
        const auto& mesh = shape.mesh;

        size_t indexOffset = 0;

        for (size_t f = 0; f < mesh.num_face_vertices.size(); f++) {
            int fv = mesh.num_face_vertices[f];
            int mat_id = mesh.material_ids[f];

            std::string tex = "";
            projv::core::vec3 matDiffuse = {200.0f, 200.0f, 200.0f};
            if (mat_id >= 0 && mat_id < (int)materials.size()) {
                tex = materials[mat_id].diffuse_texname;
                matDiffuse = {
                    materials[mat_id].diffuse[0] * 255.0f,
                    materials[mat_id].diffuse[1] * 255.0f,
                    materials[mat_id].diffuse[2] * 255.0f
                };
            }

            if (fv < 3) {
                indexOffset += fv; // skip degenerate faces
                continue;
            }

            auto getVertex = [&](const tinyobj::index_t& index) -> Vertex {
                projv::core::vec3 position = {
                    attributes.vertices[3 * index.vertex_index + 0],
                    attributes.vertices[3 * index.vertex_index + 1],
                    attributes.vertices[3 * index.vertex_index + 2]
                };
                projv::core::vec3 normal = {0, 0, 0};
                if (index.normal_index >= 0) {
                    normal = {
                        attributes.normals[3 * index.normal_index + 0],
                        attributes.normals[3 * index.normal_index + 1],
                        attributes.normals[3 * index.normal_index + 2]
                    };
                }

                projv::core::vec2 texCoord = {0, 0};
                if (index.texcoord_index >= 0) {
                    texCoord = {
                        attributes.texcoords[2 * index.texcoord_index + 0],
                        attributes.texcoords[2 * index.texcoord_index + 1]
                    };
                }

                return {position, normal, texCoord};
            };

            // Triangulate faces with more than 3 vertices (fan triangulation)
            for (int v = 1; v < fv - 1; v++) {
                const tinyobj::index_t& i0 = mesh.indices[indexOffset + 0];
                const tinyobj::index_t& i1 = mesh.indices[indexOffset + v];
                const tinyobj::index_t& i2 = mesh.indices[indexOffset + v + 1];

                Vertex vert0 = getVertex(i0);
                Vertex vert1 = getVertex(i1);
                Vertex vert2 = getVertex(i2);

                verticesMin = projv::core::min(verticesMin, vert0.position);
                verticesMin = projv::core::min(verticesMin, vert1.position);
                verticesMin = projv::core::min(verticesMin, vert2.position);
                verticesMax = projv::core::max(verticesMax, vert0.position);
                verticesMax = projv::core::max(verticesMax, vert1.position);
                verticesMax = projv::core::max(verticesMax, vert2.position);

                vert0.texture = tex;  vert0.materialColor = matDiffuse;
                vert1.texture = tex;  vert1.materialColor = matDiffuse;
                vert2.texture = tex;  vert2.materialColor = matDiffuse;

                vertices.push_back(vert0);
                vertices.push_back(vert1);
                vertices.push_back(vert2);
            }

            indexOffset += fv;
        }
    }

    size_t triangleCount = vertices.size() / 3;
    if (triangleCount == 0) {
        projv::core::error("No triangles found in model. Aborting.");
        return;
    }
    info("  {} triangle(s) parsed.", triangleCount);

    // Build the material palette before voxelizing — every voxel written below needs a palette ID,
    // and the palette has to fit inside the engine's per-component cap. Triangles are sampled in
    // proportion to their surface area, which is what voxel counts follow.
    info("Building material palette...");
    ColorHistogram histogram;
    std::vector<float> triangleAreas(triangleCount);
    double totalArea = 0.0;
    for (size_t triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++) {
        const vec3& p0 = vertices[triangleIndex * 3 + 0].position;
        const vec3& p1 = vertices[triangleIndex * 3 + 1].position;
        const vec3& p2 = vertices[triangleIndex * 3 + 2].position;
        float area = 0.5f * length(cross(p1 - p0, p2 - p0));
        triangleAreas[triangleIndex] = area;
        totalArea += area;
    }

    auto textureFor = [&](const std::string& name) -> const Texture* {
        if (name.empty()) return nullptr;
        auto it = textures.find(name);
        if (it == textures.end() || it->second.image == nullptr) return nullptr;
        return &it->second;
    };

    for (size_t triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++) {
        const Vertex& v0 = vertices[triangleIndex * 3 + 0];
        const Vertex& v1 = vertices[triangleIndex * 3 + 1];
        const Vertex& v2 = vertices[triangleIndex * 3 + 2];
        const Texture* texture = textureFor(v0.texture);

        int sampleCount = 1;
        if (totalArea > 0.0) {
            double share = double(triangleAreas[triangleIndex]) / totalArea;
            sampleCount = std::clamp(int(std::lround(share * PALETTE_SAMPLE_BUDGET)), 1, 4096);
        }
        // A low-discrepancy sweep over the triangle: cheap, deterministic, and it spreads the
        // samples across the face instead of clumping at the centroid.
        for (int sample = 0; sample < sampleCount; sample++) {
            float u = (sample + 0.5f) / float(sampleCount);
            float v = std::fmod(u * 0.61803399f, 1.0f); // golden-ratio stride
            if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; } // fold back into the triangle
            projv::Color color = sampleFaceColor(texture, flipTextureV,
                                                 v0.texCoord, v1.texCoord, v2.texCoord,
                                                 1.0f - u - v, u, v, v0.materialColor);
            histogram.add(color.r, color.g, color.b);
        }
    }
    ColorPalette palette = buildColorPalette(histogram.populatedCells());
    info("  {} material(s) in palette (cap {}).", palette.colors.size(), PALETTE_CAPACITY);

    vec3 modelExtents = verticesMax - verticesMin;
    float largestDistanceTotal = max(max(modelExtents.x, modelExtents.y), modelExtents.z);
    info("  Bounding box:  ({:.3f}, {:.3f}, {:.3f})  ->  ({:.3f}, {:.3f}, {:.3f})",
        verticesMin.x, verticesMin.y, verticesMin.z,
        verticesMax.x, verticesMax.y, verticesMax.z);
    info("  Extents (W/H/D): {:.3f} x {:.3f} x {:.3f}  (largest axis: {:.3f})",
        modelExtents.x, modelExtents.y, modelExtents.z, largestDistanceTotal);

    // A chunk's resolution must be a power of 4 (that is what sets the tree64's depth) and, below
    // 256, must also be a whole brick: the brick map's per-row bitmasks ARE the tree64's leaf level,
    // so a chunk smaller than one BRICK_SIZE^3 brick would leave the tree the wrong depth. 64 is
    // therefore the smallest chunk this can emit, and small models get a single 64^3 chunk instead
    // of being padded out to a mostly-empty 256^3 one.
    uint chunkResolution = (voxelizationResolution <= 64) ? 64u : 256u;
    uint numberOfChunksPerAxis = uint(std::ceil(voxelizationResolution / float(chunkResolution)));
    float voxelScale = 1.0f;                                    // Each voxel is exactly 1 world unit.
    float chunkScale = voxelScale * float(chunkResolution);     // World units per chunk.
    float totalWorldSize = numberOfChunksPerAxis * chunkScale;  // World span of the grid along the largest axis.
    // Normalize the model so its largest extent fills the grid while keeping voxels unit-sized.
    // (Position/voxelScale/scale are then all in the same world units — 1 unit = 1 voxel.)
    float modelScaleFactor = largestDistanceTotal > 0.0f ? totalWorldSize / largestDistanceTotal : 1.0f;

    for (size_t vertexIndex = 0; vertexIndex < vertices.size(); vertexIndex++) {
        vertices[vertexIndex].position = (vertices[vertexIndex].position - verticesMin) * modelScaleFactor;
    }

    uint totalChunks = numberOfChunksPerAxis * numberOfChunksPerAxis * numberOfChunksPerAxis;

    info("Voxelizing: {} chunk(s) ({} per axis), voxel size: {:.5f}", totalChunks, numberOfChunksPerAxis, voxelScale);

    size_t totalVoxels = 0;
    uint logStep = std::max(1u, totalChunks / 10);

    // Accumulate every occupied chunk into a single grid-volume .data container.
    projv::DataFile dataFile;
    dataFile.resolution = chunkResolution;
    dataFile.voxelScale = voxelScale;
    dataFile.hasVoxelTypeData = true;

    for (int chunkIndex = 0; chunkIndex < (int)totalChunks; chunkIndex++) {
        if (totalChunks == 1 || chunkIndex % logStep == 0) {
            info("  Chunk {}/{} ({:.0f}%)", chunkIndex + 1, totalChunks,
                100.0f * chunkIndex / totalChunks);
        }
        projv::core::ivec3 chunkIndexPosition = projv::utils::reverseZOrderIndex(chunkIndex);
        projv::ChunkHeader chunkHeader;
        chunkHeader.chunkID = chunkIndex;
        chunkHeader.position = vec3(chunkIndexPosition) * vec3(chunkScale);
        chunkHeader.voxelScale = voxelScale;
        chunkHeader.resolution = chunkResolution;
        chunkHeader.scale = projv::utils::createChunkScaleFromVoxelScaleAndResolution(voxelScale, int(chunkResolution));
        auto brickMap = projv::utils::createVoxelBrickMap(
            projv::utils::computeBrickDims(chunkResolution));

        for (size_t triangleIndex = 0; triangleIndex < vertices.size() / 3; triangleIndex++) {
            const vec3& p0 = vertices[triangleIndex * 3].position;
            const vec3& p1 = vertices[triangleIndex * 3 + 1].position;
            const vec3& p2 = vertices[triangleIndex * 3 + 2].position;
            Triangle tri{p0, p1, p2};

            vec2 texCoordsV0 = vertices[triangleIndex * 3].texCoord;
            vec2 texCoordsV1 = vertices[triangleIndex * 3 + 1].texCoord;
            vec2 texCoordsV2 = vertices[triangleIndex * 3 + 2].texCoord;
            const std::string& textureName = vertices[triangleIndex * 3].texture;
            const vec3& fallbackColor = vertices[triangleIndex * 3].materialColor;

            const Texture* texture = textureFor(textureName);

            // Precompute barycentric denominator for this triangle
            vec3 edge0 = p1 - p0;
            vec3 edge1 = p2 - p0;
            float d00 = dot(edge0, edge0);
            float d01 = dot(edge0, edge1);
            float d11 = dot(edge1, edge1);
            float baryDenom = d00 * d11 - d01 * d01;
            bool degenerate = (std::abs(baryDenom) < 1e-10f);

            AABB chunkAABB;
            chunkAABB.min = chunkHeader.position;
            chunkAABB.max = chunkHeader.position + vec3(chunkScale);

            vec3 triMin = min(min(tri.v0, tri.v1), tri.v2);
            vec3 triMax = max(max(tri.v0, tri.v1), tri.v2);

            if (triMax.x < chunkAABB.min.x || triMin.x > chunkAABB.max.x ||
                triMax.y < chunkAABB.min.y || triMin.y > chunkAABB.max.y ||
                triMax.z < chunkAABB.min.z || triMin.z > chunkAABB.max.z) {
                continue;
            }

            ivec3 minVoxel = ivec3(floor((triMin - chunkHeader.position) / voxelScale));
            ivec3 maxVoxel = ivec3(ceil((triMax - chunkHeader.position) / voxelScale));
            minVoxel = clamp(minVoxel, ivec3(0), ivec3(int(chunkResolution) - 1));
            maxVoxel = clamp(maxVoxel, ivec3(0), ivec3(int(chunkResolution) - 1));

            for (int z = minVoxel.z; z <= maxVoxel.z; z++) {
                for (int y = minVoxel.y; y <= maxVoxel.y; y++) {
                    for (int x = minVoxel.x; x <= maxVoxel.x; x++) {
                        vec3 voxelMin = chunkHeader.position + vec3(x, y, z) * voxelScale;
                        AABB voxelAABB;
                        voxelAABB.min = voxelMin;
                        voxelAABB.max = voxelMin + vec3(voxelScale);

                        if (!triAABBIntersect(tri, voxelAABB)) continue;

                        // Interpolate UV at the voxel center (not the triangle centroid).
                        float bary0 = 1.0f, bary1 = 0.0f, bary2 = 0.0f;
                        if (!degenerate) {
                            vec3 voxelCenter = (voxelAABB.min + voxelAABB.max) * 0.5f;
                            vec3 v0p = voxelCenter - p0;
                            float d20 = dot(v0p, edge0);
                            float d21 = dot(v0p, edge1);

                            bary1 = (d11 * d20 - d01 * d21) / baryDenom;
                            bary2 = (d00 * d21 - d01 * d20) / baryDenom;
                            bary0 = 1.0f - bary1 - bary2;

                            // Clamp to valid barycentric range and renormalize
                            bary0 = std::max(0.0f, bary0);
                            bary1 = std::max(0.0f, bary1);
                            bary2 = std::max(0.0f, bary2);
                            float barySum = bary0 + bary1 + bary2;
                            if (barySum > 1e-6f) {
                                bary0 /= barySum; bary1 /= barySum; bary2 /= barySum;
                            }
                        }

                        projv::Color color = sampleFaceColor(degenerate ? nullptr : texture,
                            flipTextureV, texCoordsV0, texCoordsV1, texCoordsV2,
                            bary0, bary1, bary2, fallbackColor);

                        projv::utils::brickMapSetVoxel(*brickMap, x, y, z,
                            palette.idFor(color.r, color.g, color.b));
                    }
                }
            }
        }

        // Count voxels from the brick map
        size_t chunkVoxels = 0;
        for (uint32_t bz = 0; bz < brickMap->totalBricks; ++bz) {
            if (!brickMap->bricks[bz]) continue;
            for (uint32_t row = 0; row < projv::BRICK_MASK_ROWS; ++row)
                chunkVoxels += __builtin_popcountll(brickMap->bricks[bz]->mask[row]);
        }
        totalVoxels += chunkVoxels;

        // Skip chunks that received no geometry.
        if (chunkVoxels == 0) {
            continue;
        }

        projv::Chunk chunk = projv::utils::createChunk(chunkHeader);
        projv::utils::updateChunkFromBrickMap(chunk, *brickMap);

        projv::DataBlock block;
        block.gridX = chunkIndexPosition.x;
        block.gridY = chunkIndexPosition.y;
        block.gridZ = chunkIndexPosition.z;
        // The tree64 is written unbaked (leaf nodes carry no material offsets): material offsets
        // index a materialIDs array, which the .data container does not store. loadComposeFromDisk
        // rebuilds both the tree64 and the material offsets from voxelTypeData below.
        block.geometry = std::move(chunk.geometryData);
        block.voxelTypeData = buildVoxelTypeData(*brickMap, palette);
        dataFile.blocks.push_back(std::move(block));
    }

    // Write the grid-volume .data container and a compose.json that references it.
    std::string modelName = modelObjDirectory.stem().string();
    if (modelName.empty()) modelName = "model";

    std::filesystem::create_directories(outputDirectory);
    projv::utils::writeDataFile(outputDirectory + "/model.data", dataFile);

    nlohmann::json compose;
    compose["version"] = 1;
    compose["name"] = modelName;
    compose["components"] = nlohmann::json::array();
    compose["components"].push_back({
        {"type", "data"},
        {"source", "model.data"},
        {"position", {0.0f, 0.0f, 0.0f}},
        {"mutability", "direct"}
    });
    std::ofstream composeOut(outputDirectory + "/compose.json");
    composeOut << compose.dump(4);
    composeOut.close();

    // Free all loaded texture data
    for (auto& [name, tex] : textures) {
        if (tex.image) {
            stbi_image_free(tex.image);
            tex.image = nullptr;
        }
    }

    info("--------------------------------------------");
    info("  Voxelization Summary");
    info("  Input file:      {}", modelObjDirectory.string());
    info("  Shapes / Mats:   {} / {}", shapes.size(), materials.size());
    info("  Textures:        {} loaded, {} missing", texturesLoaded, texturesFailed);
    info("  Triangles:       {}", triangleCount);
    info("  Bounding box:    ({:.3f}, {:.3f}, {:.3f})  ->  ({:.3f}, {:.3f}, {:.3f})",
        verticesMin.x, verticesMin.y, verticesMin.z,
        verticesMin.x + modelExtents.x, verticesMin.y + modelExtents.y, verticesMin.z + modelExtents.z);
    info("  Extents (W/H/D): {:.3f} x {:.3f} x {:.3f}", modelExtents.x, modelExtents.y, modelExtents.z);
    info("  Resolution:      {} requested -> {} effective", voxelizationResolution,
        numberOfChunksPerAxis * chunkResolution);
    info("  Chunks:          {} ({} per axis, {}^3 each)", totalChunks, numberOfChunksPerAxis, chunkResolution);
    info("  Voxel size:      {:.5f} world units", voxelScale);
    info("  Materials:       {} palette entries", palette.colors.size());
    info("  Total voxels:    {}", totalVoxels);
    info("  Occupied blocks: {}", dataFile.blocks.size());
    info("  Output:          {}/model.data + {}/compose.json", outputDirectory, outputDirectory);
    info("--------------------------------------------");
    if (texturesFailed > 0)
        projv::core::warn("{} texture(s) could not be loaded — affected surfaces used material color fallback.", texturesFailed);
}

int main(int argc, char** argv) {
    CLI::App app{"A voxelizer to convert Wavefront OBJ models to voxel ProjectV models."};
    argv = app.ensure_utf8(argv);

    std::string objModelDirectory = "";
    std::string objFileDirectory = "";
    std::string outputDirectory = "";
    std::string resolutionString = "256";
    bool noFlipV = false;

    app.add_option("-m, --modelDir", objModelDirectory, "Path to root directory of the obj model.");
    app.add_option("-f, --objDir", objFileDirectory, "Path to obj file of the obj model.");
    app.add_option("-o, --outputDir", outputDirectory, "Path to put the generated scene.");
    app.add_option("-r, --resolution", resolutionString, "Resolution to voxelize the scene at.");
    app.add_flag("--no-flip-v", noFlipV,
        "Sample textures without flipping the V coordinate. The default flip is the OBJ/OpenGL "
        "convention; use this for models authored against a top-left texture origin (common in "
        "3ds Max exports), which otherwise sample the mirrored half of their atlas.");
    CLI11_PARSE(app, argc, argv);

    int resolution = std::stoi(resolutionString, 0, 10);
    std::filesystem::path objFileDirectoryAsPath = objFileDirectory;

    voxelizeObjFile(objFileDirectoryAsPath, objModelDirectory, objModelDirectory, resolution, outputDirectory, !noFlipV);

    return 0;
}
