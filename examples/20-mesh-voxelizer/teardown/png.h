#ifndef PROJECTV_TEARDOWN_PNG_H
#define PROJECTV_TEARDOWN_PNG_H
// PNG in and out: heightmaps for voxscripts, preview renders for the report.

#include <cstdint>
#include <string>
#include <vector>

namespace teardown {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    const uint8_t* at(int x, int y) const { return &rgba[(size_t(y) * w + x) * 4]; }
};

bool loadImage(const std::string& path, Image& out);
bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_PNG_H
