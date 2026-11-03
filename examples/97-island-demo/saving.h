// Worlds, saved and loaded (docs/plans/island-demo.md, D11, I4). A world is a folder in worlds/ beside the
// program:
//
//   worlds/<folder>/
//     compose.json, island.data, plants.data   the island: the engine's own scene format
//     world.json                               its name, seed, the player, the inventory, the time played
//
// world.json is small and written at once. The voxels are written only when they changed since the last
// save (a .data is one file, so the whole of it): copied out of the scene on the main thread
// (utils::dataFileFromComponent) and written on a worker thread, to a temporary file renamed over the old,
// so a save never leaves half a world and never holds up a frame for long. Quitting waits for it.
#ifndef ISLAND_DEMO_SAVING_H
#define ISLAND_DEMO_SAVING_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/math.h"
#include "utils/compose_io.h"

namespace saving {
    namespace fs = std::filesystem;
    using projv::core::vec3;

    constexpr float AUTOSAVE_SECONDS = 60.0f;   // often: bgfx's Vulkan timer query crashes now and then on this driver, and a crash saves nothing

    struct WorldInfo {
        std::string folder;            // the folder's name in worlds/
        std::string name;              // what the start screen shows
        uint32_t    seed = 0;
        int64_t     lastPlayed = 0;    // seconds since 1970
        double      played = 0.0;      // seconds, all told
        bool        hasPlayer = false; // a saved position (a new world has none: the spawn point)
        vec3        feet{0.0f};
        float       yaw = 0.0f, pitch = -0.1f;
        bool        collect = false;
        std::map<std::string, double> inventory;
    };

    inline int64_t now() { return int64_t(std::time(nullptr)); }

    inline bool readInfo(const fs::path& folder, WorldInfo& info) {
        std::ifstream in(folder / "world.json");
        if (!in) return false;
        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;
        info.folder = folder.filename().string();
        info.name = j.value("name", info.folder);
        info.seed = j.value("seed", 0u);
        info.lastPlayed = j.value("lastPlayed", int64_t(0));
        info.played = j.value("played", 0.0);
        info.collect = j.value("collect", false);
        if (j.contains("player") && j["player"].is_object()) {
            const auto& p = j["player"];
            if (p.contains("feet") && p["feet"].is_array() && p["feet"].size() == 3) {
                info.feet = vec3(p["feet"][0].get<float>(), p["feet"][1].get<float>(), p["feet"][2].get<float>());
                info.hasPlayer = true;
            }
            info.yaw = p.value("yaw", 0.0f);
            info.pitch = p.value("pitch", -0.1f);
        }
        if (j.contains("inventory") && j["inventory"].is_object())
            for (const auto& [material, voxels] : j["inventory"].items())
                if (voxels.is_number()) info.inventory[material] = voxels.get<double>();
        return true;
    }

    inline bool writeInfo(const fs::path& folder, const WorldInfo& info) {
        nlohmann::json j;
        j["v"] = 1;
        j["name"] = info.name;
        j["seed"] = info.seed;
        j["lastPlayed"] = info.lastPlayed;
        j["played"] = info.played;
        j["collect"] = info.collect;
        if (info.hasPlayer) j["player"] = {{"feet", {info.feet.x, info.feet.y, info.feet.z}}, {"yaw", info.yaw}, {"pitch", info.pitch}};
        j["inventory"] = nlohmann::json::object();
        for (const auto& [material, voxels] : info.inventory) j["inventory"][material] = voxels;
        const fs::path tmp = folder / "world.json.tmp";
        {
            std::ofstream out(tmp);
            if (!out) return false;
            out << j.dump(2) << "\n";
        }
        std::error_code error;
        fs::rename(tmp, folder / "world.json", error);
        return !error;
    }

    // Every world in `worlds`, the most recently played first.
    inline std::vector<WorldInfo> listWorlds(const fs::path& worlds) {
        std::vector<WorldInfo> out;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(worlds, error)) {
            WorldInfo info;
            if (entry.is_directory() && fs::exists(entry.path() / "compose.json") && readInfo(entry.path(), info)) out.push_back(info);
        }
        std::sort(out.begin(), out.end(), [](const WorldInfo& a, const WorldInfo& b) { return a.lastPlayed > b.lastPlayed; });
        return out;
    }

    // A folder name for a world's name: letters, digits and dashes, unique in `worlds`.
    inline std::string folderFor(const fs::path& worlds, const std::string& name) {
        std::string base;
        for (char c : name) {
            if (std::isalnum(static_cast<unsigned char>(c))) base += char(std::tolower(static_cast<unsigned char>(c)));
            else if (!base.empty() && base.back() != '-') base += '-';
        }
        while (!base.empty() && base.back() == '-') base.pop_back();
        if (base.empty()) base = "island";
        std::string folder = base;
        for (int n = 2; fs::exists(worlds / folder); n++) folder = base + "-" + std::to_string(n);
        return folder;
    }

    inline std::string lastPlayedText(int64_t when) {
        if (when <= 0) return "never played";
        const std::time_t t = std::time_t(when);
        char text[64];
        std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", std::localtime(&t));
        return text;
    }
    inline std::string playedText(double seconds) {
        const int minutes = int(seconds / 60.0);
        return minutes < 60 ? std::to_string(minutes) + " min played"
                            : std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min played";
    }

    // The voxel writer: one save in flight at a time, on a worker thread.
    struct Saver {
        std::thread worker;
        std::atomic<bool> busy{false};
        std::atomic<bool> failed{false};
        uint64_t savedSignature = 0;   // what the voxels were when last written (an edit count)
        float sinceSave = 0.0f;
        int saves = 0;

        ~Saver() { wait(); }
        void wait() {
            if (worker.joinable()) worker.join();
            busy = false;
        }
    };

    // Writes `files` (name in the folder -> its contents) on the worker: each to a temporary file, renamed
    // over the old. Waits for a save already under way first.
    inline void writeVoxels(Saver& saver, const fs::path& folder, std::vector<std::pair<std::string, projv::DataFile>> files) {
        saver.wait();
        saver.busy = true;
        saver.worker = std::thread([&saver, folder, files = std::move(files)]() {
            bool ok = true;
            for (const auto& [name, data] : files) {
                const fs::path tmp = folder / (name + ".tmp");
                if (!projv::utils::writeDataFile(tmp.string(), data)) { ok = false; continue; }
                std::error_code error;
                fs::rename(tmp, folder / name, error);
                if (error) ok = false;
            }
            saver.failed = !ok;
            if (!ok) projv::core::error("island: the save to '{}' failed", folder.string());
            saver.busy = false;
        });
    }
}

#endif
