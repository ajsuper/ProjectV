// ProjectV Mesh Voxelizer
// A command-line tool that converts polygon models — OBJ, FBX, glTF/GLB, Collada, STL, PLY, 3DS,
// Blender and everything else Assimp reads — into ProjectV's Compose scene format. It parses mesh
// geometry and material data, samples diffuse textures at each voxel's surface intersection, and
// writes the result as a single grid-volume `.data` container plus the `compose.json` that places
// it.
//
// Format handling lives entirely in include/mesh_import.hpp, which flattens any input into one
// world-space triangle soup; everything below this line is format-blind.
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

#define STB_IMAGE_IMPLEMENTATION
#include "include/stb_image.h"
// stb emits its implementation on every include where this is still defined, and the headers below
// include stb_image.h too (they decode textures of their own). One definition is all this needs.
#undef STB_IMAGE_IMPLEMENTATION

#include "include/CLI.hpp"
#include "include/mesh_import.hpp"
#include "include/minecraft_import.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
using namespace projv::core;

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
    unsigned char *image = nullptr;
    ivec2 size{0, 0};
    uint channels = 0;
};

// Decodes an imported texture into RGBA8 pixels. The importer hands back either a file on disk or
// the raw bytes of an image embedded in the model container (GLB and binary FBX carry theirs that
// way), and both end up as the same thing here.
Texture decodeTexture(const meshimport::ImportedTexture& source) {
    Texture texture;
    int width = 0, height = 0, channels = 0;

    if (!source.rawPixels.empty()) {
        // Already decoded by the importer; copy it into the same ownership model as the rest so
        // there is exactly one place that frees texture memory.
        texture.image = (unsigned char*)malloc(source.rawPixels.size());
        if (texture.image != nullptr) {
            std::copy(source.rawPixels.begin(), source.rawPixels.end(), texture.image);
            texture.size = {source.rawWidth, source.rawHeight};
            texture.channels = 4;
        }
        return texture;
    }

    if (!source.encodedBytes.empty()) {
        texture.image = stbi_load_from_memory(source.encodedBytes.data(), int(source.encodedBytes.size()),
                                              &width, &height, &channels, 4);
    } else if (!source.path.empty()) {
        texture.image = stbi_load(source.path.string().c_str(), &width, &height, &channels, 4);
    }

    if (texture.image != nullptr) {
        texture.size = {width, height};
        texture.channels = 4;
    }
    return texture;
}

