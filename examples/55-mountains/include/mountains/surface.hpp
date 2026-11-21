// What the ground is made of (plan D9), shared by the lab (which blends the classes into a colour) and
// the voxels (which pick one class a voxel, dithered by the weights, so a coarse voxel a pixel wide
// averages like the blend).
//
// The season is October by default -- golden aspen groves, rust Gambel oak on the mid slopes, gold grama,
// a first dusting of snow on the peaks -- or November (MOUNTAINS_SEASON=november): bare aspens, more snow.
#pragma once

#include <cstdlib>
#include <cstring>

#include "mountains/landform.hpp"

namespace mountains {

enum class Material : uint8_t {
    // ground
    Soil, RedSoil, Grama, Sediment, Sand, Tundra,
    // canopy
    Juniper, Pinyon, Ponderosa, Fir, Spruce, AspenGold, AspenBare, Oak, Chamisa, ChamisaBloom,
    // wood
    Bark, PonderosaBark, AspenTrunk, Deadwood,
    // rock, snow, water
    Granite, Gneiss, Lichen, Basalt, Snow, Water,
    Count
};
constexpr int kMaterialCount = int(Material::Count);
constexpr int kShades = 8;   // per material: 26 x 8 = 208 palette slots of the 255

inline bool november() {
    static const bool v = [] {
        const char* s = std::getenv("MOUNTAINS_SEASON");
        return s && std::strcmp(s, "november") == 0;
    }();
    return v;
}

struct Rgb { double r = 0, g = 0, b = 0; };
inline Rgb operator*(Rgb a, double k) { return {a.r * k, a.g * k, a.b * k}; }
inline Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline Rgb lerp(Rgb a, Rgb b, double t) { return a * (1.0 - t) + b * t; }

// Albedo, display (sRGB-ish) values.
inline Rgb materialColour(Material m) {
    switch (m) {
        case Material::Soil: return {0.71, 0.59, 0.52};          // pinkish adobe tan
        case Material::RedSoil: return {0.70, 0.47, 0.38};       // the red earth of the foothills
        case Material::Grama: return {0.76, 0.66, 0.45};         // blue grama gone gold
        case Material::Sediment: return {0.82, 0.69, 0.60};      // Tesuque Fm., pink-tan
        case Material::Sand: return {0.84, 0.77, 0.66};          // pale wash sand
        case Material::Tundra: return {0.66, 0.56, 0.36};        // alpine grass, tawny
        case Material::Juniper: return {0.31, 0.38, 0.29};       // grey-blue-green
        case Material::Pinyon: return {0.22, 0.31, 0.17};        // deeper, warmer green
        case Material::Ponderosa: return {0.28, 0.35, 0.16};     // yellow-green needles
        case Material::Fir: return {0.12, 0.21, 0.13};
        case Material::Spruce: return {0.14, 0.22, 0.21};        // blue-green
        case Material::AspenGold: return {0.95, 0.70, 0.12};
        case Material::AspenBare: return {0.60, 0.56, 0.52};
        case Material::Oak: return {0.68, 0.33, 0.13};           // Gambel oak in October, rust
        case Material::Chamisa: return {0.55, 0.58, 0.46};       // rabbitbrush, grey-green
        case Material::ChamisaBloom: return {0.80, 0.70, 0.30};  // its faded yellow heads
        case Material::Bark: return {0.36, 0.27, 0.21};
        case Material::PonderosaBark: return {0.66, 0.38, 0.22}; // cinnamon plates
        case Material::AspenTrunk: return {0.88, 0.87, 0.80};
        case Material::Deadwood: return {0.56, 0.54, 0.50};      // weathered silver-grey
        case Material::Granite: return {0.70, 0.52, 0.47};       // pink-red
        case Material::Gneiss: return {0.52, 0.50, 0.50};
        case Material::Lichen: return {0.80, 0.55, 0.18};        // orange lichen on the rock
        case Material::Basalt: return {0.24, 0.22, 0.22};
        case Material::Snow: return {0.93, 0.95, 0.99};
        case Material::Water: return {0.20, 0.30, 0.38};
        default: return {1, 0, 1};
    }
}
// Shade s of material m: 0.68 .. 1.0 of the class colour, warmer as it brightens and cooler as it
// darkens, as sunlit and shaded foliage and rock are.
inline Rgb shadeColour(Material m, int s) {
    const double t = double(s) / double(kShades - 1);
    const double k = 0.68 + 0.32 * t;
    Rgb c = materialColour(m) * k;
    c.r *= 1.0 + 0.06 * (t - 0.5);
    c.b *= 1.0 - 0.08 * (t - 0.5);
    return c;
}

// The mix at a point: what a map pixel blends, and what a voxel draws one of.
struct SurfaceMix {
    double water = 0;     // river or tarn
    double snow = 0;
    double rock = 0;      // bare rock, of `rockA` / `rockB` by `rockMix`
    Material rockA = Material::Granite, rockB = Material::Gneiss;
    double rockMix = 0;
    double cover = 0;     // tree canopy, of `treeA` / `treeB` by `treeMix`
    Material treeA = Material::Pinyon, treeB = Material::Pinyon;
    double treeMix = 0;
    double understory = 0;   // shrubs under or between the trees
    Material shrub = Material::Chamisa;
    Material groundA = Material::Soil, groundB = Material::Grama;   // under it all, by `groundMix`
    double groundMix = 0;
    double shade = 0.5;   // 0..1, broad brightness variation
};

inline SurfaceMix surfaceMix(const Sample& s, double x, double z, double footprint) {
    SurfaceMix m;
    const double slope = std::sqrt(s.gx * s.gx + s.gz * s.gz);
    const double aspectW = smoothstep(0.03, 0.2, slope);
    const double northness = slope > 1e-6 ? -s.gz / slope * aspectW : 0.0;
    const double h = s.height;
    const double e = h + 170.0 * northness;   // north slopes live higher

    // Ground: tan earth and gold grass in broad patches, red earth in places, pale sand in the washes.
    m.groundMix = clamp01(0.5 + 1.1 * fbm(x / 700.0, z / 700.0, 3, 71));
    const double red = smoothstep(0.05, 0.3, fbm(x / 2600.0 + 3.0, z / 2600.0, 3, 72));
    if (red > 0.5 && e < 2900.0) m.groundA = Material::RedSoil;
    const double wash = (1.0 - smoothstep(0.02, 0.06, slope)) * smoothstep(0.1, 0.35, fbm(x / 400.0, z / 400.0, 2, 73)) *
                        (1.0 - smoothstep(2300.0, 2500.0, e));
    if (wash > 0.5) m.groundA = Material::Sand;
    const double sediment = s.badland * smoothstep(0.08, 0.3, slope);
    if (sediment > 0.5) { m.groundA = Material::RedSoil; m.groundB = Material::Sediment; m.groundMix = sediment; }
    const double tundra = smoothstep(3450.0, 3600.0, e);
    if (tundra > 0.0) { m.groundA = Material::Grama; m.groundB = Material::Tundra; m.groundMix = tundra; }

    // Trees.
    double cover = 0.0;
    const double pjMix = clamp01(0.45 + 0.9 * fbm(x / 300.0, z / 300.0, 2, 74));   // juniper or pinyon stands
    if (e < 2300.0) {
        cover = 0.18 * smoothstep(1880.0, 2000.0, e);
        m.treeA = Material::Juniper; m.treeB = Material::Pinyon; m.treeMix = pjMix * smoothstep(1950.0, 2250.0, e);
    } else if (e < 2600.0) {
        const double t = smoothstep(2300.0, 2600.0, e);
        cover = mix(0.18, 0.5, t);
        m.treeA = Material::Pinyon; m.treeB = Material::Ponderosa; m.treeMix = t;
    } else if (e < 3000.0) {
        cover = mix(0.5, 0.75, smoothstep(2600.0, 3000.0, e));
        m.treeA = Material::Ponderosa; m.treeB = Material::Fir; m.treeMix = smoothstep(2550.0, 2800.0, e);
    } else {
        cover = 0.78 * (1.0 - smoothstep(3450.0, 3600.0, e));
        m.treeA = Material::Fir; m.treeB = Material::Spruce; m.treeMix = smoothstep(3000.0, 3200.0, e);
    }
    // Aspen groves: clones, in patches through the mixed conifer and the spruce-fir.
    // Broad: in October the upper slopes are swaths of gold kilometres across.
    const double aspen = smoothstep(0.02, 0.2, fbm(x / 2400.0, z / 2400.0, 3, 91)) *
                         smoothstep(2600.0, 2800.0, e) * (1.0 - smoothstep(3300.0, 3450.0, e));
    if (aspen > 0.5) {
        m.treeA = m.treeMix > 0.5 ? m.treeB : m.treeA;
        m.treeB = november() ? Material::AspenBare : Material::AspenGold;
        m.treeMix = smoothstep(0.5, 0.8, aspen);
        cover = std::max(cover, 0.6);
    }
    cover *= 1.0 - 0.35 * std::max(-northness, 0.0) * (1.0 - smoothstep(2500.0, 2900.0, e));
    cover *= clamp01(0.75 + 0.6 * fbm(x / 700.0, z / 700.0, 3, 93));
    cover *= 1.0 - smoothstep(0.9, 1.3, slope);
    cover *= 1.0 - s.basalt * 0.6;
    // At a fine footprint open woodland is crowns, not a tint: a noise spread to roughly uniform on
    // 0..1, under the cover, so the crowns take the same share of the ground the tint stands for (and
    // a fine ring has as many trees as the coarser ring beside it).
    if (footprint < 6.0) {
        const double crowns = clamp01(0.5 + 1.7 * fbm(x / 5.0, z / 5.0, 2, 95));
        cover = 1.0 - smoothstep(cover - 0.03, cover + 0.03, crowns);
    }
    m.cover = cover;

    // Understory: chamisa in the open piedmont, Gambel oak thickets on the mid slopes (south-facing more).
    const double oak = smoothstep(0.1, 0.35, fbm(x / 500.0, z / 500.0, 3, 75)) *
                       smoothstep(2200.0, 2400.0, e) * (1.0 - smoothstep(2800.0, 3000.0, e)) *
                       (0.6 + 0.4 * std::max(-northness, 0.0));
    const double chamisa = 0.12 * (1.0 - smoothstep(2250.0, 2400.0, e)) *
                           smoothstep(-0.2, 0.2, fbm(x / 250.0, z / 250.0, 2, 76));
    if (oak > chamisa) { m.shrub = november() ? Material::Bark : Material::Oak; m.understory = 0.55 * oak; }
    else { m.shrub = Material::Chamisa; m.understory = chamisa; }
    m.understory *= 1.0 - smoothstep(0.8, 1.2, slope);

    // Rock: pink granite and grey gneiss in big bodies, sediment off the cores, basalt on the caps.
    m.rockMix = clamp01(0.5 + 0.9 * fbm(x / 1800.0, z / 1800.0, 3, 97));
    if (s.core < 0.5) { m.rockA = Material::Sediment; m.rockB = Material::RedSoil; }
    if (s.basalt > 0.5) { m.rockA = Material::Basalt; m.rockB = Material::Basalt; }
    m.rock = std::max({smoothstep(0.85, 1.3, slope), s.cirque * 0.6, s.basalt * smoothstep(0.35, 0.7, slope)});

    // Snow: low on north faces, only the tops on south ones. October: a first dusting; November: more.
    const double snowline = (november() ? 3450.0 : 3620.0) - 250.0 * northness;
    double snow = smoothstep(snowline, snowline + 180.0, h) * (1.0 - smoothstep(1.0, 1.5, slope));
    m.snow = snow * (1.0 - 0.55 * cover);

    m.water = std::max(s.river, s.cirque > 0.9 ? 0.8 : 0.0);
    m.shade = clamp01(0.5 + 0.9 * fbm(x / 60.0, z / 60.0, 2, 99));
    return m;
}

// How tall the plants of a class stand, m: in rings too coarse to draw them, a canopy voxel stands up
// off the ground when its plants are at least ~0.6 of a voxel tall.
inline double canopyHeight(Material m) {
    switch (m) {
        case Material::Juniper: return 3.5;
        case Material::Pinyon: return 5.0;
        case Material::Ponderosa: return 19.0;
        case Material::Fir: return 18.0;
        case Material::Spruce: return 21.0;
        case Material::AspenGold:
        case Material::AspenBare: return 13.0;
        case Material::Oak: return 2.5;
        case Material::Chamisa: return 0.8;
        default: return 0.0;
    }
}

// The blended colour (the lab).
inline Rgb mixColour(const SurfaceMix& m) {
    const Rgb ground = lerp(materialColour(m.groundA), materialColour(m.groundB), m.groundMix);
    const Rgb tree = lerp(materialColour(m.treeA), materialColour(m.treeB), m.treeMix);
    const Rgb rock = lerp(materialColour(m.rockA), materialColour(m.rockB), m.rockMix);
    Rgb c = lerp(ground, materialColour(m.shrub), m.understory);
    c = lerp(c, tree, m.cover);
    c = lerp(c, rock, m.rock);
    c = lerp(c, materialColour(Material::Snow), m.snow);
    c = lerp(c, materialColour(Material::Water), m.water);
    return c * (0.9 + 0.2 * m.shade);
}

// One material for a voxel, and its shade. Each choice compares a weight against a threshold: a
// spatial noise (`coherent`, 0..1) where voxels are big on screen, so neighbours agree and the ground
// has patches rather than a checkerboard, blending to `hash` (a per-voxel random) by `dither` where
// a voxel is a pixel or less and the dither is what averages to the mix. `treesDrawn`, `shrubsDrawn`:
// where they are voxels of their own, so the ground under them is ground.
inline void pickMaterial(const SurfaceMix& m, uint32_t hash, double coherent, double dither, bool treesDrawn,
                         bool shrubsDrawn, Material& material, int& shade) {
    auto u = [&](int k) {
        const double r = unit(hash32(hash + uint32_t(k) * 0x9e3779b9U));
        const double c = std::fmod(coherent + 0.618034 * k, 1.0);
        return mix(c, r, dither);
    };
    if (m.water > u(1)) material = Material::Water;
    else if (m.snow > u(2)) material = Material::Snow;
    else if (m.rock > u(3)) {
        material = m.rockMix > u(4) ? m.rockB : m.rockA;
        // Lichen on the granite and gneiss: specks up close, a warm cast from afar.
        if ((material == Material::Granite || material == Material::Gneiss) && unit(hash32(hash + 77U)) < 0.09)
            material = Material::Lichen;
    }
    else if (!treesDrawn && m.cover > u(5)) material = m.treeMix > u(6) ? m.treeB : m.treeA;
    else if (!shrubsDrawn && m.understory > u(9)) material = m.shrub;
    else material = m.groundMix > u(7) ? m.groundB : m.groundA;
    const double s = m.shade + (u(8) - 0.5) * 0.35;
    shade = std::clamp(int(s * kShades), 0, kShades - 1);
}

// What lies under the surface voxel, seen on cliffs and cut banks.
inline Material subsurface(const SurfaceMix& m) {
    if (m.rock > 0.3 || m.snow > 0.5) return m.rockMix > 0.5 ? m.rockB : m.rockA;
    if (m.groundA == Material::Tundra || m.groundB == Material::Tundra) return Material::Gneiss;
    return m.groundA == Material::RedSoil ? Material::RedSoil : Material::Soil;
}

}  // namespace mountains
