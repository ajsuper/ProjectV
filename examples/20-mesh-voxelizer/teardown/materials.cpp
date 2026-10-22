#include "materials.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace teardown {

namespace {
    const char* const NAMES[] = {"none", "glass", "foliage", "dirt", "rock", "wood", "masonry", "brick",
                                 "plaster", "metal", "heavymetal", "plastic", "hardmetal", "hardmasonry",
                                 "ice", "unphysical"};
    static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == size_t(Substance::Count));

    uint8_t toByte(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
}

const char* substanceName(Substance s) { return NAMES[size_t(s)]; }

Substance substanceFromName(const std::string& name) {
    for (size_t i = 0; i < size_t(Substance::Count); i++)
        if (name == NAMES[i]) return Substance(i);
    if (name == "grass") return Substance::Foliage;
    if (name == "concrete") return Substance::Masonry;
    if (name == "weakmetal") return Substance::Metal;
    return Substance::None;
}

// Rows of eight, bottom-up, from data/built-in/palette.vox's NOTE chunk.
Substance substanceFromPaletteIndex(int i) {
    if (i <= 0)   return Substance::None;
    if (i <= 8)   return Substance::Glass;
    if (i <= 24)  return Substance::Foliage;
    if (i <= 40)  return Substance::Dirt;
    if (i <= 56)  return Substance::Rock;
    if (i <= 72)  return Substance::Wood;
    if (i <= 88)  return Substance::Masonry; // concrete
    if (i <= 104) return Substance::Brick;
    if (i <= 120) return Substance::Plaster;
    if (i <= 136) return Substance::Metal;   // "weak metal"
    if (i <= 152) return Substance::HeavyMetal;
    if (i <= 168) return Substance::Plastic;
    if (i <= 176) return Substance::HardMetal;
    if (i <= 184) return Substance::HardMasonry;
    if (i <= 224) return Substance::None;    // reserved
    if (i <= 240) return Substance::Unphysical;
    return Substance::None;                  // reserved
}

MatId MaterialTable::intern(const VoxMaterial& m) {
    auto it = index_.find(m);
    if (it != index_.end()) return it->second;
    if (materials_.size() >= MAT_HOLE) throw std::runtime_error("more than 65534 distinct materials");
    MatId id = MatId(materials_.size());
    materials_.push_back(m);
    index_.emplace(m, id);
    return id;
}

MaterialTable& materials() {
    static MaterialTable table;
    return table;
}

VoxMaterial makeMaterial(Substance s, float r, float g, float b, float alpha, float reflect, float shiny, float metal,
                         float emissive) {
    VoxMaterial m;
    m.r = toByte(r);
    m.g = toByte(g);
    m.b = toByte(b);
    m.substance = s;
    m.alpha = toByte(s == Substance::Glass && alpha >= 1.0f ? 0.5f : alpha);
    m.gloss = toByte(std::max(reflect, shiny * 0.5f));
    m.metal = toByte(metal);
    m.emissive = uint8_t(std::clamp(std::lround(emissive * 16.0f), 0L, 255L));
    return m;
}

} // namespace teardown
