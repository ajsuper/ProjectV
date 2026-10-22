#include "discover.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

#include "pugixml.hpp"

namespace fs = std::filesystem;

namespace teardown {

namespace {
    constexpr const char* APP_ID = "1167630";

    std::string home() {
        const char* h = std::getenv("HOME");
        return h ? h : "";
    }

    // Steam roots, and every library each one lists in libraryfolders.vdf.
    std::vector<fs::path> steamLibraries() {
        const std::string h = home();
        std::vector<fs::path> roots = {
            h + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
            h + "/.local/share/Steam",
            h + "/.steam/steam",
        };
        std::vector<fs::path> libs;
        std::set<std::string> seen;
        std::error_code ec;
        auto add = [&](const fs::path& p) {
            fs::path c = fs::weakly_canonical(p, ec);
            if (fs::is_directory(c / "steamapps", ec) && seen.insert(c.string()).second) libs.push_back(c);
        };
        for (const auto& r : roots) {
            add(r);
            std::ifstream vdf(r / "steamapps" / "libraryfolders.vdf");
            std::string line;
            static const std::regex pathLine("\"path\"\\s+\"([^\"]+)\"");
            std::smatch m;
            while (std::getline(vdf, line))
                if (std::regex_search(line, m, pathLine)) add(m[1].str());
        }
        return libs;
    }

    bool isGameDir(const fs::path& p) {
        std::error_code ec;
        return fs::is_directory(p / "data", ec) && (fs::exists(p / "teardown.exe", ec) || fs::exists(p / "teardown", ec));
    }

    std::string infoValue(const fs::path& info, const std::string& key) {
        std::ifstream in(info);
        std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            auto trim = [](std::string& s) {
                s.erase(0, s.find_first_not_of(" \t\r"));
                s.erase(s.find_last_not_of(" \t\r") + 1);
            };
            trim(k);
            trim(v);
            if (k == key) return v;
        }
        return {};
    }

    std::string slug(std::string s) {
        std::string out;
        for (char c : s) {
            if (std::isalnum(uint8_t(c))) out += char(std::tolower(uint8_t(c)));
            else if (!out.empty() && out.back() != '_') out += '_';
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        return out;
    }

    void scanMod(const fs::path& mod, const std::string& prefix, const fs::path& dataDir, std::vector<LevelSource>& out) {
        std::error_code ec;
        std::string title = infoValue(mod / "info.txt", "en_name");
        if (title.empty()) title = infoValue(mod / "info.txt", "name");
        std::vector<fs::path> xmls;
        for (const auto& e : fs::directory_iterator(mod, ec))
            if (e.is_regular_file() && e.path().extension() == ".xml") xmls.push_back(e.path());
        std::sort(xmls.begin(), xmls.end());
        for (const fs::path& xml : xmls) {
            pugi::xml_document doc;
            // Only the root element's name matters; parsing just that much is not offered, and the
            // files are small next to their voxels.
            if (!doc.load_file(xml.c_str()) || std::string(doc.document_element().name()) != "scene") continue;
            LevelSource s;
            const std::string stem = xml.stem().string();
            s.id = prefix + slug(mod.filename().string()) + (stem == "main" ? "" : "_" + slug(stem));
            s.title = title.empty() ? mod.filename().string() : title;
            if (stem != "main") s.title += " (" + stem + ")";
            s.xmlPath = xml.string();
            s.paths.modRoot = mod.string();
            s.paths.levelDir = (xml.parent_path() / stem).string();
            s.paths.dataDir = dataDir.string();
            out.push_back(std::move(s));
        }
    }
}

std::string findTeardown(const std::string& hint) {
    if (!hint.empty()) return isGameDir(hint) ? fs::path(hint).string() : std::string();
    for (const auto& lib : steamLibraries()) {
        fs::path p = lib / "steamapps" / "common" / "Teardown";
        if (isGameDir(p)) return p.string();
    }
    return {};
}

std::vector<LevelSource> findLevels(const std::string& teardownDir) {
    std::vector<LevelSource> out;
    const fs::path game(teardownDir), data = game / "data";
    std::error_code ec;
    auto scanDir = [&](const fs::path& dir, const std::string& prefix) {
        if (!fs::is_directory(dir, ec)) return;
        std::vector<fs::path> mods;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory()) mods.push_back(e.path());
        std::sort(mods.begin(), mods.end());
        for (const auto& m : mods) scanMod(m, prefix, data, out);
    };
    scanDir(game / "mods", "");
    scanDir(game / "dlcs", "");
    // Workshop subscriptions and local mods live beside the game, not in it.
    const fs::path steamapps = game.parent_path().parent_path();
    scanDir(steamapps / "workshop" / "content" / APP_ID, "workshop_");
    scanDir(steamapps / "compatdata" / APP_ID / "pfx" / "drive_c" / "users" / "steamuser" / "Documents" / "Teardown" / "mods",
            "local_");
    return out;
}

} // namespace teardown
