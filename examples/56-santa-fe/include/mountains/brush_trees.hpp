// Trees grown by the scene editor's tree brush (examples/60-scene-editor/brushes/trees.lua), through its
// own brush host (brush.cpp), as 50-terrain-generator's terrain_asset_baker grows its assets -- the user:
// the editor's generator is more mature and higher quality than trees.hpp's (santa-fe-reference S3).
//
// Each Santa Fe species is a few designs: the brush's parameters (the canyonlands and alpine biomes'
// designs as starting points, the crown scaled to the height) and a height in metres. A tree is grown at
// about 0.1 m a voxel -- the ground's nearest ring -- so trees, props and ground read at one scale; the
// tallest at up to ~0.14 m (the brush is capped at 256 voxels tall and 110,000 voxels a tree).
//
// Growing is a few seconds of Lua, so every grown tree is cached beside the binary (tree_cache/), keyed
// by its design and the brush file's size: a change to either regrows it.
#pragma once

#ifdef SANTA_FE_BRUSH_TREES

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "brush.h"
#include "mountains/props.hpp"
#include "mountains/trees.hpp"

namespace mountains {

struct BrushDesign {
    TreeKind kind;
    std::string name;
    double heightM;           // the reference height (trees.hpp's, times kTreeScale)
    int designHeight;         // the height the params were written for (their crown is scaled from it)
    nlohmann::json params;
};

// The voxel a tree of this height is grown at: 0.1 m, coarser only where the brush's caps demand it.
// A whole multiple of the lattice, so a tree's voxels lie on the ground's: 0.1 m up to 23 m tall, then 0.2, 0.3.
inline double brushTreeVoxel(double heightM) { return 0.1 * std::max(1.0, std::ceil(heightM / 23.0)); }

inline std::vector<BrushDesign> santaFeDesigns() {
    using J = nlohmann::json;
    std::vector<BrushDesign> d;
    const double s = kTreeScale;
    auto add = [&](TreeKind k, const std::string& n, double h, int dh, J p) { d.push_back({k, n, h, dh, std::move(p)}); };
    const J pj = {{"broadleaf", false}, {"conifer", false}};
    auto with = [](J base, J extra) { for (auto it = extra.begin(); it != extra.end(); ++it) base[it.key()] = it.value(); return base; };
    // One-seed and Rocky Mountain juniper: the canyonlands' Utah junipers, near enough.
    add(TreeKind::Juniper, "juniper_a", 3.6 * s, 45, with(pj, {{"juniper", true}, {"canopy", 22}, {"curve", 0.5}, {"dieback", 0.3}, {"seed", 11}}));
    add(TreeKind::Juniper, "juniper_c", 3.9 * s, 52, with(pj, {{"juniper", true}, {"canopy", 24}, {"curve", 0.4}, {"twist", 1.0}, {"dieback", 0.25}, {"seed", 37}}));
    add(TreeKind::Juniper, "juniper_d", 3.2 * s, 36, with(pj, {{"juniper", true}, {"canopy", 18}, {"curve", 0.9}, {"dieback", 0.2}, {"seed", 41}}));
    add(TreeKind::Juniper, "juniper_e", 3.7 * s, 48, with(pj, {{"juniper", true}, {"canopy", 25}, {"curve", 0.3}, {"dieback", 0.0}, {"seed", 53}}));
    add(TreeKind::Pinyon, "pinyon_a", 5.2 * s, 62, with(pj, {{"pinyon", true}, {"canopy", 28}, {"curve", 0.3}, {"seed", 17}}));
    add(TreeKind::Pinyon, "pinyon_b", 4.6 * s, 52, with(pj, {{"pinyon", true}, {"canopy", 24}, {"curve", 0.4}, {"seed", 29}}));
    add(TreeKind::Pinyon, "pinyon_c", 5.8 * s, 72, with(pj, {{"pinyon", true}, {"canopy", 32}, {"curve", 0.5}, {"seed", 31}}));
    add(TreeKind::Snag, "snag_a", 4.5 * s, 34, with(pj, {{"snag", true}, {"canopy", 16}, {"curve", 0.6}, {"seed", 43}}));
    add(TreeKind::Snag, "snag_b", 3.6 * s, 26, with(pj, {{"snag", true}, {"canopy", 14}, {"curve", 0.9}, {"twist", 1.0}, {"seed", 59}}));
    // Ponderosa: a tall clean trunk, an open crown of upswept clumps (the Scots pine's design, flatter in age).
    const J cf = {{"broadleaf", false}, {"conifer", true}};
    add(TreeKind::Ponderosa, "ponderosa_a", 19.0 * s, 90, with(cf, {{"canopy", 30}, {"limbAngle", 0.85}, {"reach", 0.4}, {"needles", 0.6}, {"lumpiness", 0.85}, {"curve", 0.3}, {"seed", 67}}));
    add(TreeKind::Ponderosa, "ponderosa_b", 17.0 * s, 90, with(cf, {{"canopy", 34}, {"limbAngle", 0.95}, {"reach", 0.3}, {"needles", 0.7}, {"lumpiness", 0.9}, {"curve", 0.2}, {"seed", 71}}));
    add(TreeKind::Ponderosa, "ponderosa_c", 21.0 * s, 90, with(cf, {{"canopy", 28}, {"limbAngle", 0.7}, {"reach", 0.45}, {"needles", 0.55}, {"lumpiness", 0.8}, {"curve", 0.4}, {"seed", 89}}));
    // Douglas and white fir; Engelmann spruce, bluer and narrower.
    add(TreeKind::Fir, "fir_a", 17.0 * s, 90, with(cf, {{"canopy", 20}, {"limbAngle", 0.15}, {"reach", 0.35}, {"needles", 1.0}, {"lumpiness", 0.3}, {"seed", 23}}));
    add(TreeKind::Fir, "fir_b", 15.0 * s, 90, with(cf, {{"canopy", 24}, {"limbAngle", -0.05}, {"reach", 0.5}, {"needles", 1.0}, {"lumpiness", 0.4}, {"seed", 97}}));
    add(TreeKind::Fir, "fir_c", 18.0 * s, 90, with(cf, {{"canopy", 26}, {"limbAngle", -0.25}, {"reach", 1.0}, {"needles", 0.8}, {"lumpiness", 0.6}, {"seed", 53}}));
    add(TreeKind::Spruce, "spruce_a", 20.0 * s, 90, with(cf, {{"canopy", 26}, {"limbAngle", 0.0}, {"reach", 0.6}, {"needles", 0.85}, {"seed", 11}}));
    add(TreeKind::Spruce, "spruce_b", 18.0 * s, 90, with(cf, {{"canopy", 22}, {"limbAngle", -0.4}, {"reach", 0.9}, {"needles", 0.9}, {"seed", 37}}));
    add(TreeKind::Spruce, "spruce_c", 21.0 * s, 90, with(cf, {{"canopy", 18}, {"limbAngle", -1.0}, {"reach", 0.9}, {"needles", 0.95}, {"seed", 41}}));
    // Aspen: slender white trunks, small round crowns; gold in October (recoloured from the autumn ramp).
    const J bl = {{"broadleaf", true}, {"conifer", false}};
    const std::string season = november() ? "Summer" : "Autumn";
    add(TreeKind::Aspen, "aspen_a", 12.0 * s, 90, with(bl, {{"crownShape", "Round"}, {"canopy", 18}, {"branching", 3}, {"leafSize", 0.7}, {"trunk", 2}, {"curve", 0.5}, {"reach", 0.9}, {"season", season}, {"seed", 43}}));
    add(TreeKind::Aspen, "aspen_b", 11.0 * s, 90, with(bl, {{"crownShape", "Columnar"}, {"canopy", 14}, {"branching", 3}, {"leafSize", 0.75}, {"trunk", 2}, {"curve", 0.4}, {"season", season}, {"seed", 29}}));
    add(TreeKind::Aspen, "aspen_c", 13.0 * s, 90, with(bl, {{"crownShape", "Round"}, {"canopy", 16}, {"branching", 2}, {"leafSize", 0.7}, {"trunk", 2}, {"curve", 0.7}, {"season", season}, {"seed", 61}}));
    // Gambel oak: low, spreading thickets, rust in October.
    add(TreeKind::Oak, "oak_a", 2.8 * kGameScale, 90, with(bl, {{"crownShape", "Spreading"}, {"canopy", 36}, {"branching", 2}, {"season", season}, {"seed", 59}}));
    add(TreeKind::Oak, "oak_b", 3.2 * kGameScale, 90, with(bl, {{"crownShape", "Spreading"}, {"canopy", 40}, {"branching", 1}, {"leafSize", 1.1}, {"season", season}, {"seed", 13}}));
    return d;
}

// Boulder formations from the editor's Boulders brush (boulders.lua): Santa Fe's rocks -- pink granite tors
// and erratics on the cores, gneiss, basalt columns and talus on the mesa scarps, sandstone in the basin. The
// largest grown at 0.2-0.3 m a voxel (whole multiples of the lattice: the brush caps a placement at 110,000
// voxels and 64 tall), so a tor stands ~6 m to the player. `family`: 0 granite, 1 gneiss, 2 basalt, 3 sandstone, 4 erratic (anywhere).
struct BoulderDesign { int family; double voxel; BrushDesign design; };

inline std::vector<BoulderDesign> santaFeBoulders() {
    using J = nlohmann::json;
    std::vector<BoulderDesign> d;
    const J none = {{"rounded", false}, {"angular", false}, {"slab", false}, {"split", false}, {"tor", false}, {"columns", false}};
    auto add = [&](int family, double voxel, const std::string& name, J extra) {
        J p = none;
        p["force"] = true;
        for (auto it = extra.begin(); it != extra.end(); ++it) p[it.key()] = it.value();
        d.push_back({family, voxel, {TreeKind::Juniper, name, 0.0, 1, p}});
    };
    for (int seed : {3, 17}) {
        const std::string s = "_" + std::to_string(seed);
        add(0, 0.3, "granite_tor_pile" + s, {{"tor", true}, {"rock", "Granite"}, {"height", 40}, {"width", 1.3}, {"group", 3}, {"layout", "Pile"}, {"spread", 0.9},
                                              {"rubble", 0.6}, {"weathering", 1.4}, {"moss", 0.12}, {"mossDry", 0.8}, {"lichen", 0.7}, {"embed", 0.12}, {"seed", seed}});
        add(0, 0.2, "granite_tor" + s, {{"tor", true}, {"rock", "Granite"}, {"height", 64}, {"width", 1.1}, {"group", 1}, {"rubble", 0.4},
                                         {"weathering", 1.2}, {"moss", 0.1}, {"mossDry", 0.8}, {"lichen", 0.6}, {"seed", seed + 1}});
        add(0, 0.2, "granite_cluster" + s, {{"rounded", true}, {"angular", true}, {"rock", "Granite"}, {"height", 34}, {"width", 1.3}, {"group", 6},
                                             {"layout", "Cluster"}, {"spread", 1.2}, {"sizeVariety", 0.8}, {"rubble", 0.6}, {"moss", 0.15}, {"mossDry", 0.7}, {"lichen", 0.5}, {"seed", seed + 2}});
        add(0, 0.2, "granite_split" + s, {{"split", true}, {"rock", "Granite"}, {"height", 40}, {"width", 1.5}, {"group", 1}, {"rubble", 0.5}, {"lichen", 0.6}, {"seed", seed + 3}});
        add(1, 0.2, "gneiss_train" + s, {{"angular", true}, {"rock", "Gneiss"}, {"height", 32}, {"width", 1.4}, {"group", 5}, {"layout", "Line"}, {"spread", 1.4},
                                          {"sizeVariety", 0.7}, {"rubble", 0.7}, {"lichen", 0.5}, {"seed", seed + 4}});
        add(2, 0.3, "basalt_columns" + s, {{"columns", true}, {"rock", "Basalt"}, {"height", 42}, {"width", 1.4}, {"group", 1}, {"rubble", 0.8}, {"lichen", 0.4}, {"seed", seed + 5}});
        add(2, 0.1, "basalt_talus" + s, {{"angular", true}, {"rock", "Basalt"}, {"height", 34}, {"width", 1.2}, {"group", 9}, {"layout", "Pile"}, {"spread", 1.0},
                                          {"sizeVariety", 0.8}, {"rubble", 1.0}, {"lichen", 0.4}, {"seed", seed + 6}});
        add(3, 0.1, "sandstone_slabs" + s, {{"slab", true}, {"rock", "Sandstone"}, {"height", 26}, {"width", 2.5}, {"group", 3}, {"layout", "Pile"}, {"spread", 1.1},
                                             {"rubble", 0.5}, {"weathering", 1.5}, {"seed", seed + 7}});
        add(3, 0.2, "sandstone_split" + s, {{"split", true}, {"rounded", true}, {"rock", "Sandstone"}, {"height", 34}, {"width", 1.6}, {"group", 2}, {"weathering", 1.6}, {"seed", seed + 8}});
        add(4, 0.3, "erratic" + s, {{"rounded", true}, {"rock", "Granite"}, {"height", 42}, {"width", 1.8}, {"group", 1}, {"rubble", 0.3}, {"moss", 0.2}, {"mossDry", 0.8},
                                     {"lichen", 0.7}, {"seed", seed + 9}});
    }
    return d;
}

// Grows placements with one of the scene editor's scatter brushes (`trees` by default; `boulders` too).
class TreeGrower {
public:
    explicit TreeGrower(std::string brush = "trees") : brushName_(std::move(brush)) {}
    bool load(std::string& error) {
        projv::editor::brushLibraryLoad(library_, SANTA_FE_BRUSHES_DIR, false);
        brush_ = library_.find(brushName_);
        if (!brush_ || !brush_->loadError.empty()) {
            error = brush_ ? brush_->loadError : "no trees brush in " SANTA_FE_BRUSHES_DIR;
            return false;
        }
        projv::editor::brushExpandMaterials(*brush_, slots_);
        std::error_code ec;
        brushSize_ = std::filesystem::file_size(std::string(SANTA_FE_BRUSHES_DIR) + "/" + brushName_ + ".lua", ec);
        return true;
    }
    const std::vector<projv::editor::BrushMaterialSlot>& slots() const { return slots_; }