// Fetches the color a barycentric position on a triangle resolves to: the diffuse texel it maps
// to, or the material's flat diffuse color when the triangle has no usable texture. Shared by the
// palette pre-pass and the voxel loop so both see exactly the same colors.
//
// `flipV` selects the texture-coordinate convention, which differs per format: OBJ, FBX and Collada
// put v=0 at the *bottom* of the image while stb_image hands back the top row first, so those need
// the flip, whereas glTF specifies a top-left origin and must not be flipped. meshimport picks the
// default from the file extension. It stays overridable because plenty of individual models
// (particularly ones converted out of 3ds Max) disagree with their own format, and sampling those
// with the wrong setting lands on whatever occupies the mirrored half of the atlas.
// `outAlpha`, when given, receives the texel's alpha — 255 for anything untextured, since a flat
// material color is fully opaque. Callers use it to drop cutout texels: alpha-tested foliage stores
// its leaf silhouette in the alpha channel and leaves the rest of the quad transparent, so a
// voxelizer that ignores alpha turns every leaf card into a solid slab. See ALPHA_CUTOFF_DEFAULT.
projv::Color sampleFaceColor(const Texture* texture, bool flipV,
                             const vec2& texCoord0, const vec2& texCoord1, const vec2& texCoord2,
                             float bary0, float bary1, float bary2,
                             const vec3& fallbackColor, uint8_t* outAlpha = nullptr) {
    if (texture == nullptr) {
        if (outAlpha != nullptr) *outAlpha = 255;
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

    // decodeTexture always requests 4 components from stb, so the alpha byte is present even for a
    // source image that had none — stb fills it with 255, which is exactly the opaque answer.
    if (outAlpha != nullptr) *outAlpha = texture->image[index + 3];
    return {texture->image[index + 0], texture->image[index + 1], texture->image[index + 2]};
}

// ---- Displacement ----------------------------------------------------------------------------
//
// Height maps let voxelization recover surface relief the mesh never had: mortar between bricks,
// carving in a moulding, the grain of a rough floor. That detail is exactly what a triangle mesh
// pushes into a texture instead of geometry, and it is exactly what a high-resolution voxel grid has
// the budget to represent — at -r 8192 a voxel is 8.4 mm, so a 3 cm relief is ~3.6 voxels deep.
//
// This is much cheaper in voxels than in meshes. A renderer displacing a mesh has to tessellate
// until triangles are pixel-sized; a voxelizer just asks, per voxel, "is this inside the displaced
// surface?" — no tessellation, no new topology.

// Which way a height map reads. There is no reliable convention in the wild — an asset routinely
// mixes both, because its maps came from different sources — so this is decided per map by default.
enum class HeightPolarity {
    Auto,           // Decide per map from its own histogram. See decideHeightPolarity.
    BrightIsHigh,   // The nominal convention: white protrudes.
    DarkIsHigh,     // Inverted authoring: black protrudes.
};

struct DisplaceOptions {
    float scaleMeters = 0.0f;       // Peak-to-trough relief in model units. 0 disables displacement.
    bool zeroAtMean = false;        // Use each map's own mean as the neutral height instead of 0.5.
    HeightPolarity polarity = HeightPolarity::Auto;
    float thicknessVoxels = 1.0f;   // Shell thickness of the displaced surface, in voxels.
    bool enabled() const { return scaleMeters > 0.0f; }
};

// Statistics of a height map, enough to judge which way it reads.
struct HeightStats {
    float mean = 0.5f;
    float median = 0.5f;
};

inline HeightStats measureHeight(const Texture& texture) {
    HeightStats stats;
    if (texture.image == nullptr || texture.size.x <= 0 || texture.size.y <= 0) return stats;
    const int texelCount = texture.size.x * texture.size.y;
    const int stride = std::max(1, texelCount / 8192);
    uint64_t histogram[256] = {0};
    double sum = 0.0;
    size_t sampled = 0;
    for (int texel = 0; texel < texelCount; texel += stride) {
        unsigned char value = texture.image[size_t(texel) * texture.channels];
        histogram[value]++;
        sum += value;
        sampled++;
    }
    if (sampled == 0) return stats;
    stats.mean = float(sum / double(sampled)) / 255.0f;
    uint64_t half = sampled / 2, running = 0;
    for (int bin = 0; bin < 256; bin++) {
        running += histogram[bin];
        if (running >= half) { stats.median = bin / 255.0f; break; }
    }
    return stats;
}

// Decides whether a height map is authored dark-is-high, i.e. needs its sign flipped.
//
// The physical assumption is what makes this decidable: relief on an architectural surface is
// overwhelmingly *recesses cut into an otherwise flat field* — mortar between bricks, carving in a
// moulding, pits in stone. So the field should be the majority of the image and the detail the
// minority, and the detail should be the darker of the two.
//
// A minority of thin features drags the mean away from the median in the features' own direction, so
// mean > median means the minority is bright — a map whose "detail" protrudes, which is the inverted
// authoring. Separately, a map whose field is itself dark (low mean) is inverted regardless of the
// spread: a correctly-authored map's flat field sits high, because there is nothing to be lower than.
//
// San Miguel needs this per-map rather than globally: its brickwork map is authored conventionally
// while the stone arch, column and pitted-stone maps are inverted, so any single global choice gets
// one of the two groups wrong.
inline bool decideHeightPolarity(const HeightStats& stats, HeightPolarity requested) {
    if (requested == HeightPolarity::BrightIsHigh) return false;
    if (requested == HeightPolarity::DarkIsHigh) return true;
    const bool brightMinority = (stats.mean - stats.median) > 0.015f;
    const bool darkField = stats.mean < 0.35f;
    return brightMinority || darkField;
}

// Samples a height map as 0..1. Grayscale images decode to R=G=B, so the red channel is the height;
// using it directly rather than a luminance weighting keeps an accidentally-tinted map behaving.
float sampleHeight(const Texture& texture, bool flipV,
                   const vec2& texCoord0, const vec2& texCoord1, const vec2& texCoord2,
                   float bary0, float bary1, float bary2) {
    vec2 uv = bary0 * texCoord0 + bary1 * texCoord1 + bary2 * texCoord2;
    if (flipV) uv.y = 1.0f - uv.y;
    uv.x -= std::floor(uv.x);
    uv.y -= std::floor(uv.y);
    int tx = clamp(int(uv.x * texture.size.x), 0, texture.size.x - 1);
    int ty = clamp(int(uv.y * texture.size.y), 0, texture.size.y - 1);
    return texture.image[(ty * texture.size.x + tx) * int(texture.channels)] / 255.0f;
}

// Is this image a tangent-space normal map rather than a height map? Those encode a direction as RGB
// and average to roughly (128, 128, 255) — flat normals pointing straight out of the surface. A
// height map is grayscale, so R, G and B track each other. Displacement needs a scalar height, and a
// normal map is its gradient, so using one as a height field produces noise rather than relief.
//
// Checked from the pixels rather than the filename because the reference in the .mtl says "bump"
// either way and is not evidence of the contents.
bool looksLikeNormalMap(const Texture& texture) {
    if (texture.image == nullptr || texture.size.x <= 0 || texture.size.y <= 0) return false;
    const int stride = std::max(1, (texture.size.x * texture.size.y) / 4096); // ~4k samples is plenty.
    double sumR = 0.0, sumG = 0.0, sumB = 0.0;
    size_t sampled = 0;
    for (int texel = 0; texel < texture.size.x * texture.size.y; texel += stride) {
        const unsigned char* pixel = texture.image + size_t(texel) * texture.channels;
        sumR += pixel[0]; sumG += pixel[1]; sumB += pixel[2];
        sampled++;
    }
    if (sampled == 0) return false;
    double meanR = sumR / double(sampled), meanG = sumG / double(sampled), meanB = sumB / double(sampled);
    return meanB > 170.0 && meanB > meanR + 40.0 && meanB > meanG + 40.0 &&
           std::abs(meanR - 128.0) < 45.0 && std::abs(meanG - 128.0) < 45.0;
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

// Texels at or below this alpha produce no voxel. Alpha-tested foliage is the reason: a leaf card is
// one quad whose alpha channel carries the leaf silhouette and whose remainder is fully transparent,
// so sampling color alone fills the whole quad and turns a tree into a set of solid slabs. 128 is
// the conventional half-way cutoff that authoring tools assume.
//
// The palette pre-pass has to apply the same test. A fully transparent texel still holds *some* RGB
// underneath — usually black or white filler — and histogramming that spends scarce palette entries
// on colors no visible voxel will ever use.
constexpr uint8_t ALPHA_CUTOFF_DEFAULT = 128;

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

// Writes a finished grid-volume container plus the compose.json that places it — the folder
// loadComposeFromDisk opens. Shared by every front end, because the output format does not care
// whether the voxels came from triangles or from a Minecraft save.
void writeComposeScene(projv::DataFile& dataFile, const std::string& outputDirectory,
                       const std::string& sceneName, const ColorPalette& palette) {
    std::filesystem::create_directories(outputDirectory);
    projv::utils::writeDataFile(outputDirectory + "/model.data", dataFile);

    // The palette goes in compose.json, not in the .data: the container stores the material *byte*
    // per voxel and this names what those bytes mean. Keeping them apart is what lets one .data be
    // instanced by several components that colour it differently -- and it puts the colours somewhere
    // a person can edit by hand.
    nlohmann::json paletteJson = nlohmann::json::array();
    for (const projv::Color& color : palette.colors) {
        uint32_t packed = projv::packColor(color);
        paletteJson.push_back({
            {"color", {(packed >> 20) & 0x3FFu, (packed >> 10) & 0x3FFu, packed & 0x3FFu}}
        });
    }

    nlohmann::json compose;
    compose["version"] = 1;
    compose["name"] = sceneName;
    compose["components"] = nlohmann::json::array();
    compose["components"].push_back({
        {"type", "data"},
        {"source", "model.data"},
        {"position", {0.0f, 0.0f, 0.0f}},
        {"mutability", "direct"},
        {"palette", paletteJson}
    });
    std::ofstream composeOut(outputDirectory + "/compose.json");
    composeOut << compose.dump(4);
    composeOut.close();
}

void voxelizeModel(std::filesystem::path modelPath, std::filesystem::path assetDirectory, int voxelizationResolution, std::string outputDirectory, bool flipTextureV, uint8_t alphaCutoff, unsigned int threadCount, const DisplaceOptions& displace) {
    using namespace projv::core;
    info("--------------------------------------------");
    info("  ProjectV Voxelizer");
    info("  Input:      {}", modelPath.string());
    info("  Assets:     {}", assetDirectory.string());
    info("  Output:     {}", outputDirectory);
    info("  Resolution: {} (rounded up to whole chunks; chunks are 256^3, or 64^3 at r<=64)", voxelizationResolution);
    info("  Flip V:     {}", flipTextureV ? "yes (bottom-left texture origin)" : "no (top-left texture origin)");
    if (alphaCutoff == 0) {
        info("  Alpha:      ignored (every texel is treated as solid)");
    } else {
        info("  Alpha:      cutoff {} — texels below this produce no voxel", alphaCutoff);
    }
    info("  Threads:    {}", threadCount);
    if (displace.enabled()) {
        const char* polarityName = displace.polarity == HeightPolarity::Auto ? "auto (per map)"
                                 : displace.polarity == HeightPolarity::DarkIsHigh ? "dark-is-high"
                                                                                   : "bright-is-high";
        info("  Displace:   {:.4f} model units peak-to-trough, zero at {}, polarity {}",
            displace.scaleMeters, displace.zeroAtMean ? "each map's mean" : "mid-gray", polarityName);
    } else {
        info("  Displace:   off (--displace-scale enables it)");
    }
    info("--------------------------------------------");

    info("Loading model...");
    meshimport::ImportedModel model;
    int texturesMissing = 0;
    if (!meshimport::importModel(modelPath, assetDirectory, model, texturesMissing)) {
        return; // importModel has already logged why.
    }
    info("  {} ({} mesh(es), {} material(s)).", model.formatName, model.meshCount, model.materials.size());

    // Decode every texture the model references, once, into a slot parallel to model.textures.
    std::vector<Texture> textures(model.textures.size());
    int texturesLoaded = 0, texturesFailed = 0;
    if (!model.textures.empty()) info("Loading textures...");
    for (size_t textureIndex = 0; textureIndex < model.textures.size(); textureIndex++) {
        const meshimport::ImportedTexture& source = model.textures[textureIndex];
        textures[textureIndex] = decodeTexture(source);

        bool embedded = source.path.empty();
        if (textures[textureIndex].image == nullptr) {
            projv::core::warn("  [FAILED] '{}' - {}", source.name,
                embedded ? "embedded image could not be decoded" : stbi_failure_reason());
            texturesFailed++;
        } else {
            projv::core::info("  [OK] '{}' ({}x{}){}", source.name, textures[textureIndex].size.x,
                textures[textureIndex].size.y, embedded ? " [embedded]" : "");
            texturesLoaded++;
        }
    }
    texturesFailed += texturesMissing;

    // The importer already produced a world-space triangle soup; all that is left is to measure it.
    info("Parsing geometry...");
    std::vector<meshimport::ImportedVertex>& vertices = model.vertices; // Normalized into grid space below.
    size_t triangleCount = model.triangleMaterials.size();

    vec3 verticesMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    vec3 verticesMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const meshimport::ImportedVertex& vertex : vertices) {
        verticesMin = projv::core::min(verticesMin, vertex.position);
        verticesMax = projv::core::max(verticesMax, vertex.position);
    }
    info("  {} triangle(s) parsed.", triangleCount);

    // Resolves a triangle's texture, or nullptr when its material is untextured or the image could
    // not be loaded — in which case sampleFaceColor falls back to the material's flat color.
    auto textureForTriangle = [&](size_t triangleIndex) -> const Texture* {
        int textureIndex = model.materials[model.triangleMaterials[triangleIndex]].textureIndex;
        if (textureIndex < 0 || textures[textureIndex].image == nullptr) return nullptr;
        return &textures[textureIndex];
    };

    auto fallbackColorForTriangle = [&](size_t triangleIndex) -> const vec3& {
        return model.materials[model.triangleMaterials[triangleIndex]].diffuseColor;
    };

    // Validate the height maps the importer found, and work out each one's neutral height. A "bump"
    // reference that turned out to name a normal map is dropped here rather than used as a height
    // field, which would turn its gradient encoding into noise. Nothing below runs when displacement
    // is off, so a run without --displace-scale is unaffected by any of this.
    std::vector<float> heightZeroPoint(textures.size(), 0.5f);
    std::vector<bool> heightUsable(textures.size(), false);
    std::vector<bool> heightInvert(textures.size(), false);
    int heightMapsUsed = 0, heightMapsRejected = 0, heightMapsFlipped = 0, heightMapsWereColor = 0;
    if (displace.enabled()) {
        info("Validating height maps...");
        // Every texture some material uses as its *diffuse* map. A sibling lookup that lands on one
        // of these has found a colour texture, not a height map — see below.
        std::vector<bool> usedAsDiffuse(textures.size(), false);
        std::vector<bool> referenced(textures.size(), false);
        for (const meshimport::ImportedMaterial& material : model.materials) {
            if (material.textureIndex >= 0) usedAsDiffuse[material.textureIndex] = true;
            if (material.heightTextureIndex >= 0) referenced[material.heightTextureIndex] = true;
        }
        for (size_t textureIndex = 0; textureIndex < textures.size(); textureIndex++) {
            if (!referenced[textureIndex] || textures[textureIndex].image == nullptr) continue;
            const Texture& texture = textures[textureIndex];

            // Stripping `N_` can land on the material's own colour texture rather than a height map:
            // San Miguel's `N_muros_b1.png` sits beside `muros_b1.png`, which is the wall albedo. A
            // desaturated albedo passes every "is this grayscale" test there is, so the reliable
            // signal is that the file is *also* somebody's diffuse map. Displacing by albedo would
            // turn every painted marking into geometry.
            if (usedAsDiffuse[textureIndex]) {
                projv::core::warn("  [IS A COLOUR MAP] '{}' — this file is used as a diffuse texture, "
                                  "so it is not a height map. No displacement for it.",
                    model.textures[textureIndex].name);
                heightMapsWereColor++;
                heightMapsRejected++;
                continue;
            }
            if (looksLikeNormalMap(texture)) {
                projv::core::warn("  [NORMAL MAP] '{}' — no grayscale height sibling found, so this "
                                  "material gets no displacement.", model.textures[textureIndex].name);
                heightMapsRejected++;
                continue;
            }

            HeightStats stats = measureHeight(texture);
            heightZeroPoint[textureIndex] = displace.zeroAtMean ? stats.mean : 0.5f;
            heightInvert[textureIndex] = decideHeightPolarity(stats, displace.polarity);
            heightUsable[textureIndex] = true;
            heightMapsUsed++;
            if (heightInvert[textureIndex]) {
                heightMapsFlipped++;
                info("  '{}' mean={:.2f} median={:.2f} — reads dark-is-high, flipping so its detail "
                     "recesses.", model.textures[textureIndex].name, stats.mean, stats.median);
            }
            // A map averaging far from mid-gray spends most of its range on one side, so treating
            // 0.5 as neutral shifts the whole surface instead of adding relief around it.
            if (!displace.zeroAtMean && std::abs(stats.mean - 0.5f) > 0.25f) {
                projv::core::warn("  '{}' averages {:.2f}, far from mid-gray — displacement will be "
                                  "mostly a uniform {} shift. --displace-zero mean centres it.",
                    model.textures[textureIndex].name, stats.mean, stats.mean > 0.5f ? "outward" : "inward");
            }
        }
        info("  {} height map(s) usable ({} flipped to dark-is-high), {} rejected "
             "({} were colour maps, {} normal maps).",
            heightMapsUsed, heightMapsFlipped, heightMapsRejected, heightMapsWereColor,
            heightMapsRejected - heightMapsWereColor);
    }

    // The height map index for a triangle, or -1 when its material has none or it was rejected.
    // An index rather than a pointer because the caller needs the map's zero point as well.
    auto heightMapForTriangle = [&](size_t triangleIndex) -> int {
        if (!displace.enabled()) return -1;
        int heightIndex = model.materials[model.triangleMaterials[triangleIndex]].heightTextureIndex;
        if (heightIndex < 0 || !heightUsable[heightIndex]) return -1;
        return heightIndex;
    };

    // Build the material palette before voxelizing — every voxel written below needs a palette ID,
    // and the palette has to fit inside the engine's per-component cap. Triangles are sampled in
    // proportion to their surface area, which is what voxel counts follow.
    info("Building material palette...");
    ColorHistogram histogram;
    size_t paletteSamplesCut = 0;
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

    for (size_t triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++) {
        const meshimport::ImportedVertex& v0 = vertices[triangleIndex * 3 + 0];
        const meshimport::ImportedVertex& v1 = vertices[triangleIndex * 3 + 1];
        const meshimport::ImportedVertex& v2 = vertices[triangleIndex * 3 + 2];
        const Texture* texture = textureForTriangle(triangleIndex);

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
            uint8_t alpha = 255;
            projv::Color color = sampleFaceColor(texture, flipTextureV,
                                                 v0.texCoord, v1.texCoord, v2.texCoord,
                                                 1.0f - u - v, u, v,
                                                 fallbackColorForTriangle(triangleIndex), &alpha);
            // Same cutoff the voxel loop uses. A cutout texel produces no voxel, so letting its
            // filler color vote here would spend palette entries on colors nothing can display.
            if (alphaCutoff != 0 && alpha < alphaCutoff) { paletteSamplesCut++; continue; }
            histogram.add(color.r, color.g, color.b);
        }
    }
    ColorPalette palette = buildColorPalette(histogram.populatedCells());
    info("  {} material(s) in palette (cap {}).", palette.colors.size(), PALETTE_CAPACITY);
    if (paletteSamplesCut > 0) {
        info("  {} sample(s) dropped below the alpha cutoff (cutout texels).", paletteSamplesCut);
    }

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

    // Displacement is specified in model units but applied in grid space, where 1 unit is 1 voxel, so
    // it converts through the same factor the positions did. This is what makes the flag
    // resolution-independent: 3 cm of relief is 3 cm whether -r is 4096 or 8192, it just resolves
    // into more voxels at the higher setting.
    const float displaceHalfRangeVoxels = displace.enabled()
        ? 0.5f * displace.scaleMeters * modelScaleFactor : 0.0f;
    // How far a displaced surface can stray from its triangle, and therefore how much every bounding
    // box below has to grow. Getting this wrong does not produce a wrong answer in the interior — it
    // produces missing geometry at chunk seams, which is far harder to notice.
    const float displaceReachVoxels = displace.enabled()
        ? displaceHalfRangeVoxels + displace.thicknessVoxels : 0.0f;
    if (displace.enabled()) {
        info("Displacement: +/-{:.2f} voxel(s) about the surface ({:.4f} model units peak-to-trough).",
            displaceHalfRangeVoxels, displace.scaleMeters);
        if (displaceHalfRangeVoxels < 0.5f) {
            projv::core::warn("  That is under half a voxel, so displacement will barely register. "
                              "Raise --displace-scale or -r.");
        }
    }

    uint totalChunks = numberOfChunksPerAxis * numberOfChunksPerAxis * numberOfChunksPerAxis;

    info("Voxelizing: {} chunk(s) ({} per axis), voxel size: {:.5f}", totalChunks, numberOfChunksPerAxis, voxelScale);

    size_t totalVoxels = 0;

    // Accumulate every occupied chunk into a single grid-volume .data container.
    projv::DataFile dataFile;
    dataFile.resolution = chunkResolution;
    dataFile.voxelScale = voxelScale;

    // ---- Triangle binning -----------------------------------------------------------------------
    //
    // Without this, the chunk loop rescans the entire triangle soup once per chunk: O(chunks x
    // triangles), which at -r 8192 is 32768 chunks x 10M triangles — 3.3e11 bounding-box tests to
    // place geometry that, at a few centimetres per triangle against 2.16 m chunks, almost always
    // belongs to exactly one chunk. Cost then tracks the grid rather than the model, which is the
    // wrong thing for it to track.
    //
    // Binning inverts the loop: assign each triangle once to the chunks its bbox overlaps, then let
    // each chunk visit only its own bucket. Empty chunks cost nothing instead of a full scan.
    //
    // The overlap test here is deliberately the same conservative bbox test the serial loop applied,
    // so the set of (chunk, triangle) pairs considered is identical and binning cannot change the
    // output — a triangle exactly on a chunk boundary still lands in both neighbours.
    //
    // Stored as CSR (offsets plus one flat index array) rather than a vector per chunk: at 32768
    // chunks, per-vector allocation overhead and pointer chasing would cost more than the payload.
    info("Binning triangles into chunks...");
    const int chunksPerAxis = int(numberOfChunksPerAxis);
    auto chunkCellOf = [&](const vec3& point) {
        ivec3 cell = ivec3(floor(point / chunkScale));
        return clamp(cell, ivec3(0), ivec3(chunksPerAxis - 1));
    };
    // Linear (x + y*N + z*N^2) cell indexing; the chunk loop converts from its Z-order chunk index.
    auto linearCell = [&](int x, int y, int z) {
        return size_t(x) + size_t(y) * size_t(chunksPerAxis)
             + size_t(z) * size_t(chunksPerAxis) * size_t(chunksPerAxis);
    };

    // 64-bit offsets: a large floor triangle can span hundreds of chunks at high -r, so the
    // incidence total is not safely bounded by a 32-bit count even though each index fits in one.
    std::vector<uint64_t> binOffsets(size_t(totalChunks) + 1, 0);
    for (size_t triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++) {
        const vec3& p0 = vertices[triangleIndex * 3 + 0].position;
        const vec3& p1 = vertices[triangleIndex * 3 + 1].position;
        const vec3& p2 = vertices[triangleIndex * 3 + 2].position;
        // Grown by the displacement reach: a displaced surface can leave its triangle's own box and
        // cross into a neighbouring chunk, and a triangle absent from that chunk's bucket would leave
        // a seam there.
        ivec3 lo = chunkCellOf(min(min(p0, p1), p2) - vec3(displaceReachVoxels));
        ivec3 hi = chunkCellOf(max(max(p0, p1), p2) + vec3(displaceReachVoxels));
        for (int z = lo.z; z <= hi.z; z++)
            for (int y = lo.y; y <= hi.y; y++)
                for (int x = lo.x; x <= hi.x; x++)
                    binOffsets[linearCell(x, y, z) + 1]++;
    }
    std::partial_sum(binOffsets.begin(), binOffsets.end(), binOffsets.begin());
    const uint64_t totalIncidences = binOffsets.back();

    std::vector<uint32_t> binTriangles(totalIncidences);
    {
        // `cursor` walks a copy of the bucket starts so binOffsets stays the final index.
        std::vector<uint64_t> cursor(binOffsets.begin(), binOffsets.end() - 1);
        for (size_t triangleIndex = 0; triangleIndex < triangleCount; triangleIndex++) {
            const vec3& p0 = vertices[triangleIndex * 3 + 0].position;
            const vec3& p1 = vertices[triangleIndex * 3 + 1].position;
            const vec3& p2 = vertices[triangleIndex * 3 + 2].position;
            // Must match the count pass above exactly, displacement reach included.
            ivec3 lo = chunkCellOf(min(min(p0, p1), p2) - vec3(displaceReachVoxels));
            ivec3 hi = chunkCellOf(max(max(p0, p1), p2) + vec3(displaceReachVoxels));
            for (int z = lo.z; z <= hi.z; z++)
                for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++)
                        binTriangles[cursor[linearCell(x, y, z)]++] = uint32_t(triangleIndex);
        }
    }
    info("  {} triangle-chunk incidence(s), {:.2f} per triangle ({:.1f} MB).", totalIncidences,
        triangleCount > 0 ? double(totalIncidences) / double(triangleCount) : 0.0,
        double(totalIncidences * sizeof(uint32_t)) / (1024.0 * 1024.0));

    // Only chunks that actually received triangles can produce a voxel, so the rest are skipped
    // outright — at -r 8192 that is ~1.3k chunks of 32768.
    std::vector<int> chunksToProcess;
    for (int chunkIndex = 0; chunkIndex < (int)totalChunks; chunkIndex++) {
        projv::core::ivec3 gridPosition = projv::utils::reverseZOrderIndex(chunkIndex);
        size_t cell = linearCell(gridPosition.x, gridPosition.y, gridPosition.z);
        if (binOffsets[cell + 1] > binOffsets[cell]) chunksToProcess.push_back(chunkIndex);
    }
    info("  {} of {} chunk(s) hold geometry.", chunksToProcess.size(), totalChunks);

    // Each chunk is independent: its own brick map, its own DataBlock, and everything it reads —
    // vertices, textures, the palette LUT — is immutable by this point. So the only shared state is
    // the work counter and the output collection. Blocks land in per-thread vectors tagged with
    // their chunk index and are merged in that order afterwards, which keeps the .data
    // byte-identical no matter what --threads is set to.
    struct ThreadResult {
        std::vector<std::pair<int, projv::DataBlock>> blocks;
        size_t voxels = 0;
        size_t alphaSkipped = 0;
    };
    std::vector<ThreadResult> threadResults(threadCount);
    std::atomic<size_t> nextWorkItem{0};
    std::atomic<size_t> chunksDone{0};
    const size_t logEvery = std::max<size_t>(1, chunksToProcess.size() / 10);

    auto voxelizeChunkRange = [&](unsigned int threadIndex) {
      ThreadResult& threadOutput = threadResults[threadIndex];
      for (;;) {
        size_t workItem = nextWorkItem.fetch_add(1);
        if (workItem >= chunksToProcess.size()) break;
        int chunkIndex = chunksToProcess[workItem];

        projv::core::ivec3 chunkIndexPosition = projv::utils::reverseZOrderIndex(chunkIndex);
        projv::ChunkHeader chunkHeader;
        chunkHeader.chunkID = chunkIndex;
        chunkHeader.position = vec3(chunkIndexPosition) * vec3(chunkScale);
        chunkHeader.voxelScale = voxelScale;
        chunkHeader.resolution = chunkResolution;
        chunkHeader.scale = projv::utils::createChunkScaleFromVoxelScaleAndResolution(voxelScale, int(chunkResolution));
        auto brickMap = projv::utils::createVoxelBrickMap(
            projv::utils::computeBrickDims(chunkResolution));

        const size_t chunkCell = linearCell(chunkIndexPosition.x, chunkIndexPosition.y, chunkIndexPosition.z);
        for (uint64_t bucketSlot = binOffsets[chunkCell]; bucketSlot < binOffsets[chunkCell + 1]; bucketSlot++) {
            const size_t triangleIndex = binTriangles[bucketSlot];
            const vec3& p0 = vertices[triangleIndex * 3].position;
            const vec3& p1 = vertices[triangleIndex * 3 + 1].position;
            const vec3& p2 = vertices[triangleIndex * 3 + 2].position;
            Triangle tri{p0, p1, p2};

            vec2 texCoordsV0 = vertices[triangleIndex * 3].texCoord;
            vec2 texCoordsV1 = vertices[triangleIndex * 3 + 1].texCoord;
            vec2 texCoordsV2 = vertices[triangleIndex * 3 + 2].texCoord;
            const vec3& fallbackColor = fallbackColorForTriangle(triangleIndex);

            const Texture* texture = textureForTriangle(triangleIndex);

            // Precompute barycentric denominator for this triangle
            vec3 edge0 = p1 - p0;
            vec3 edge1 = p2 - p0;
            float d00 = dot(edge0, edge0);
            float d01 = dot(edge0, edge1);
            float d11 = dot(edge1, edge1);
            float baryDenom = d00 * d11 - d01 * d01;
            bool degenerate = (std::abs(baryDenom) < 1e-10f);

            vec3 triMin = min(min(tri.v0, tri.v1), tri.v2);
            vec3 triMax = max(max(tri.v0, tri.v1), tri.v2);

            // No chunk-overlap rejection here: membership in this bucket already established it.

            // Displaced triangles occupy a band around their own plane, so their candidate voxel
            // range has to grow to match. Untextured-by-height triangles keep the exact original
            // range, which is what makes a no-displacement run bit-identical to before.
            const int heightMapIndex = heightMapForTriangle(triangleIndex);
            vec3 faceNormal{0.0f, 0.0f, 0.0f};
            float heightZero = 0.5f;
            bool heightIsInverted = false;
            if (heightMapIndex >= 0) {
                vec3 rawNormal = cross(edge0, edge1);
                float normalLength = length(rawNormal);
                if (normalLength > 1e-12f) {
                    // A geometric face normal, not an interpolated vertex normal. At these voxel
                    // sizes the difference is under a voxel — San Miguel's triangles are ~4 cm
                    // against 8.4 mm voxels — and it saves carrying normals through the importer.
                    faceNormal = rawNormal / normalLength;
                    triMin = triMin - vec3(displaceReachVoxels);
                    triMax = triMax + vec3(displaceReachVoxels);
                    heightZero = heightZeroPoint[heightMapIndex];
                    heightIsInverted = heightInvert[heightMapIndex];
                }
            }
            const bool displaceThisTriangle = (heightMapIndex >= 0) && length(faceNormal) > 0.0f;

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
                        vec3 voxelCenter = (voxelAABB.min + voxelAABB.max) * 0.5f;

                        // Undisplaced: the voxel is solid exactly when the triangle passes through
                        // it. Displaced: the triangle itself is no longer the surface, so the SAT
                        // test is replaced by the band test further down.
                        if (!displaceThisTriangle && !triAABBIntersect(tri, voxelAABB)) continue;

                        // Interpolate UV at the voxel center (not the triangle centroid).
                        float bary0 = 1.0f, bary1 = 0.0f, bary2 = 0.0f;
                        if (!degenerate) {
                            vec3 v0p = voxelCenter - p0;
                            float d20 = dot(v0p, edge0);
                            float d21 = dot(v0p, edge1);

                            bary1 = (d11 * d20 - d01 * d21) / baryDenom;
                            bary2 = (d00 * d21 - d01 * d20) / baryDenom;
                            bary0 = 1.0f - bary1 - bary2;

                            // This formula already resolves the voxel centre against the triangle's
                            // plane, so for displacement it is the barycentric of the projected
                            // point — which is exactly what the height field has to be sampled at.
                            // Outside the triangle it must be rejected rather than clamped, or the
                            // displaced band would spill past the triangle's edges and thicken every
                            // seam in the mesh.
                            if (displaceThisTriangle) {
                                const float edgeTolerance = -1e-4f;
                                if (bary0 < edgeTolerance || bary1 < edgeTolerance || bary2 < edgeTolerance) {
                                    continue;
                                }
                            }

                            // Clamp to valid barycentric range and renormalize
                            bary0 = std::max(0.0f, bary0);
                            bary1 = std::max(0.0f, bary1);
                            bary2 = std::max(0.0f, bary2);
                            float barySum = bary0 + bary1 + bary2;
                            if (barySum > 1e-6f) {
                                bary0 /= barySum; bary1 /= barySum; bary2 /= barySum;
                            }
                        }

                        // The displaced surface test. Height is sampled at the voxel's projected
                        // position and turned into a signed offset along the face normal; the voxel
                        // is solid when it lies within the shell around that offset surface.
                        //
                        // A shell rather than a fill from the original surface outward, because a
                        // recessed height (mortar, carving) has to *remove* material — filling the
                        // gap between plane and displaced surface would raise grooves into ridges.
                        // The 0.87 floor is half a voxel's diagonal, the smallest thickness that
                        // cannot leave pinholes on a surface crossing the grid at an angle.
                        if (displaceThisTriangle) {
                            float height = sampleHeight(textures[heightMapIndex], flipTextureV,
                                texCoordsV0, texCoordsV1, texCoordsV2, bary0, bary1, bary2);
                            float offset = (height - heightZero) * 2.0f * displaceHalfRangeVoxels;
                            if (heightIsInverted) offset = -offset;
                            float distanceToSurface = dot(voxelCenter - p0, faceNormal) - offset;
                            float halfThickness = std::max(0.87f, displace.thicknessVoxels * 0.5f);
                            if (std::abs(distanceToSurface) > halfThickness) continue;
                        }

                        uint8_t alpha = 255;
                        projv::Color color = sampleFaceColor(degenerate ? nullptr : texture,
                            flipTextureV, texCoordsV0, texCoordsV1, texCoordsV2,
                            bary0, bary1, bary2, fallbackColor, &alpha);

                        // A cutout texel means the triangle is transparent at this point, so there
                        // is no surface here to occupy the voxel. This one test is what stops
                        // alpha-tested foliage from voxelizing as solid slabs: a leaf card is mostly
                        // transparent, and without it the whole quad fills in.
                        if (alphaCutoff != 0 && alpha < alphaCutoff) {
                            threadOutput.alphaSkipped++;
                            continue;
                        }

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
        threadOutput.voxels += chunkVoxels;

        size_t done = chunksDone.fetch_add(1) + 1;
        if (done % logEvery == 0 || done == chunksToProcess.size()) {
            info("  Chunk {}/{} ({:.0f}%)", done, chunksToProcess.size(),
                100.0 * double(done) / double(chunksToProcess.size()));
        }

        // A chunk can still come out empty even though triangles were binned into it: the bbox
        // overlap that put them here is conservative, and the alpha cutoff can reject every texel.
        if (chunkVoxels == 0) {
            continue;
        }

        projv::Chunk chunk = projv::utils::createChunk(chunkHeader);
        projv::utils::updateChunkFromBrickMap(chunk, *brickMap);

        projv::DataBlock block;
        block.gridX = chunkIndexPosition.x;
        block.gridY = chunkIndexPosition.y;
        block.gridZ = chunkIndexPosition.z;
        // Baked here rather than at load: bakeMaterialsFromBrickMap stamps each leaf node with its
        // offset into materialIDs, so the pair written to disk is exactly the pair the GPU reads.
        block.geometry = std::move(chunk.geometryData);
        projv::utils::bakeMaterialsFromBrickMap(block.geometry, block.materialIDs, *brickMap);
        threadOutput.blocks.emplace_back(chunkIndex, std::move(block));
      }
    };

    if (threadCount <= 1) {
        voxelizeChunkRange(0);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threadCount);
        for (unsigned int threadIndex = 0; threadIndex < threadCount; threadIndex++) {
            pool.emplace_back(voxelizeChunkRange, threadIndex);
        }
        for (std::thread& thread : pool) thread.join();
    }

    // Merge the per-thread output. Sorting by chunk index restores the order the serial loop
    // produced, so --threads changes only the wall clock and never the bytes on disk.
    size_t totalAlphaSkipped = 0;
    std::vector<std::pair<int, projv::DataBlock>> orderedBlocks;
    for (ThreadResult& threadOutput : threadResults) {
        totalVoxels += threadOutput.voxels;
        totalAlphaSkipped += threadOutput.alphaSkipped;
        for (std::pair<int, projv::DataBlock>& entry : threadOutput.blocks) {
            orderedBlocks.emplace_back(entry.first, std::move(entry.second));
        }
    }
    std::sort(orderedBlocks.begin(), orderedBlocks.end(),
        [](const std::pair<int, projv::DataBlock>& a, const std::pair<int, projv::DataBlock>& b) {
            return a.first < b.first;
        });
    for (std::pair<int, projv::DataBlock>& entry : orderedBlocks) {
        dataFile.blocks.push_back(std::move(entry.second));
    }
    if (totalAlphaSkipped > 0) {
        info("  {} voxel placement(s) rejected by the alpha cutoff.", totalAlphaSkipped);
    }

    // Write the grid-volume .data container and a compose.json that references it.
    std::string modelName = modelPath.stem().string();
    if (modelName.empty()) modelName = "model";
    writeComposeScene(dataFile, outputDirectory, modelName, palette);

    // Free all loaded texture data
    for (Texture& texture : textures) {
        if (texture.image) {
            stbi_image_free(texture.image);
            texture.image = nullptr;
        }
    }

    info("--------------------------------------------");
    info("  Voxelization Summary");
    info("  Input file:      {}", modelPath.string());
    info("  Format:          {}", model.formatName);
    info("  Meshes / Mats:   {} / {}", model.meshCount, model.materials.size());
    info("  Textures:        {} loaded ({} embedded), {} missing", texturesLoaded,
        model.embeddedTextureCount, texturesFailed);
    info("  Triangles:       {}", triangleCount);
    info("  Bounding box:    ({:.3f}, {:.3f}, {:.3f})  ->  ({:.3f}, {:.3f}, {:.3f})",
        verticesMin.x, verticesMin.y, verticesMin.z,
        verticesMin.x + modelExtents.x, verticesMin.y + modelExtents.y, verticesMin.z + modelExtents.z);
    info("  Extents (W/H/D): {:.3f} x {:.3f} x {:.3f}", modelExtents.x, modelExtents.y, modelExtents.z);
    info("  Resolution:      {} requested -> {} effective", voxelizationResolution,
        numberOfChunksPerAxis * chunkResolution);
    info("  Chunks:          {} ({} per axis, {}^3 each), {} held geometry", totalChunks,
        numberOfChunksPerAxis, chunkResolution, chunksToProcess.size());
    info("  Binning:         {} incidence(s), {:.2f} per triangle", totalIncidences,
        triangleCount > 0 ? double(totalIncidences) / double(triangleCount) : 0.0);
    info("  Threads:         {}", threadCount);
    info("  Voxel size:      {:.5f} world units", voxelScale);
    info("  Materials:       {} palette entries", palette.colors.size());
    if (alphaCutoff != 0) {
        info("  Alpha cutoff:    {} — {} voxel placement(s) rejected", alphaCutoff, totalAlphaSkipped);
    }
    if (displace.enabled()) {
        info("  Displacement:    +/-{:.2f} voxel(s), {} map(s) used ({} flipped), {} rejected "
             "({} colour, {} normal)",
            displaceHalfRangeVoxels, heightMapsUsed, heightMapsFlipped, heightMapsRejected,
            heightMapsWereColor, heightMapsRejected - heightMapsWereColor);
    }
    info("  Total voxels:    {}", totalVoxels);
    info("  Occupied blocks: {}", dataFile.blocks.size());
    info("  Output:          {}/model.data + {}/compose.json", outputDirectory, outputDirectory);
    info("--------------------------------------------");
    if (texturesFailed > 0)
        projv::core::warn("{} texture(s) could not be loaded — affected surfaces used material color fallback.", texturesFailed);
}

