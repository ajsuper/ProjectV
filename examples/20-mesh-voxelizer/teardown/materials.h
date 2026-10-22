#ifndef PROJECTV_TEARDOWN_MATERIALS_H
#define PROJECTV_TEARDOWN_MATERIALS_H
// Voxel materials: what a Teardown voxel is made of and how it looks, interned into one table so
// a canvas can store a 16-bit id per voxel.
//
// Teardown decides a voxel's *substance* by its palette index -- rows of 8 in the MagicaVoxel
// palette, documented in the game's own data/built-in/palette.vox NOTE chunk -- and its *look* by
// the palette colour plus the MATL chunk (metal / glass / emit and their parameters). Both survive
// here: the substance becomes a ProjectV physics material name, the look becomes a palette entry.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace teardown {

enum class Substance : uint8_t {
    None = 0, Glass, Foliage, Dirt, Rock, Wood, Masonry, Brick, Plaster, Metal, HeavyMetal,
    Plastic, HardMetal, HardMasonry, Ice, Unphysical, Count
};

const char* substanceName(Substance s);             // Teardown's own name ("heavymetal")
Substance   substanceFromName(const std::string& s); // CreateMaterial / voxbox material="..."
Substance   substanceFromPaletteIndex(int index);    // 1..255

// The look and substance of one voxel. Byte-quantised so equal-looking materials intern to one id.
struct VoxMaterial {
    uint8_t r = 255, g = 255, b = 255; // sRGB
    Substance substance = Substance::None;
    uint8_t gloss = 0;     // 0..255
    uint8_t metal = 0;     // 0..255
    uint8_t alpha = 255;   // 255 opaque
    uint8_t emissive = 0;  // 0..255, strength code: strength = emissive / 16

    bool operator==(const VoxMaterial&) const = default;
};

struct VoxMaterialHash {
    size_t operator()(const VoxMaterial& m) const {
        uint64_t k = uint64_t(m.r) | uint64_t(m.g) << 8 | uint64_t(m.b) << 16 | uint64_t(m.substance) << 24 |
                     uint64_t(m.gloss) << 32 | uint64_t(m.metal) << 40 | uint64_t(m.alpha) << 48 |
                     uint64_t(m.emissive) << 56;
        return std::hash<uint64_t>()(k * 0x9E3779B97F4A7C15ull);
    }
};

using MatId = uint16_t;
constexpr MatId MAT_EMPTY = 0;
constexpr MatId MAT_HOLE  = 0xFFFF; // carves its parent shape; never reaches an output canvas

class MaterialTable {
public:
    MaterialTable() { materials_.push_back({}); } // id 0 = empty
    MatId intern(const VoxMaterial& m);
    const VoxMaterial& get(MatId id) const { return materials_[id]; }
    size_t size() const { return materials_.size(); }

private:
    std::vector<VoxMaterial> materials_;
    std::unordered_map<VoxMaterial, MatId, VoxMaterialHash> index_;
};

// The process-wide table. Conversion is single-level-at-a-time and the table only grows.
MaterialTable& materials();

// Teardown material parameters, as CreateMaterial and voxbox `pbr` give them.
VoxMaterial makeMaterial(Substance s, float r, float g, float b, float alpha = 1.0f, float reflect = 0.0f,
                         float shiny = 0.0f, float metal = 0.0f, float emissive = 0.0f);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_MATERIALS_H
