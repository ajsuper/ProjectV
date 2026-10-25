#include "teardown_import.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>

#include "discover.h"
#include "level.h"
#include "preview.h"
#include "pugixml.hpp"
#include "verify.h"
#include "vox_file.h"
#include "writer.h"

namespace fs = std::filesystem;

int teardown::g_verbosity = 1;

namespace teardown {

namespace {
    // A level file given by path rather than found in the install. Its mod is the nearest folder
    // above it with an info.txt (where MOD/ points); without one, the file's own folder.
    LevelSource sourceForFile(const std::string& file, const std::string& gameDir) {
        const fs::path xml = fs::absolute(file);
        fs::path mod = xml.parent_path();
        for (fs::path p = mod; !p.empty() && p != p.root_path(); p = p.parent_path()) {
            if (fs::exists(p / "info.txt")) { mod = p; break; }
        }
        LevelSource s;
        s.id = xml.stem().string() == "main" ? mod.filename().string() : xml.stem().string();
        s.title = s.id;
        s.xmlPath = xml.string();
        s.paths.modRoot = mod.string();
        s.paths.levelDir = (xml.parent_path() / xml.stem()).string();
        s.paths.dataDir = gameDir.empty() ? std::string() : (fs::path(gameDir) / "data").string();
        return s;
    }

    int dumpVox(const std::string& path) {
        auto f = loadVox(path);
        if (!f) return 1;
        for (size_t m = 0; m < f->models.size(); m++)
            std::printf("model %zu: size %d %d %d, %zu voxels\n", m, f->models[m].size.x, f->models[m].size.y,
                        f->models[m].size.z, f->models[m].voxels.size());
        for (const auto& inst : f->instances)
            std::printf("instance '%s' model %d t=(%d %d %d)\n", inst.name.c_str(), inst.model, inst.translation.x,
                        inst.translation.y, inst.translation.z);
        Canvas c;
        fileToCanvas(*f, c);
        ivec3 lo, hi;
        if (c.bounds(lo, hi))
            std::printf("whole file in Teardown axes: (%d %d %d)-(%d %d %d), %zu voxels\n", lo.x, lo.y, lo.z, hi.x, hi.y,
                        hi.z, c.voxelCount());
        return 0;
    }