// ---- Minecraft worlds ------------------------------------------------------------------------
//
// A Minecraft save is already a voxel grid, so this path skips triangle rasterization entirely:
// blocks map one-to-one onto voxels and go straight into the brick maps. Everything downstream —
// the palette, the tree64 build, the .data container — is shared with the mesh path.
//
// Because a block *is* a voxel, `-r` has nothing to scale: the grid size follows from how much of
// the world was selected. Use --mc-bounds to control the size instead.
void voxelizeMinecraftWorld(std::filesystem::path worldPath, const minecraft::ImportOptions& options,
                            std::string outputDirectory) {
    using namespace projv::core;
    info("--------------------------------------------");
    info("  ProjectV Voxelizer — Minecraft world");
    info("  Input:      {}", worldPath.string());
    info("  Dimension:  {}", options.dimension);
    info("  Output:     {}", outputDirectory);
    if (!options.resourcePack.empty()) info("  Pack:       {}", options.resourcePack.string());
    info("--------------------------------------------");

    info("Reading world...");
    minecraft::ImportedWorld world;
    if (!minecraft::importWorld(worldPath, options, world)) {
        return; // importWorld has already logged why.
    }

    ivec3 extents = world.boundsMax - world.boundsMin + ivec3(1);
    info("  {} block(s) read from {} chunk(s) in {} region file(s).", world.voxels.size(),
        world.chunksRead, world.regionsRead);
    info("  Extents (W/H/D): {} x {} x {} blocks", extents.x, extents.y, extents.z);

    // The palette is built from the blocks that were actually read, weighted by how often each
    // color occurs — the same median-cut path the mesh front end uses, just fed voxels instead of
    // surface samples.
    info("Building material palette...");
    ColorHistogram histogram;
    for (const minecraft::Voxel& voxel : world.voxels) {
        histogram.add(voxel.color.r, voxel.color.g, voxel.color.b);
    }
    ColorPalette palette = buildColorPalette(histogram.populatedCells());
    info("  {} material(s) in palette (cap {}).", palette.colors.size(), PALETTE_CAPACITY);

    // One block is one voxel, so the grid is just the selection rounded up to whole chunks.
    int largestExtent = std::max({extents.x, extents.y, extents.z});
    uint chunkResolution = (largestExtent <= 64) ? 64u : 256u;
    uint numberOfChunksPerAxis = uint(std::ceil(largestExtent / float(chunkResolution)));
    float voxelScale = 1.0f;
    float chunkScale = voxelScale * float(chunkResolution);
    uint totalChunks = numberOfChunksPerAxis * numberOfChunksPerAxis * numberOfChunksPerAxis;

    info("Voxelizing: up to {} chunk(s) ({} per axis, {}^3 each)", totalChunks, numberOfChunksPerAxis,
        chunkResolution);

    // Bucket the blocks by destination chunk so each chunk is built in one pass. Unlike the mesh
    // path there is nothing to intersect — a block's chunk is pure arithmetic on its position.
    std::unordered_map<uint32_t, std::vector<minecraft::Voxel>> voxelsByChunk;
    for (const minecraft::Voxel& voxel : world.voxels) {
        ivec3 gridPosition = voxel.position - world.boundsMin;
        ivec3 chunkPosition = gridPosition / int(chunkResolution);
        uint32_t chunkIndex = uint32_t(projv::utils::createZOrderIndex(chunkPosition));
        voxelsByChunk[chunkIndex].push_back({gridPosition, voxel.color});
    }

    projv::DataFile dataFile;
    dataFile.resolution = chunkResolution;
    dataFile.voxelScale = voxelScale;

    size_t totalVoxels = 0;
    size_t chunksWritten = 0;
    for (const auto& [chunkIndex, voxels] : voxelsByChunk) {
        ivec3 chunkIndexPosition = projv::utils::reverseZOrderIndex(chunkIndex);

        projv::ChunkHeader chunkHeader;
        chunkHeader.chunkID = chunkIndex;
        chunkHeader.position = vec3(chunkIndexPosition) * vec3(chunkScale);
        chunkHeader.voxelScale = voxelScale;
        chunkHeader.resolution = chunkResolution;
        chunkHeader.scale = projv::utils::createChunkScaleFromVoxelScaleAndResolution(voxelScale, int(chunkResolution));

        auto brickMap = projv::utils::createVoxelBrickMap(
            projv::utils::computeBrickDims(chunkResolution));

        for (const minecraft::Voxel& voxel : voxels) {
            ivec3 local = voxel.position - chunkIndexPosition * int(chunkResolution);
            projv::utils::brickMapSetVoxel(*brickMap, local.x, local.y, local.z,
                palette.idFor(voxel.color.r, voxel.color.g, voxel.color.b));
        }

        size_t chunkVoxels = 0;
        for (uint32_t brickIndex = 0; brickIndex < brickMap->totalBricks; brickIndex++) {
            if (!brickMap->bricks[brickIndex]) continue;
            for (uint32_t row = 0; row < projv::BRICK_MASK_ROWS; row++)
                chunkVoxels += __builtin_popcountll(brickMap->bricks[brickIndex]->mask[row]);
        }
        if (chunkVoxels == 0) continue;
        totalVoxels += chunkVoxels;

        projv::Chunk chunk = projv::utils::createChunk(chunkHeader);
        projv::utils::updateChunkFromBrickMap(chunk, *brickMap);

        projv::DataBlock block;
        block.gridX = chunkIndexPosition.x;
        block.gridY = chunkIndexPosition.y;
        block.gridZ = chunkIndexPosition.z;
        block.geometry = std::move(chunk.geometryData);
        projv::utils::bakeMaterialsFromBrickMap(block.geometry, block.materialIDs, *brickMap);
        dataFile.blocks.push_back(std::move(block));
        chunksWritten++;
    }

    std::string sceneName = worldPath.stem().string();
    if (sceneName.empty()) sceneName = "world";
    writeComposeScene(dataFile, outputDirectory, sceneName, palette);

    info("--------------------------------------------");
    info("  Voxelization Summary");
    info("  World:           {}", worldPath.string());
    info("  Dimension:       {}", options.dimension);
    info("  Regions/Chunks:  {} / {}", world.regionsRead, world.chunksRead);
    info("  Bounds:          ({}, {}, {})  ->  ({}, {}, {})",
        world.boundsMin.x, world.boundsMin.y, world.boundsMin.z,
        world.boundsMax.x, world.boundsMax.y, world.boundsMax.z);
    info("  Extents (W/H/D): {} x {} x {} blocks", extents.x, extents.y, extents.z);
    info("  Chunks:          {} written ({}^3 each)", chunksWritten, chunkResolution);
    if (world.texturesSampled > 0)
        info("  Pack textures:   {} sampled", world.texturesSampled);
    info("  Materials:       {} palette entries", palette.colors.size());
    info("  Total voxels:    {}", totalVoxels);
    info("  Output:          {}/model.data + {}/compose.json", outputDirectory, outputDirectory);
    info("--------------------------------------------");

    if (world.chunksFailed > 0)
        projv::core::warn("{} chunk(s) could not be read and were skipped.", world.chunksFailed);
    if (!world.unknownBlocks.empty()) {
        // Worth surfacing: these came out neutral gray, and a resource pack would fix them.
        std::string sample;
        for (size_t i = 0; i < world.unknownBlocks.size() && i < 12; i++) {
            sample += (i > 0 ? ", " : "") + world.unknownBlocks[i];
        }
        projv::core::warn("{} block type(s) had no known color and were drawn gray ({}{}). "
                          "Pass --mc-resource-pack to sample their real textures.",
                          world.unknownBlocks.size(), sample,
                          world.unknownBlocks.size() > 12 ? ", ..." : "");
    }
}