    // Grows `d` (from the cache if it is there), as voxels relative to the trunk's foot: dy >= 0 above
    // the ground. False if the brush planted nothing.
    // `voxel` > 0: grow at that voxel, its height in voxels the design's height parameter as given (no
    // regrowing to a target: the boulders).
    bool grow(const BrushDesign& d, TreeVoxels& out, std::string& error, double fixedVoxel = 0.0) {
        const double voxel = fixedVoxel > 0.0 ? fixedVoxel : brushTreeVoxel(d.heightM);
        const int target = fixedVoxel > 0.0 ? d.params.value("height", 14) : int(std::lround(d.heightM / voxel));
        nlohmann::json params = d.params;
        if (fixedVoxel <= 0.0 && params.contains("canopy"))
            params["canopy"] = std::clamp(int(std::lround(params["canopy"].get<double>() * double(target) / double(d.designHeight))), 1, 96);
        const std::string key = brushName_ + "|" + params.dump() + "|" + std::to_string(target) + "|" + std::to_string(brushSize_);
        const std::string cache = "tree_cache/" + d.name + "_" + std::to_string(std::hash<std::string>{}(key)) + ".bin";
        out = TreeVoxels{};
        out.kind = d.kind;
        out.voxel = voxel;
        if (readCache(cache, out)) return true;

        projv::editor::brushResetParams(*brush_);
        for (auto it = params.begin(); it != params.end(); ++it) setParam(it.key(), it.value());
        double height = double(target) * 1.15;
        std::vector<projv::editor::BrushPlacementVoxel> voxels;
        int grown = 0;
        bool ok = false;
        if (fixedVoxel > 0.0) height = double(target);
        for (int pass = 0; pass < (fixedVoxel > 0.0 ? 1 : 5); pass++) {
            setParam("height", height);
            voxels.clear();
            if (!plant(voxels, error)) break;
            ok = true;
            grown = 0;
            for (const auto& v : voxels) grown = std::max(grown, v.dy);
            if (std::abs(grown - target) <= 3) break;
            height = std::clamp(height * double(target) / double(std::max(1, grown)), 2.0, 256.0);
        }
        if (!ok) return false;
        out.height = grown;
        for (const auto& v : voxels) {
            out.pos.push_back({int16_t(v.dx), int16_t(v.dy - 1), int16_t(v.dz)});   // dy 1 is the first voxel above ground
            out.slot.push_back(uint8_t(std::clamp(v.materialIndex, 0, 255)));
        }
        writeCache(cache, out);
        return true;
    }

private:
    bool setParam(const std::string& name, const nlohmann::json& value) {
        using projv::editor::BrushParamType;
        for (size_t i = 0; i < brush_->params.size() && i < brush_->values.size(); i++) {
            if (brush_->params[i].name != name) continue;
            switch (brush_->params[i].type) {
                case BrushParamType::Bool: brush_->values[i].boolean = value.is_boolean() ? value.get<bool>() : value.get<double>() != 0.0; return true;
                case BrushParamType::Enum:
                case BrushParamType::Text:
                case BrushParamType::Asset: brush_->values[i].text = value.get<std::string>(); return true;
                default: brush_->values[i].number = value.get<double>(); return true;
            }
        }
        return false;
    }
    // One tree, planted with Force on flat ground (solid below y = 1), as the baker plants it.
    bool plant(std::vector<projv::editor::BrushPlacementVoxel>& out, std::string& error) {
        projv::editor::BrushInvocation invocation = projv::editor::brushBeginStroke(*brush_, error);
        if (!invocation.valid()) return false;
        projv::editor::brushSetSolidQuery(*brush_, [](int32_t, int32_t y, int32_t) { return y <= 0; });
        projv::editor::BrushContext context;
        context.solid = true;
        context.ny = 1.0f;
        context.crevice = 0.6f;
        context.materialName = "ground";
        context.force = true;
        std::string reason;
        const bool planted = projv::editor::brushEvaluateScatter(invocation, context, out, &reason);
        projv::editor::brushSetSolidQuery(*brush_, projv::editor::BrushSolidQuery());
        if (!planted) error = invocation.failed() ? invocation.error() : "the brush planted nothing: " + reason;
        return planted;
    }
    static bool readCache(const std::string& path, TreeVoxels& t) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        uint32_t n = 0;
        int32_t h = 0;
        f.read(reinterpret_cast<char*>(&n), 4);
        f.read(reinterpret_cast<char*>(&h), 4);
        if (!f || n > 2000000u) return false;
        t.height = h;
        t.pos.resize(n);
        t.slot.resize(n);
        f.read(reinterpret_cast<char*>(t.pos.data()), std::streamsize(n * sizeof(t.pos[0])));
        f.read(reinterpret_cast<char*>(t.slot.data()), std::streamsize(n));
        return bool(f);
    }
    static void writeCache(const std::string& path, const TreeVoxels& t) {
        std::error_code ec;
        std::filesystem::create_directories("tree_cache", ec);
        std::ofstream f(path, std::ios::binary);
        const uint32_t n = uint32_t(t.pos.size());
        const int32_t h = t.height;
        f.write(reinterpret_cast<const char*>(&n), 4);
        f.write(reinterpret_cast<const char*>(&h), 4);
        f.write(reinterpret_cast<const char*>(t.pos.data()), std::streamsize(n * sizeof(t.pos[0])));
        f.write(reinterpret_cast<const char*>(t.slot.data()), std::streamsize(n));
    }

    std::string brushName_;
    projv::editor::BrushLibrary library_;
    projv::editor::BrushDefinition* brush_ = nullptr;
    std::vector<projv::editor::BrushMaterialSlot> slots_;
    uintmax_t brushSize_ = 0;
};

}  // namespace mountains

#endif
