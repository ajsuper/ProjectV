#include "png.h"

#include <cstdio>
#include <cstring>
#include <zlib.h>

// stb_image's implementation is compiled once, in main.cpp.
#include "stb_image.h"

namespace teardown {

bool loadImage(const std::string& path, Image& out) {
    int w, h, n;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return true;
}

namespace {
    void be32(std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
    }
    void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> head;
        be32(head, uint32_t(data.size()));
        std::fwrite(head.data(), 1, 4, f);
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        std::fwrite(body.data(), 1, body.size(), f);
        std::vector<uint8_t> crc;
        be32(crc, uint32_t(crc32(0, body.data(), uInt(body.size()))));
        std::fwrite(crc.data(), 1, 4, f);
    }
}

bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    be32(ihdr, uint32_t(w));
    be32(ihdr, uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
    }
    uLongf len = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(len);
    compress2(z.data(), &len, raw.data(), uLong(raw.size()), 6);
    z.resize(len);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    return std::fclose(f) == 0;
}

} // namespace teardown