// A Minecraft save is a directory, not a file, so it is recognized by what it contains: level.dat
// beside a region directory. A bare region directory or a single .mca file is accepted too, since
// those are reasonable things to point at when pulling one area out of a world.
bool isMinecraftWorld(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) return path.extension() == ".mca";
    if (!std::filesystem::is_directory(path, error)) return false;

    if (path.filename() == "region") return true;
    if (std::filesystem::exists(path / "level.dat", error)) return true;
    return std::filesystem::is_directory(path / "region", error) ||
           std::filesystem::is_directory(path / "DIM-1" / "region", error) ||
           std::filesystem::is_directory(path / "DIM1" / "region", error);
}

int main(int argc, char** argv) {
    CLI::App app{"A voxelizer to convert polygon models (obj, fbx, gltf/glb, dae, stl, ply, 3ds, "
                 "blend, ...) and Minecraft worlds to voxel ProjectV models."};
    argv = app.ensure_utf8(argv);

    std::string assetDirectory = "";
    std::string modelFile = "";
    std::string outputDirectory = "";
    std::string resolutionString = "256";
    bool noFlipV = false;
    bool flipV = false;
    bool listFormats = false;
    double alphaCutoffOption = double(ALPHA_CUTOFF_DEFAULT) / 255.0;
    bool noAlphaCutoff = false;
    unsigned int threadCount = 0;   // 0 = decide from hardware_concurrency below.
    DisplaceOptions displaceOptions;
    std::string displaceZero = "mid";
    std::string displacePolarity = "auto";

    minecraft::ImportOptions minecraftOptions;
    std::vector<int> minecraftBounds;
    std::vector<int> minecraftYRange;
    std::string resourcePack = "";
    bool minecraftNoWater = false;
    bool forceMinecraft = false;

    app.add_option("-m, --modelDir, -a, --assetDir", assetDirectory,
        "Root directory to search for the model's textures. Defaults to the model file's own "
        "directory.");
    app.add_option("-f, --file, --objDir", modelFile, "Path to the model file to voxelize.");
    app.add_option("-o, --outputDir", outputDirectory, "Path to put the generated scene.");
    app.add_option("-r, --resolution", resolutionString, "Resolution to voxelize the scene at.");
    // Tri-state: the sensible flip differs per format, so the default is chosen from the file
    // extension and either flag pins it explicitly.
    app.add_flag("--flip-v", flipV, "Force the V texture coordinate to be flipped.");
    app.add_flag("--no-flip-v", noFlipV,
        "Sample textures without flipping the V coordinate. The default is chosen per format — "
        "flipped for OBJ/FBX/Collada (bottom-left origin), unflipped for glTF (top-left origin) — "
        "but individual models disagree with their own format often enough to need an override, "
        "particularly 3ds Max exports. The symptom is a model sampling the mirrored half of its "
        "atlas: foliage comes out uniformly gray, or a trunk takes on the leaves' color.");
    app.add_option("--alpha-cutoff", alphaCutoffOption,
        "Alpha below which a texel produces no voxel, as 0..1 (default: 0.5). Alpha-tested foliage "
        "stores its leaf silhouette in the texture's alpha channel and leaves the rest of the quad "
        "transparent, so ignoring alpha voxelizes every leaf card as a solid slab.")
        ->check(CLI::Range(0.0, 1.0));
    app.add_flag("--no-alpha-cutoff", noAlphaCutoff,
        "Treat every texel as solid regardless of alpha. This is the pre-alpha behaviour; use it for "
        "a model whose alpha channel is junk rather than a cutout mask.");
    app.add_option("--threads", threadCount,
        "Worker threads for voxelization (default: as many as the machine reports). Chunks are "
        "independent, so this only changes wall clock — the .data is byte-identical either way.");
    app.add_option("--displace-scale", displaceOptions.scaleMeters,
        "Peak-to-trough displacement in model units, taken from each material's height/bump map. 0 "
        "(the default) disables displacement. Relief is applied along the surface normal, so mortar "
        "lines and carving become real geometry instead of texture. Only pays off when it is worth "
        "more than a voxel: at -r 8192 on a 69 m scene a voxel is 8.4 mm.")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--displace-zero", displaceZero,
        "Which height counts as the original surface: 'mid' (0.5, the authoring convention) or "
        "'mean' (each map's own average). Use 'mean' for maps that sit far from mid-gray, where "
        "treating 0.5 as neutral shifts the whole surface instead of adding relief around it.")
        ->check(CLI::IsMember({"mid", "mean"}));
    app.add_option("--displace-polarity", displacePolarity,
        "Which way height maps read: 'auto' (default, decided per map from its own histogram), "
        "'bright' (white protrudes — the nominal convention), or 'dark' (black protrudes). Assets "
        "routinely mix both because their maps came from different sources, which is why auto is "
        "per-map rather than one global choice.")
        ->check(CLI::IsMember({"auto", "bright", "dark"}));
    app.add_option("--displace-thickness", displaceOptions.thicknessVoxels,
        "Thickness in voxels of the displaced surface shell (default: 1). Raise it if a steep height "
        "gradient leaves pinholes; the effective minimum is half a voxel diagonal.")
        ->check(CLI::PositiveNumber);
    app.add_flag("--list-formats", listFormats, "Print every model format this build can read, then exit.");

    // --- Minecraft world options ---
    // These apply only when the input is a world; a Minecraft save is detected automatically, so
    // --minecraft is only needed for a save in an unusual layout.
    app.add_flag("--minecraft", forceMinecraft,
        "Treat the input as a Minecraft world even if it does not look like one.");
    app.add_option("--mc-dimension", minecraftOptions.dimension,
        "Minecraft dimension to import: overworld, nether, or end (default: overworld).")
        ->check(CLI::IsMember({"overworld", "nether", "end"}));
    app.add_option("--mc-bounds", minecraftBounds,
        "Block area to import as: minX minZ maxX maxZ. Defaults to the world's extent, clamped "
        "to --mc-area around its center.")
        ->expected(4);
    app.add_option("--mc-y", minecraftYRange,
        "Vertical range to import as: minY maxY. Defaults to the world's full height.")
        ->expected(2);
    app.add_option("--mc-area", minecraftOptions.autoBoundsLimit,
        "Width in blocks of the area imported when --mc-bounds is not given (default: 512).");
    app.add_option("--mc-max-voxels", minecraftOptions.maxVoxels,
        "Stop after this many blocks, as a memory guard (default: 30000000).");
    app.add_option("--mc-resource-pack", resourcePack,
        "Resource pack directory (the one holding assets/) to sample real block colors from. "
        "Without one, colors come from a built-in table covering the common blocks.");
    app.add_flag("--mc-no-water", minecraftNoWater, "Skip water, leaving oceans and lakes hollow.");

    CLI11_PARSE(app, argc, argv);

    if (listFormats) {
        projv::core::info("Supported model formats:");
        projv::core::info("  {}", meshimport::supportedExtensions());
        projv::core::info("Also supported: Minecraft (Java Edition) worlds — pass the world directory.");
        return 0;
    }

    if (modelFile.empty()) {
        projv::core::error("No input model given. Pass one with -f, or see --help.");
        return 1;
    }
    std::filesystem::path modelPath = modelFile;
    if (!std::filesystem::exists(modelPath)) {
        projv::core::error("Input model does not exist: {}", modelPath.string());
        return 1;
    }
    if (flipV && noFlipV) {
        projv::core::error("--flip-v and --no-flip-v are mutually exclusive.");
        return 1;
    }

    if (forceMinecraft || isMinecraftWorld(modelPath)) {
        if (minecraftBounds.size() == 4) {
            minecraftOptions.hasBounds = true;
            minecraftOptions.minX = std::min(minecraftBounds[0], minecraftBounds[2]);
            minecraftOptions.maxX = std::max(minecraftBounds[0], minecraftBounds[2]);
            minecraftOptions.minZ = std::min(minecraftBounds[1], minecraftBounds[3]);
            minecraftOptions.maxZ = std::max(minecraftBounds[1], minecraftBounds[3]);
        }
        if (minecraftYRange.size() == 2) {
            minecraftOptions.minY = std::min(minecraftYRange[0], minecraftYRange[1]);
            minecraftOptions.maxY = std::max(minecraftYRange[0], minecraftYRange[1]);
        }
        minecraftOptions.resourcePack = resourcePack;
        minecraftOptions.includeWater = !minecraftNoWater;

        // A block is a voxel here, so there is no resolution to choose — the grid follows the
        // selected area. Saying so is better than silently ignoring the flag.
        if (!resolutionString.empty() && resolutionString != "256") {
            projv::core::warn("-r does not apply to Minecraft worlds (one block is always one voxel). "
                              "Use --mc-bounds to control the size of the import.");
        }

        voxelizeMinecraftWorld(modelPath, minecraftOptions, outputDirectory);
        return 0;
    }

    int resolution = std::stoi(resolutionString, 0, 10);

    // The asset root defaults to the model's own directory, which is where a self-contained model
    // keeps its textures. Only multi-model packs with a shared texture folder need -m.
    std::filesystem::path assetPath = assetDirectory.empty()
        ? modelPath.parent_path()
        : std::filesystem::path(assetDirectory);

    bool flipTextureV = meshimport::defaultFlipVForFormat(modelPath.extension().string());
    if (flipV) flipTextureV = true;
    if (noFlipV) flipTextureV = false;

    // 0 disables the test entirely, which is what --no-alpha-cutoff asks for. Otherwise the 0..1
    // option becomes the 0..255 byte the sampler compares against.
    uint8_t alphaCutoff = noAlphaCutoff
        ? uint8_t(0)
        : uint8_t(std::clamp(int(std::lround(alphaCutoffOption * 255.0)), 0, 255));
    if (threadCount == 0) {
        threadCount = std::max(1u, std::thread::hardware_concurrency());
    }
    displaceOptions.zeroAtMean = (displaceZero == "mean");
    displaceOptions.polarity = displacePolarity == "bright" ? HeightPolarity::BrightIsHigh
                            : displacePolarity == "dark"   ? HeightPolarity::DarkIsHigh
                                                           : HeightPolarity::Auto;

    voxelizeModel(modelPath, assetPath, resolution, outputDirectory, flipTextureV, alphaCutoff,
                  threadCount, displaceOptions);

    return 0;
}
