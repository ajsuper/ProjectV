#ifndef PROJECTV_MINECRAFT_BLOCKS_HPP
#define PROJECTV_MINECRAFT_BLOCKS_HPP

// Block name to color mapping for Minecraft worlds.
//
// A world file stores block *identities* ("minecraft:oak_stairs"), not colors — the colors live in
// the game's textures, which are not part of the save. Something has to bridge that gap, and this
// header is it.
//
// Three tiers, in order:
//
//   1. A resource pack, if one is supplied. Textures are averaged over their opaque pixels, which
//      is exact for the pack in use and is the only tier that can color modded blocks.
//   2. An explicit table of the ~200 blocks that actually cover most of a world by volume — terrain,
//      wood, stone, ore, the common building materials.
//   3. Rules, which are what make the long tail tractable. Minecraft's naming is highly regular:
//      the 16 dye colors prefix a dozen block families, and stairs/slabs/walls/fences are always
//      derived from a base material. Stripping a known suffix and re-looking-up handles hundreds of
//      blocks that no table would be worth enumerating.
//
// Anything that still misses is reported by name and drawn in neutral gray, so an unmapped block
// shows up as a hole in the color scheme rather than a hole in the build.

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math.h"
#include "utils/material.h"

namespace minecraft {

struct NamedColor {
    const char* name;
    uint8_t r, g, b;
};

// Blocks with no volume worth voxelizing: air of every kind, plus the technical blocks that are
// invisible in game and would otherwise fill a build with solid boxes.
inline bool isSkippedBlock(const std::string& name) {
    static const std::vector<std::string> SKIPPED = {
        "air", "cave_air", "void_air", "barrier", "light", "structure_void", "moving_piston"
    };
    return std::find(SKIPPED.begin(), SKIPPED.end(), name) != SKIPPED.end();
}

// The 16 dye colors, as they appear on wool and concrete — the saturated end of the range. Other
// families derive from these (terracotta is muted, stained glass is lightened).
inline const std::vector<NamedColor>& dyeColors() {
    static const std::vector<NamedColor> COLORS = {
        {"white", 233, 236, 236},     {"orange", 240, 118, 19},   {"magenta", 189, 68, 179},
        {"light_blue", 58, 175, 217}, {"yellow", 248, 198, 39},   {"lime", 112, 185, 25},
        {"pink", 237, 141, 172},      {"gray", 62, 68, 71},       {"light_gray", 142, 142, 134},
        {"cyan", 21, 137, 145},       {"purple", 121, 42, 172},   {"blue", 53, 57, 157},
        {"brown", 114, 71, 40},       {"green", 84, 109, 27},     {"red", 176, 46, 38},
        {"black", 20, 21, 25}
    };
    return COLORS;
}

inline const std::unordered_map<std::string, projv::Color>& blockColorTable() {
    static const std::unordered_map<std::string, projv::Color> TABLE = [] {
        static const NamedColor ENTRIES[] = {
            // Terrain
            {"stone", 125, 125, 125},              {"granite", 149, 103, 86},
            {"diorite", 207, 207, 209},            {"andesite", 136, 136, 136},
            {"deepslate", 80, 80, 82},             {"tuff", 108, 109, 102},
            {"calcite", 223, 222, 216},            {"dripstone_block", 134, 107, 92},
            {"cobblestone", 127, 127, 127},        {"mossy_cobblestone", 110, 118, 90},
            {"dirt", 134, 96, 67},                 {"coarse_dirt", 119, 85, 59},
            {"rooted_dirt", 144, 103, 76},         {"podzol", 91, 64, 29},
            {"mycelium", 111, 98, 97},             {"grass_block", 106, 141, 64},
            {"farmland", 122, 84, 55},             {"mud", 60, 49, 41},
            {"clay", 160, 166, 179},               {"gravel", 131, 127, 126},
            {"sand", 219, 207, 163},               {"red_sand", 190, 102, 33},
            {"sandstone", 216, 203, 155},          {"red_sandstone", 186, 99, 29},
            {"bedrock", 85, 85, 85},               {"obsidian", 21, 18, 30},
            {"crying_obsidian", 32, 10, 60},       {"magma_block", 142, 63, 31},
            {"snow", 249, 254, 254},               {"snow_block", 249, 254, 254},
            {"powder_snow", 248, 253, 253},        {"ice", 145, 183, 253},
            {"packed_ice", 141, 180, 250},         {"blue_ice", 116, 167, 253},
            {"water", 63, 118, 228},               {"lava", 217, 110, 26},
            {"moss_block", 89, 109, 45},           {"sculk", 12, 32, 38},
            {"smooth_stone", 158, 158, 158},       {"basalt", 73, 72, 80},
            {"smooth_basalt", 72, 72, 80},         {"blackstone", 42, 35, 40},
            {"amethyst_block", 133, 97, 191},      {"budding_amethyst", 133, 97, 191},

            // Ores
            {"coal_ore", 111, 111, 111},           {"iron_ore", 136, 130, 127},
            {"copper_ore", 124, 127, 109},         {"gold_ore", 145, 133, 106},
            {"redstone_ore", 133, 107, 107},       {"emerald_ore", 108, 136, 115},
            {"lapis_ore", 107, 117, 141},          {"diamond_ore", 129, 140, 143},
            {"nether_quartz_ore", 117, 65, 60},    {"nether_gold_ore", 119, 46, 39},
            {"ancient_debris", 94, 61, 55},

            // Metal and mineral blocks
            {"iron_block", 220, 220, 220},         {"gold_block", 246, 208, 61},
            {"diamond_block", 98, 237, 228},       {"emerald_block", 42, 203, 86},
            {"lapis_block", 30, 67, 140},          {"redstone_block", 175, 24, 5},
            {"coal_block", 16, 16, 16},            {"copper_block", 192, 107, 79},
            {"exposed_copper", 161, 125, 103},     {"weathered_copper", 108, 153, 118},
            {"oxidized_copper", 82, 162, 132},     {"netherite_block", 66, 60, 63},
            {"raw_iron_block", 166, 135, 107},     {"raw_copper_block", 154, 105, 79},
            {"raw_gold_block", 221, 169, 46},      {"quartz_block", 235, 229, 222},
            {"amethyst_cluster", 174, 129, 216},

            // Wood — logs, then planks. Stairs/slabs/fences resolve to planks by rule.
            {"oak_log", 109, 85, 50},              {"spruce_log", 58, 37, 16},
            {"birch_log", 216, 215, 210},          {"jungle_log", 85, 67, 25},
            {"acacia_log", 103, 96, 86},           {"dark_oak_log", 60, 46, 26},
            {"mangrove_log", 84, 49, 46},          {"cherry_log", 87, 53, 62},
            {"crimson_stem", 92, 25, 29},          {"warped_stem", 43, 104, 99},
            {"bamboo_block", 132, 141, 39},
            {"oak_planks", 162, 131, 79},          {"spruce_planks", 114, 84, 48},
            {"birch_planks", 196, 179, 123},       {"jungle_planks", 160, 115, 80},
            {"acacia_planks", 168, 90, 50},        {"dark_oak_planks", 66, 43, 20},
            {"mangrove_planks", 117, 54, 48},      {"cherry_planks", 226, 177, 164},
            {"crimson_planks", 101, 48, 70},       {"warped_planks", 43, 104, 99},
            {"bamboo_planks", 193, 178, 83},

            // Foliage
            {"oak_leaves", 62, 142, 35},           {"spruce_leaves", 55, 90, 55},
            {"birch_leaves", 128, 167, 85},        {"jungle_leaves", 65, 150, 30},
            {"acacia_leaves", 90, 150, 40},        {"dark_oak_leaves", 60, 130, 30},
            {"mangrove_leaves", 60, 140, 45},      {"cherry_leaves", 226, 160, 190},
            {"azalea_leaves", 90, 140, 55},        {"flowering_azalea_leaves", 120, 140, 70},
            {"nether_wart_block", 114, 2, 2},      {"warped_wart_block", 20, 100, 96},
            {"grass", 88, 130, 53},                {"tall_grass", 88, 130, 53},
            {"fern", 88, 130, 53},                 {"large_fern", 88, 130, 53},
            {"vine", 47, 100, 20},                 {"lily_pad", 32, 128, 48},
            {"cactus", 85, 127, 42},               {"bamboo", 132, 141, 39},
            {"sugar_cane", 148, 192, 101},         {"seagrass", 60, 130, 45},
            {"kelp", 70, 125, 40},                 {"moss_carpet", 89, 109, 45},
            {"melon", 111, 145, 26},               {"pumpkin", 196, 116, 22},
            {"carved_pumpkin", 196, 116, 22},      {"jack_o_lantern", 213, 154, 60},
            {"hay_block", 165, 139, 12},           {"dried_kelp_block", 50, 58, 39},
            {"sponge", 195, 192, 74},              {"wet_sponge", 173, 179, 60},
            {"azalea", 100, 145, 60},              {"flowering_azalea", 120, 145, 75},
            {"mushroom_stem", 203, 196, 185},      {"brown_mushroom_block", 141, 106, 83},
            {"red_mushroom_block", 202, 57, 55},

            // Building blocks
            {"bricks", 150, 97, 83},               {"stone_bricks", 122, 122, 122},
            {"mossy_stone_bricks", 115, 121, 105}, {"cracked_stone_bricks", 118, 117, 117},
            {"chiseled_stone_bricks", 118, 117, 117},
            {"deepslate_bricks", 71, 71, 74},      {"deepslate_tiles", 54, 54, 57},
            {"polished_deepslate", 84, 84, 86},    {"cobbled_deepslate", 77, 77, 80},
            {"nether_bricks", 44, 22, 26},         {"red_nether_bricks", 70, 7, 9},
            {"end_stone", 219, 222, 158},          {"end_stone_bricks", 218, 224, 162},
            {"purpur_block", 169, 125, 169},       {"purpur_pillar", 171, 128, 171},
            {"prismarine", 99, 156, 151},          {"prismarine_bricks", 99, 171, 158},
            {"dark_prismarine", 51, 91, 75},       {"sea_lantern", 172, 199, 190},
            {"glowstone", 249, 213, 145},          {"shroomlight", 240, 146, 70},
            {"netherrack", 97, 38, 38},            {"soul_sand", 81, 62, 50},
            {"soul_soil", 74, 56, 44},             {"crimson_nylium", 130, 31, 31},
            {"warped_nylium", 43, 114, 101},       {"bone_block", 229, 225, 205},
            {"terracotta", 152, 94, 67},           {"glass", 200, 225, 231},
            {"tinted_glass", 44, 40, 47},          {"glass_pane", 200, 225, 231},
            {"iron_bars", 137, 137, 137},          {"cobweb", 220, 220, 220},
            {"scaffolding", 172, 133, 79},         {"ladder", 154, 127, 74},
            {"chest", 162, 130, 78},               {"trapped_chest", 162, 130, 78},
            {"barrel", 118, 90, 51},               {"crafting_table", 154, 111, 62},
            {"bookshelf", 154, 125, 77},           {"furnace", 124, 124, 124},
            {"blast_furnace", 92, 92, 93},         {"smoker", 87, 78, 71},
            {"anvil", 68, 68, 68},                 {"cauldron", 73, 73, 73},
            {"enchanting_table", 78, 47, 56},      {"jukebox", 91, 62, 44},
            {"note_block", 88, 60, 42},            {"tnt", 178, 61, 41},
            {"sea_pickle", 91, 108, 44},           {"beacon", 117, 219, 212},
            {"lodestone", 128, 129, 132},          {"respawn_anchor", 62, 30, 88},
            {"target", 224, 199, 191},             {"observer", 98, 98, 98},
            {"piston", 124, 118, 100},             {"sticky_piston", 116, 126, 92},
            {"dispenser", 124, 124, 124},          {"dropper", 124, 124, 124},
            {"hopper", 70, 70, 70},                {"rail", 140, 132, 127},
            {"powered_rail", 140, 118, 82},        {"redstone_lamp", 95, 59, 34},
            {"torch", 255, 215, 100},              {"wall_torch", 255, 215, 100},
            {"lantern", 220, 160, 80},             {"soul_lantern", 100, 190, 200},
            {"campfire", 190, 120, 50},            {"end_rod", 226, 224, 213},
            {"slime_block", 111, 192, 91},         {"honey_block", 251, 179, 53},
            {"honeycomb_block", 229, 148, 29},     {"dirt_path", 148, 121, 65},
            {"composter", 121, 87, 47},            {"lectern", 156, 122, 70},
            {"loom", 131, 106, 74},                {"smithing_table", 55, 51, 57},
            {"cartography_table", 106, 90, 71},    {"fletching_table", 197, 180, 121},
            {"grindstone", 142, 142, 142},         {"stonecutter", 122, 122, 122},
            {"bell", 226, 184, 76},                {"chain", 51, 55, 65},
            {"spawner", 26, 40, 52},               {"infested_stone", 125, 125, 125},
            {"soul_fire", 60, 190, 200},           {"fire", 220, 130, 40},
            {"nether_portal", 92, 34, 165},        {"end_portal_frame", 78, 106, 88},
            {"dragon_egg", 12, 9, 15},             {"conduit", 148, 126, 100}
        };

        std::unordered_map<std::string, projv::Color> table;
        table.reserve(sizeof(ENTRIES) / sizeof(ENTRIES[0]) * 2);
        for (const NamedColor& entry : ENTRIES) {
            table.emplace(entry.name, projv::Color{entry.r, entry.g, entry.b});
        }
        return table;
    }();
    return TABLE;
}

namespace detail {

inline bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline projv::Color scaleColor(projv::Color color, float factor) {
    auto scale = [factor](uint8_t channel) {
        return uint8_t(std::min(255.0f, std::max(0.0f, float(channel) * factor)));
    };
    return {scale(color.r), scale(color.g), scale(color.b)};
}

inline projv::Color blendToward(projv::Color color, projv::Color target, float amount) {
    auto blend = [amount](uint8_t from, uint8_t to) {
        return uint8_t(float(from) * (1.0f - amount) + float(to) * amount);
    };
    return {blend(color.r, target.r), blend(color.g, target.g), blend(color.b, target.b)};
}

// Families whose color is entirely determined by their dye prefix. The multiplier reproduces how
// the same dye reads on different materials — terracotta is fired and muted, glass is washed out.
struct DyedFamily {
    const char* suffix;
    float saturation; // 1.0 keeps the wool color
    float lightness;
};

inline bool resolveDyedBlock(const std::string& name, projv::Color& color) {
    static const DyedFamily FAMILIES[] = {
        {"_wool", 1.0f, 1.0f},         {"_concrete", 1.0f, 0.95f},
        {"_concrete_powder", 1.0f, 1.15f}, {"_carpet", 1.0f, 1.0f},
        {"_terracotta", 0.55f, 0.85f}, {"_glazed_terracotta", 0.8f, 1.05f},
        {"_stained_glass", 0.75f, 1.15f}, {"_stained_glass_pane", 0.75f, 1.15f},
        {"_shulker_box", 0.9f, 0.9f},  {"_bed", 1.0f, 1.0f},
        {"_banner", 1.0f, 1.0f},       {"_candle", 1.0f, 1.05f}
    };

    for (const DyedFamily& family : FAMILIES) {
        if (!endsWith(name, family.suffix)) continue;
        std::string prefix = name.substr(0, name.size() - std::string(family.suffix).size());
        for (const NamedColor& dye : dyeColors()) {
            if (prefix != dye.name) continue;
            projv::Color base{dye.r, dye.g, dye.b};
            // Desaturating toward the color's own luminance keeps hue while dulling it, which is
            // what firing clay does to a dye.
            uint8_t luminance = uint8_t(0.299f * base.r + 0.587f * base.g + 0.114f * base.b);
            base = blendToward(base, {luminance, luminance, luminance}, 1.0f - family.saturation);
            color = scaleColor(base, family.lightness);
            return true;
        }
    }
    return false;
}

// Derived shapes carry the color of the material they are cut from, so the suffix is stripped and
// the base looked up again. Wood is the special case: `oak_stairs` derives from `oak_planks`, not
// from a block called `oak`.
inline bool resolveDerivedBlock(const std::string& name, projv::Color& color, int depth);

inline bool lookupWithRules(const std::string& name, projv::Color& color, int depth) {
    const auto& table = blockColorTable();
    auto exact = table.find(name);
    if (exact != table.end()) {
        color = exact->second;
        return true;
    }
    if (resolveDyedBlock(name, color)) return true;
    return resolveDerivedBlock(name, color, depth);
}

inline bool resolveDerivedBlock(const std::string& name, projv::Color& color, int depth) {
    if (depth > 3) return false;

    static const char* SHAPE_SUFFIXES[] = {
        "_stairs", "_slab", "_wall", "_fence_gate", "_fence", "_trapdoor", "_door", "_button",
        "_pressure_plate", "_sign", "_wall_sign", "_hanging_sign", "_pillar", "_bricks", "_brick",
        "_tiles", "_block"
    };
    static const char* MATERIAL_PREFIXES[] = {
        "polished_", "smooth_", "chiseled_", "cracked_", "cut_", "waxed_", "stripped_", "infested_"
    };

    for (const char* prefix : MATERIAL_PREFIXES) {
        if (startsWith(name, prefix)) {
            std::string base = name.substr(std::string(prefix).size());
            if (lookupWithRules(base, color, depth + 1)) return true;
        }
    }

    for (const char* suffix : SHAPE_SUFFIXES) {
        if (!endsWith(name, suffix)) continue;
        std::string base = name.substr(0, name.size() - std::string(suffix).size());
        if (base.empty()) continue;

        // Wood shapes derive from planks; `oak` on its own is not a block.
        if (lookupWithRules(base + "_planks", color, depth + 1)) return true;
        if (lookupWithRules(base, color, depth + 1)) return true;
        if (lookupWithRules(base + "_block", color, depth + 1)) return true;
    }

    // Logs come in several forms that all read as the same wood.
    if (endsWith(name, "_wood") || endsWith(name, "_hyphae")) {
        std::string base = name.substr(0, name.rfind('_'));
        if (lookupWithRules(base + "_log", color, depth + 1)) return true;
        if (lookupWithRules(base + "_stem", color, depth + 1)) return true;
    }

    // Deepslate ores are the stone ore darkened toward deepslate.
    if (startsWith(name, "deepslate_") && endsWith(name, "_ore")) {
        std::string base = name.substr(std::string("deepslate_").size());
        if (lookupWithRules(base, color, depth + 1)) {
            color = blendToward(color, {80, 80, 82}, 0.55f);
            return true;
        }
    }

    return false;
}

} // namespace detail

/**
 * Resolves a block name to a color using the table and the naming rules.
 * @param name Block name with the namespace already stripped (e.g. "oak_stairs").
 * @param color Receives the resolved color.
 * @return bool True if the block is known; false means the caller should fall back to gray.
 */
inline bool resolveBlockColor(const std::string& name, projv::Color& color) {
    if (detail::lookupWithRules(name, color, 0)) return true;

    // Last resort before giving up: a few keywords that appear across whole families and are
    // better than neutral gray even when the exact block is unknown (modded blocks, new additions).
    struct Keyword { const char* text; projv::Color color; };
    static const Keyword KEYWORDS[] = {
        {"leaves", {62, 142, 35}},   {"planks", {162, 131, 79}},  {"log", {109, 85, 50}},
        {"wood", {109, 85, 50}},     {"grass", {88, 130, 53}},    {"water", {63, 118, 228}},
        {"lava", {217, 110, 26}},    {"sand", {219, 207, 163}},   {"glass", {200, 225, 231}},
        {"ore", {125, 125, 125}},    {"stone", {125, 125, 125}},  {"brick", {150, 97, 83}},
        {"ice", {145, 183, 253}},    {"snow", {249, 254, 254}},   {"coral", {200, 90, 140}},
        {"flower", {200, 90, 110}},  {"wart", {114, 2, 2}},       {"moss", {89, 109, 45}}
    };
    for (const Keyword& keyword : KEYWORDS) {
        if (name.find(keyword.text) != std::string::npos) {
            color = keyword.color;
            return true;
        }
    }

    return false;
}

} // namespace minecraft

#endif // PROJECTV_MINECRAFT_BLOCKS_HPP
