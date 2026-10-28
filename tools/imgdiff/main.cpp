// imgdiff: does a capture match its baseline within the shader library's tolerance (D5)?
//
//   projv_imgdiff before.tga after.tga [--diff out.tga]
//   projv_imgdiff before/ after/                       every image in before/ against its namesake
//
// Prints the mean absolute error, the share of pixels off by more than 2/255 and the largest single
// difference; exits 1 when any pair fails or cannot be compared. --diff writes |a - b| scaled x16.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "imgdiff.h"

namespace fs = std::filesystem;

namespace {
    struct Image {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    bool load(const fs::path& path, Image& image) {
        int channels = 0;
        uint8_t* data = stbi_load(path.string().c_str(), &image.width, &image.height, &channels, 4);
        if (!data) {
            std::fprintf(stderr, "%s: %s\n", path.string().c_str(), stbi_failure_reason());
            return false;
        }
        image.rgba.assign(data, data + size_t(image.width) * image.height * 4);
        stbi_image_free(data);
        return true;
    }

    // Uncompressed 32-bit TGA, top-left origin: enough for a diff image, no writer dependency.
    void writeDiff(const fs::path& path, const Image& a, const Image& b) {
        FILE* file = std::fopen(path.string().c_str(), "wb");
        if (!file) return;
        uint8_t header[18] = {};
        header[2] = 2;
        header[12] = uint8_t(a.width & 0xFF); header[13] = uint8_t(a.width >> 8);
        header[14] = uint8_t(a.height & 0xFF); header[15] = uint8_t(a.height >> 8);
        header[16] = 32; header[17] = 0x28;
        std::fwrite(header, 1, sizeof(header), file);
        for (size_t p = 0; p < size_t(a.width) * a.height; ++p) {
            uint8_t bgra[4] = {0, 0, 0, 255};
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(int(a.rgba[p * 4 + c]) - int(b.rgba[p * 4 + c])) * 16;
                bgra[2 - c] = uint8_t(std::min(d, 255));
            }
            std::fwrite(bgra, 1, 4, file);
        }
        std::fclose(file);
    }

    bool comparePair(const fs::path& before, const fs::path& after, const fs::path& diff) {
        Image a, b;
        if (!load(before, a) || !load(after, b)) return false;
        if (a.width != b.width || a.height != b.height) {
            std::printf("FAIL %s: %dx%d against %dx%d\n", after.string().c_str(), a.width, a.height, b.width, b.height);
            return false;
        }
        const projv::imgdiff::Result r =
            projv::imgdiff::compare(a.rgba, b.rgba, size_t(a.width) * a.height, 4);
        std::printf("%s %s: mean %.4f%%, over 2/255 %.4f%% of pixels, largest %d/255\n",
                    r.pass ? "pass" : "FAIL", after.string().c_str(),
                    r.meanAbsolute * 100.0, r.overFraction * 100.0, r.largest);
        if (!diff.empty()) writeDiff(diff, a, b);
        return r.pass;
    }
}

int main(int argc, char** argv) {
    std::vector<std::string> paths;
    fs::path diff;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--diff" && i + 1 < argc) diff = argv[++i];
        else paths.push_back(argument);
    }
    if (paths.size() != 2) {
        std::fprintf(stderr, "usage: projv_imgdiff <before> <after> [--diff out.tga]  (two images or two folders)\n");
        return 2;
    }

    const fs::path before = paths[0], after = paths[1];
    if (!fs::is_directory(before)) {
        if (!diff.empty() && diff.has_parent_path()) fs::create_directories(diff.parent_path());
        return comparePair(before, after, diff) ? 0 : 1;
    }

    bool allPass = true;
    int compared = 0;
    std::vector<fs::path> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(before)) {
        const std::string extension = entry.path().extension().string();
        if (extension == ".tga" || extension == ".png") names.push_back(entry.path().filename());
    }
    std::sort(names.begin(), names.end());
    if (!diff.empty()) fs::create_directories(diff);
    for (const fs::path& name : names) {
        if (!fs::exists(after / name)) {
            std::printf("FAIL %s: missing\n", (after / name).string().c_str());
            allPass = false;
            continue;
        }
        allPass &= comparePair(before / name, after / name, diff.empty() ? fs::path() : diff / name);
        ++compared;
    }
    std::printf("%d compared, %s\n", compared, allPass ? "all pass" : "FAILURES");
    return allPass && compared > 0 ? 0 : 1;
}
