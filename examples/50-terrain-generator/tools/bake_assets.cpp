// Bakes a biome's assets -- trees, shrubs, boulders -- with the scene editor's brushes.
//
//   terrain_asset_baker [biome]               (default: canyonlands)
//   terrain_asset_baker <designs.json> [output-folder]
//
// A biome is a folder, biomes/<biome>/, holding designs.json; the assets are written beside it, in
// biomes/<biome>/assets/. This holds no shape logic of its own. Each design names a brush in
// examples/60-scene-editor/brushes/ (`brush`, default "trees") and that brush's parameters; it is
// grown through the editor's own brush host (brush.cpp) -- so an asset here is exactly what the
// editor's Custom tool plants with Force on -- and written as a current-format compose asset:
//
//   assets/<name>/compose.json   one `data` component, `model`, with its palette
//   assets/<name>/model.data     the voxels, as a Grid of 16^3 blocks
//   assets/assets.json           the list the terrain generator reads: name, kind, climate band,
//                                abundance, size
//
// A design's `height` is only a first guess. The tree brush gives every tree 72-100% of it, so the
// driver regrows until the asset comes out within a few voxels of its `targetHeight` (the design's,
// else the file's).
//
// Coordinates: the trunk stands at (128, 0, 128). The terrain's loader keeps voxels in 0..255 per
// axis (tree_placement.hpp), so anything wider than that is reported and dropped.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "brush.h"
#include "core/log.h"
#include "data_structures/scene.h"
#include "utils/compose_io.h"
#include "utils/editing.h"
#include "utils/material.h"
#include "utils/scene_query.h"