    // Converts one level; false on failure.
    bool convert(const LevelSource& src, const std::string& folderPath, const Options& options) {
        const auto t0 = std::chrono::steady_clock::now();
        logInfo("\n== %s  (%s)", src.id.c_str(), src.title.c_str());

        LevelOptions levelOptions;
        levelOptions.separateDynamic = options.separateDynamic;
        levelOptions.runVoxscripts = options.voxscripts;
        levelOptions.water = options.water && options.transparent;
        levelOptions.transparent = options.transparent;
        Level level;
        if (!buildLevel(src, levelOptions, level)) return false;
        const auto t1 = std::chrono::steady_clock::now();
        logInfo("   read %zu shapes, %zu prefabs, %zu voxscripts; %zu dynamic bodies  (%.1fs)", level.shapes, level.prefabs,
                level.voxscripts, level.bodies.size(), std::chrono::duration<double>(t1 - t0).count());
        if (level.transparentRemoved) logInfo("   --no-transparent: %zu glass voxels left out", level.transparentRemoved);
        if (level.world.empty() && level.bodies.empty()) {
            logInfo("   no geometry (a menu or script-only scene) - skipped");
            return true;
        }

        WriteOptions writeOptions;
        writeOptions.blockResolution = options.blockResolution;
        writeOptions.physicsNames = options.physics;
        writeOptions.entities = options.physics;
        writeOptions.threads = int(options.threads);
        writeOptions.scale = options.scale;
        WriteReport report;
        if (!writeLevel(level, src, folderPath, writeOptions, report)) {
            logWarn("writing %s failed", folderPath.c_str());
            return false;
        }
        const auto t2 = std::chrono::steady_clock::now();
        logInfo("   world: %zu voxels in %zu blocks, %zu materials%s", report.worldVoxels, report.worldBlocks,
                report.worldMaterials, report.worldMaterials > 255 ? " (quantised to 255)" : "");
        if (report.worldMaterials > 255) logInfo("   palette: largest colour shift %.3f (Oklab)", report.worldPaletteError);
        logInfo("   bodies: %zu placed from %zu files, %zu voxels", report.bodies, report.bodyFiles, report.bodyVoxels);
        if (report.waterVoxels) logInfo("   water: %zu voxels", report.waterVoxels);
        logInfo("   wrote %s  (%.1f MB, %.1fs)", folderPath.c_str(), report.bytes / 1048576.0,
                std::chrono::duration<double>(t2 - t1).count());

        // Read it back the way a game would, and check nothing went missing on the way.
        const VerifyResult check = verifyFolder(folderPath, false);
        const size_t expected = report.worldVoxels + report.bodyVoxels + report.waterVoxels;
        if (!check.ok || check.voxels != expected) {
            logWarn("%s does not read back intact: %zu of %zu voxels", folderPath.c_str(), check.voxels, expected);
            return false;
        }
        logInfo("   verified: ProjectV loads %zu chunks (%zu grids, %zu loose), all %zu voxels", check.chunks, check.grids,
                check.loose, check.voxels);

        if (options.preview) {
            const std::string png = (fs::path(folderPath) / "preview.png").string();
            if (renderPreview(level, png, options.previewWidth, options.previewFocus, options.previewRadius))
                logInfo("   preview: %s", png.c_str());
        }
        return true;
    }
}

bool isLevelFile(const std::string& path) {
    if (fs::path(path).extension() != ".xml") return false;
    pugi::xml_document doc;
    return doc.load_file(path.c_str()) && std::string(doc.document_element().name()) == "scene";
}

int run(const Options& options) {
    if (!options.dumpVox.empty()) return dumpVox(options.dumpVox);
    if (!options.verify.empty()) {
        bool ok = true;
        for (const auto& dir : options.verify) ok = verifyFolder(dir).ok && ok;
        return ok ? 0 : 1;
    }
    if (!options.rescale.empty()) {
        const float factor = float(std::atof(options.rescale[0].c_str()));
        if (factor <= 0.0f || options.rescale.size() < 2) {
            logWarn("--td-rescale takes a positive factor and then the folders to rescale");
            return 2;
        }
        bool ok = true;
        for (size_t i = 1; i < options.rescale.size(); i++) {
            const bool done = rescaleCompose(options.rescale[i], factor);
            std::printf("%s %s\n", done ? "rescaled" : "FAILED", options.rescale[i].c_str());
            ok = done && ok;
        }
        return ok ? 0 : 1;
    }

    const std::string game = findTeardown(options.game);

    // A level file given directly.
    if (!options.levelFile.empty()) {
        if (game.empty()) logWarn("Teardown not found, so BUILT-IN/ paths will not resolve. Pass --td-game <game folder>.");
        const LevelSource src = sourceForFile(options.levelFile, game);
        const std::string folder = options.outputDirectory.empty() ? (fs::path("converted") / src.id).string()
                                                                   : options.outputDirectory;
        return convert(src, folder, options) ? 0 : 1;
    }

    if (game.empty()) {
        logWarn("Teardown not found%s. Pass --td-game <game folder>.",
                options.game.empty() ? " in any Steam library" : (" at " + options.game).c_str());
        return 1;
    }
    const std::vector<LevelSource> levels = findLevels(game);
    logInfo("Teardown: %s (%zu levels)", game.c_str(), levels.size());
    if (options.list) {
        for (const auto& l : levels) std::printf("  %-36s %s\n", l.id.c_str(), l.title.c_str());
        return 0;
    }

    std::vector<const LevelSource*> chosen;
    std::set<std::string> found;
    for (const auto& l : levels)
        if (options.all || std::find(options.levels.begin(), options.levels.end(), l.id) != options.levels.end()) {
            chosen.push_back(&l);
            found.insert(l.id);
        }
    for (const auto& w : options.levels)
        if (!found.count(w)) logWarn("no installed level '%s' (see --td-list)", w.c_str());
    if (chosen.empty()) return 1;

    const fs::path root = options.outputDirectory.empty() ? fs::path("converted") : fs::path(options.outputDirectory);
    int failures = 0;
    for (const LevelSource* src : chosen)
        if (!convert(*src, (root / src->id).string(), options)) failures++;
    if (failures) logWarn("%d of %zu levels failed", failures, chosen.size());
    return failures ? 1 : 0;
}

} // namespace teardown
