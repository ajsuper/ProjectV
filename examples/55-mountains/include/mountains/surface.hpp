// What the ground is made of: a first pass at the plan's M4/M5 bands (D9), shared by the lab (which
// blends the classes into a colour) and the voxels (which pick one class a voxel, dithered by the
// weights, so a coarse voxel a pixel wide averages like the blend).
#pragma once

#include "mountains/landform.hpp"

namespace mountains {

enum class Material : uint8_t {
    Soil, Grama, Sediment, Pinyon, Ponderosa, Fir, Spruce, AspenBare, Tundra,
    Granite, Gneiss, Basalt, Snow, Water, Count
};
constexpr int kMaterialCount = int(Material::Count);
constexpr int kShades = 16;   // per material: 14 x 16 = 224 palette slots of the 255

struct Rgb { double r = 0, g = 0, b = 0; };
inline Rgb operator*(Rgb a, double k) { return {a.r * k, a.g * k, a.b * k}; }
inline Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline Rgb lerp(Rgb a, Rgb b, double t) { return a * (1.0 - t) + b * t; }

// Albedo, display (sRGB-ish) values.
inline Rgb materialColour(Material m) {
    switch (m) {
        case Material::Soil: return {0.72, 0.58, 0.45};
        case Material::Grama: return {0.72, 0.63, 0.46};
        case Material::Sediment: return {0.82, 0.67, 0.53};
        case Material::Pinyon: return {0.22, 0.28, 0.17};
        case Material::Ponderosa: return {0.19, 0.26, 0.15};
        case Material::Fir: return {0.11, 0.18, 0.12};
        case Material::Spruce: return {0.09, 0.15, 0.12};
        case Material::AspenBare: return {0.56, 0.53, 0.49};
        case Material::Tundra: return {0.60, 0.55, 0.41};
        case Material::Granite: return {0.63, 0.50, 0.46};
        case Material::Gneiss: return {0.50, 0.48, 0.47};
        case Material::Basalt: return {0.26, 0.24, 0.23};
        case Material::Snow: return {0.93, 0.95, 0.99};
        case Material::Water: return {0.22, 0.32, 0.40};
        default: return {1, 0, 1};
    }
}
// Shade s of material m: brightness 0.82 .. 1.12, slightly warmer as it brightens.
inline Rgb shadeColour(Material m, int s) {
    const double t = double(s) / double(kShades - 1);
    const double k = 0.82 + 0.30 * t;
    Rgb c = materialColour(m) * k;
    c.r *= 1.0 + 0.03 * (t - 0.5);
    c.b *= 1.0 - 0.03 * (t - 0.5);
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

    // Ground.
    m.groundMix = clamp01(0.5 + 0.7 * fbm(x / 900.0, z / 900.0, 2, 71));
    const double sediment = s.badland * smoothstep(0.08, 0.3, slope);
    if (sediment > 0.5) { m.groundA = Material::Grama; m.groundB = Material::Sediment; m.groundMix = sediment; }
    const double tundra = smoothstep(3450.0, 3600.0, e);
    if (tundra > 0.0) { m.groundA = Material::Grama; m.groundB = Material::Tundra; m.groundMix = tundra; }

    // Trees.
    double cover = 0.0;
    if (e < 2300.0) {
        cover = 0.25 * smoothstep(1880.0, 2000.0, e);
        m.treeA = m.treeB = Material::Pinyon;
    } else if (e < 2600.0) {
        const double t = smoothstep(2300.0, 2600.0, e);
        cover = mix(0.25, 0.5, t);
        m.treeA = Material::Pinyon; m.treeB = Material::Ponderosa; m.treeMix = t;
    } else if (e < 3000.0) {
        cover = mix(0.5, 0.75, smoothstep(2600.0, 3000.0, e));
        m.treeA = Material::Ponderosa; m.treeB = Material::Fir; m.treeMix = smoothstep(2550.0, 2800.0, e);
    } else {
        cover = 0.78 * (1.0 - smoothstep(3450.0, 3600.0, e));
        m.treeA = Material::Fir; m.treeB = Material::Spruce; m.treeMix = smoothstep(3000.0, 3200.0, e);
        const double aspen = smoothstep(0.25, 0.45, fbm(x / 1500.0, z / 1500.0, 3, 91)) *
                             (1.0 - smoothstep(3300.0, 3450.0, e));
        if (aspen > 0.5) { m.treeA = m.treeB; m.treeB = Material::AspenBare; m.treeMix = aspen; }
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

    // Rock.
    m.rockMix = clamp01(0.5 + 0.7 * fbm(x / 2500.0, z / 2500.0, 2, 97));
    if (s.core < 0.5) { m.rockA = Material::Sediment; m.rockB = Material::Sediment; }
    if (s.basalt > 0.5) { m.rockA = Material::Basalt; m.rockB = Material::Basalt; }
    m.rock = std::max({smoothstep(0.85, 1.3, slope), s.cirque * 0.6, s.basalt * smoothstep(0.35, 0.7, slope)});

    // Late November snow: low on north faces, only the tops on south ones.
    const double snowline = 3450.0 - 250.0 * northness;
    double snow = smoothstep(snowline, snowline + 180.0, h) * (1.0 - smoothstep(1.0, 1.5, slope));
    m.snow = snow * (1.0 - 0.55 * cover);

    m.water = std::max(s.river, s.cirque > 0.9 ? 0.8 : 0.0);
    m.shade = clamp01(0.5 + 0.8 * fbm(x / 60.0, z / 60.0, 2, 99));
    return m;
}

// The blended colour (the lab).
inline Rgb mixColour(const SurfaceMix& m) {
    const Rgb ground = lerp(materialColour(m.groundA), materialColour(m.groundB), m.groundMix);
    const Rgb tree = lerp(materialColour(m.treeA), materialColour(m.treeB), m.treeMix);
    const Rgb rock = lerp(materialColour(m.rockA), materialColour(m.rockB), m.rockMix);
    Rgb c = lerp(ground, tree, m.cover);
    c = lerp(c, rock, m.rock);
    c = lerp(c, materialColour(Material::Snow), m.snow);
    c = lerp(c, materialColour(Material::Water), m.water);
    return c * (0.9 + 0.2 * m.shade);
}

// One material for a voxel, and its shade. Each choice compares a weight against a threshold: a
// spatial noise (`coherent`, 0..1) where voxels are big on screen, so neighbours agree and the ground
// has patches rather than a checkerboard, blending to `hash` (a per-voxel random) by `dither` where
// a voxel is a pixel or less and the dither is what averages to the mix.
inline void pickMaterial(const SurfaceMix& m, uint32_t hash, double coherent, double dither, Material& material,
                         int& shade) {
    auto u = [&](int k) {
        const double r = unit(hash32(hash + uint32_t(k) * 0x9e3779b9U));
        const double c = std::fmod(coherent + 0.618034 * k, 1.0);
        return mix(c, r, dither);
    };
    if (m.water > u(1)) material = Material::Water;
    else if (m.snow > u(2)) material = Material::Snow;
    else if (m.rock > u(3)) material = m.rockMix > u(4) ? m.rockB : m.rockA;
    else if (m.cover > u(5)) material = m.treeMix > u(6) ? m.treeB : m.treeA;
    else material = m.groundMix > u(7) ? m.groundB : m.groundA;
    const double s = m.shade + (u(8) - 0.5) * 0.25;
    shade = std::clamp(int(s * kShades), 0, kShades - 1);
}

// What lies under the surface voxel, seen on cliffs and cut banks.
inline Material subsurface(const SurfaceMix& m) {
    if (m.rock > 0.3 || m.snow > 0.5) return m.rockMix > 0.5 ? m.rockB : m.rockA;
    return m.groundA == Material::Tundra || m.groundB == Material::Tundra ? Material::Gneiss : Material::Soil;
}

}  // namespace mountains