namespace {

using projv::editor::BrushDefinition;
using projv::editor::BrushParamType;

constexpr int kOrigin = 128;   // where the trunk stands, in the asset's voxel space

// Sets one parameter by name from a JSON value, whatever its declared type.
bool setParam(BrushDefinition& brush, const std::string& name, const nlohmann::json& value) {
    for (size_t i = 0; i < brush.params.size() && i < brush.values.size(); i++) {
        if (brush.params[i].name != name) continue;
        switch (brush.params[i].type) {
            case BrushParamType::Bool:
                brush.values[i].boolean = value.is_boolean() ? value.get<bool>() : value.get<double>() != 0.0;
                return true;
            case BrushParamType::Enum:
            case BrushParamType::Text:
            case BrushParamType::Asset:
                brush.values[i].text = value.get<std::string>();
                return true;
            default:
                brush.values[i].number = value.get<double>();
                return true;
        }
    }
    return false;
}

struct Grown {
    std::vector<projv::editor::BrushPlacementVoxel> voxels;
    int height = 0, minX = 0, maxX = 0, minZ = 0, maxZ = 0;
};

// One tree, planted with Force on flat ground: the site is the ground voxel at the origin, solid
// below y = 1 and empty above.
bool grow(BrushDefinition& brush, Grown& out, std::string& error) {
    projv::editor::BrushInvocation invocation = projv::editor::brushBeginStroke(brush, error);
    if (!invocation.valid()) return false;
    projv::editor::brushSetSolidQuery(brush, [](int32_t, int32_t y, int32_t) { return y <= 0; });

    projv::editor::BrushContext context;
    context.solid = true;
    context.ny = 1.0f;
    context.crevice = 0.6f;          // what open, flat ground reads as
    context.materialName = "ground";
    context.force = true;
    std::string reason;
    bool planted = projv::editor::brushEvaluateScatter(invocation, context, out.voxels, &reason);
    projv::editor::brushSetSolidQuery(brush, projv::editor::BrushSolidQuery());
    if (!planted) {
        error = invocation.failed() ? invocation.error() : "the brush planted nothing: " + reason;
        return false;
    }
    out.height = 0;
    out.minX = out.minZ = std::numeric_limits<int>::max();
    out.maxX = out.maxZ = std::numeric_limits<int>::min();
    for (const auto& v : out.voxels) {
        out.height = std::max(out.height, v.dy);
        out.minX = std::min(out.minX, v.dx); out.maxX = std::max(out.maxX, v.dx);
        out.minZ = std::min(out.minZ, v.dz); out.maxZ = std::max(out.maxZ, v.dz);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    // A bare word is a biome under biomes/; anything ending .json is a designs file.
    std::string first = argc > 1 ? argv[1] : "canyonlands";
    const bool isFile = first.size() > 5 && first.substr(first.size() - 5) == ".json";
    const fs::path designPath = isFile ? fs::path(first) : fs::path(ASSET_BIOMES_DIR) / first / "designs.json";
    const fs::path output = isFile ? (argc > 2 ? fs::path(argv[2]) : designPath.parent_path() / "assets")
                                   : designPath.parent_path() / "assets";

    std::ifstream designFile(designPath);
    if (!designFile) {
        projv::core::error("bake_assets: cannot read {}", designPath.string());
        return 1;
    }
    nlohmann::json designs = nlohmann::json::parse(designFile, nullptr, false);
    if (designs.is_discarded()) {
        projv::core::error("bake_assets: {} is not valid JSON", designPath.string());
        return 1;
    }
    const int fileTargetHeight = designs.value("targetHeight", 90);
    const float voxelScale = designs.value("voxelScale", 1.0f);

    projv::editor::BrushLibrary library;
    projv::editor::brushLibraryLoad(library, ASSET_BRUSHES_DIR, false);

    fs::create_directories(output);
    nlohmann::json manifest;
    manifest["comment"] = "Written by terrain_asset_baker from designs.json beside this folder. The "
                          "terrain generator reads the assets, their kinds and climate bands from here.";
    manifest["treeVoxelWorldSize"] = designs.value("treeVoxelWorldSize", 1.75);
    manifest["species"] = nlohmann::json::array();

    int failures = 0;
    for (const nlohmann::json& design : designs["designs"]) {
        const std::string name = design.at("name").get<std::string>();
        const std::string brushName = design.value("brush", std::string("trees"));
        BrushDefinition* brush = library.find(brushName);
        if (!brush || !brush->loadError.empty()) {
            projv::core::error("bake_assets: {}: no usable '{}' brush in {}: {}", name, brushName, ASSET_BRUSHES_DIR,
                               brush ? brush->loadError : std::string("not found"));
            failures++;
            continue;
        }
        std::vector<projv::editor::BrushMaterialSlot> slots;
        projv::editor::brushExpandMaterials(*brush, slots);
        const int targetHeight = design.value("targetHeight", fileTargetHeight);
        projv::editor::brushResetParams(*brush);
        for (auto it = design["params"].begin(); it != design["params"].end(); ++it) {
            if (!setParam(*brush, it.key(), it.value())) {
                projv::core::warn("bake_assets: {}: the brush has no parameter '{}'", name, it.key());
            }
        }

        // Regrow until the height lands near the target. The brush scales everything else from it,
        // so this is one control to converge, and two or three passes always do.
        double height = design["params"].value("height", double(targetHeight) * 1.15);
        Grown tree;
        std::string error;
        bool ok = false;
        for (int pass = 0; pass < 5; pass++) {
            setParam(*brush, "height", height);
            if (!grow(*brush, tree, error)) break;
            ok = true;
            if (std::abs(tree.height - targetHeight) <= 3) break;
            height = std::clamp(height * double(targetHeight) / double(std::max(1, tree.height)), 2.0, 256.0);
        }
        if (!ok) {
            projv::core::error("bake_assets: {}: {}", name, error);
            failures++;
            continue;
        }

        // The asset: one Asset node holding one data component, `model`, whose palette is the
        // brush's expanded material list -- named, so an editor brush that grows through tree
        // foliage (`displaces`) still recognises it once this is placed in a scene.
        projv::Scene scene;
        projv::ComponentHandle root = projv::utils::addComponent(scene, projv::ComponentKind::Asset, name,
                                                                 projv::INVALID_COMPONENT_HANDLE, 16, voxelScale);
        projv::ComponentHandle model = projv::utils::addComponent(scene, projv::ComponentKind::Chunk, "model",
                                                                  root, 16, voxelScale);
        if (root == projv::INVALID_COMPONENT_HANDLE || model == projv::INVALID_COMPONENT_HANDLE) {
            projv::core::error("bake_assets: {}: could not create the components", name);
            failures++;
            continue;
        }
        std::vector<uint8_t> slotOf(slots.size(), 0);
        for (size_t i = 0; i < slots.size(); i++) {
            const projv::editor::BrushMaterialSlot& slot = slots[i];
            slotOf[i] = projv::utils::internMaterial(scene, scene.components[model], slot.name,
                                                     projv::packRGB10(slot.color[0], slot.color[1], slot.color[2]));
        }

        std::vector<projv::PendingVoxelOp> ops;
        ops.reserve(tree.voxels.size());
        size_t clipped = 0;
        for (const auto& v : tree.voxels) {
            // dy >= 1 is the brush's "above the ground"; the asset's ground line is y = 0.
            projv::core::ivec3 p(kOrigin + v.dx, v.dy - 1, kOrigin + v.dz);
            if (p.x < 0 || p.y < 0 || p.z < 0 || p.x > 255 || p.y > 255 || p.z > 255) { clipped++; continue; }
            if (size_t(v.materialIndex) >= slotOf.size()) continue;
            projv::PendingVoxelOp op{};
            op.isAdd = true;
            op.position = p;
            op.slot = slotOf[size_t(v.materialIndex)];
            ops.push_back(op);
        }
        projv::utils::queueVoxelAdd(scene, model, ops);
        projv::utils::updateScene(scene);

        const fs::path folder = output / name;
        fs::remove_all(folder);   // a stale .data from an earlier layout would otherwise linger
        if (!projv::utils::saveComposeToDisk(scene, root, folder.string())) {
            projv::core::error("bake_assets: {}: could not write {}", name, folder.string());
            failures++;
            continue;
        }

        // Read it back the way anything else will, and count what came out.
        projv::Scene check = projv::utils::loadComposeFromDisk(folder.string() + "/");
        uint64_t stored = 0;
        for (projv::ComponentHandle h = 0; h < check.components.size(); h++) {
            if (check.components[h].kind != projv::ComponentKind::Asset)
                stored += projv::utils::getComponentVoxelCount(check, h);
        }

        const nlohmann::json climate = design.value("climate", nlohmann::json::object());
        nlohmann::json species;
        species["name"] = name;
        species["label"] = design.value("label", name);
        species["kind"] = design.value("kind", "broadleaf");
        species["temp"] = climate.value("temp", nlohmann::json::array({0.5, 0.15}));
        species["humid"] = climate.value("humid", nlohmann::json::array({0.5, 0.18}));
        species["abundance"] = climate.value("abundance", 1.0);
        species["height"] = tree.height;
        species["width"] = std::max(tree.maxX - tree.minX, tree.maxZ - tree.minZ) + 1;
        species["voxels"] = stored;
        manifest["species"].push_back(species);

        projv::core::info("bake_assets: {:<16} {:3d} tall, {:3d} wide, {:6} voxels{}", name, tree.height,
                          int(species["width"]), stored,
                          clipped ? " (" + std::to_string(clipped) + " outside 0..255 dropped)" : std::string());
        if (stored != ops.size()) {
            projv::core::warn("bake_assets: {}: wrote {} voxels, read back {}", name, ops.size(), stored);
        }
    }

    std::ofstream manifestFile(output / "assets.json");
    manifestFile << manifest.dump(2) << "\n";
    projv::core::info("bake_assets: {} asset(s) in {}{}", manifest["species"].size(), output.string(),
                      failures ? ", " + std::to_string(failures) + " failed" : std::string());
    return failures == 0 ? 0 : 1;
}
