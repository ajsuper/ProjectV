#include "net/wire.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace projv::net {
    // ---- Bits ----

    void BitWriter::bits(uint64_t value, uint32_t count) {
        count = std::min(count, 64u);
        for (uint32_t i = 0; i < count; i++) {
            if (bitsWritten % 8 == 0) out.push_back(0);
            if ((value >> i) & 1u) out.back() |= uint8_t(1u << (bitsWritten % 8));
            bitsWritten++;
        }
    }

    void BitWriter::varint(uint64_t value) {
        do {
            uint64_t group = value & 0x7Fu;
            value >>= 7;
            bits(group | (value ? 0x80u : 0u), 8);
        } while (value);
    }

    void BitWriter::signedVarint(int64_t value) {
        varint((uint64_t(value) << 1) ^ uint64_t(value >> 63));
    }

    void BitWriter::f32(float value) {
        uint32_t raw;
        std::memcpy(&raw, &value, 4);
        bits(raw, 32);
    }

    void BitWriter::bytes(std::span<const uint8_t> data) {
        varint(data.size());
        for (uint8_t b : data) bits(b, 8);
    }

    void BitWriter::quantized(float value, float min, float max, uint32_t count) {
        count = std::clamp(count, 1u, 32u);
        const uint64_t top = (uint64_t(1) << count) - 1;
        float t = max > min && std::isfinite(value) ? (std::clamp(value, min, max) - min) / (max - min) : 0.0f;
        bits(uint64_t(std::llround(double(t) * double(top))), count);
    }

    uint64_t BitReader::bits(uint32_t count) {
        count = std::min(count, 64u);
        if (failed || at + count > in.size() * 8) {
            failed = true;
            return 0;
        }
        uint64_t value = 0;
        for (uint32_t i = 0; i < count; i++, at++)
            if ((in[at / 8] >> (at % 8)) & 1u) value |= uint64_t(1) << i;
        return value;
    }

    uint64_t BitReader::varint() {
        uint64_t value = 0;
        for (uint32_t shift = 0; shift < 64; shift += 7) {
            uint64_t group = bits(8);
            if (failed) return 0;
            value |= (group & 0x7Fu) << shift;
            if (!(group & 0x80u)) return value;
        }
        failed = true;   // longer than any 64-bit number
        return 0;
    }

    int64_t BitReader::signedVarint() {
        uint64_t z = varint();
        return int64_t(z >> 1) ^ -int64_t(z & 1u);
    }

    float BitReader::f32() {
        uint32_t raw = uint32_t(bits(32));
        float value;
        std::memcpy(&value, &raw, 4);
        return value;
    }

    std::vector<uint8_t> BitReader::bytes(size_t maxLength) {
        uint64_t length = varint();
        if (failed || length > maxLength || length * 8 > bitsLeft()) {
            failed = true;
            return {};
        }
        std::vector<uint8_t> out(length);
        for (uint8_t& b : out) b = uint8_t(bits(8));
        return out;
    }

    float BitReader::quantized(float min, float max, uint32_t count) {
        count = std::clamp(count, 1u, 32u);
        const uint64_t top = (uint64_t(1) << count) - 1;
        uint64_t raw = bits(count);
        return min + (max - min) * float(double(raw) / double(top));
    }

    // ---- Bodies ----

    void writePosition(BitWriter& w, core::vec3 position, core::vec3 origin, float step) {
        for (int i = 0; i < 3; i++) {
            double steps = std::isfinite(position[i]) ? std::round(double(position[i] - origin[i]) / double(step)) : 0.0;
            w.signedVarint(int64_t(std::clamp(steps, -1e15, 1e15)));
        }
    }

    core::vec3 readPosition(BitReader& r, core::vec3 origin, float step) {
        core::vec3 p;
        for (int i = 0; i < 3; i++) p[i] = origin[i] + float(double(r.signedVarint()) * double(step));
        return p;
    }

    namespace {
        constexpr float SMALLEST = 0.70710678f;   // 1/sqrt2: the most a component other than the largest can be
    }

    void writeRotation(BitWriter& w, core::quat q, uint32_t bitsEach) {
        float c[4] = {q.x, q.y, q.z, q.w};
        float length = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
        if (!(length > 1e-6f) || !std::isfinite(length)) {
            c[0] = c[1] = c[2] = 0.0f;
            c[3] = 1.0f;
            length = 1.0f;
        }
        int largest = 0;
        for (int i = 1; i < 4; i++)
            if (std::abs(c[i]) > std::abs(c[largest])) largest = i;
        const float sign = c[largest] < 0.0f ? -1.0f : 1.0f;
        w.bits(uint64_t(largest), 2);
        for (int i = 0; i < 4; i++)
            if (i != largest) w.quantized(c[i] * sign / length, -SMALLEST, SMALLEST, bitsEach);
    }

    core::quat readRotation(BitReader& r, uint32_t bitsEach) {
        const int largest = int(r.bits(2));
        float c[4];
        float sum = 0.0f;
        for (int i = 0; i < 4; i++) {
            if (i == largest) continue;
            c[i] = r.quantized(-SMALLEST, SMALLEST, bitsEach);
            sum += c[i] * c[i];
        }
        c[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
        core::quat q(c[3], c[0], c[1], c[2]);
        return glm::normalize(q);
    }

    void writeVector(BitWriter& w, core::vec3 v, float limit, uint32_t bitsEach) {
        for (int i = 0; i < 3; i++) w.quantized(v[i], -limit, limit, bitsEach);
    }

    core::vec3 readVector(BitReader& r, float limit, uint32_t bitsEach) {
        core::vec3 v;
        for (int i = 0; i < 3; i++) v[i] = r.quantized(-limit, limit, bitsEach);
        return v;
    }

    // ---- Labels ----

    void writeLabels(BitWriter& w, std::span<const uint32_t> labels) {
        size_t runs = 0;
        for (size_t i = 0; i < labels.size(); i++)
            if (i == 0 || labels[i] != labels[i - 1]) runs++;
        w.varint(runs);
        for (size_t i = 0; i < labels.size();) {
            size_t j = i;
            while (j < labels.size() && labels[j] == labels[i]) j++;
            w.varint(labels[i]);
            w.varint(j - i);
            i = j;
        }
    }

    std::vector<uint32_t> readLabels(BitReader& r, size_t maxCells, uint32_t maxLabel) {
        std::vector<uint32_t> out;
        uint64_t runs = r.varint();
        if (r.failed || runs > maxCells) {
            r.failed = true;
            return {};
        }
        for (uint64_t k = 0; k < runs; k++) {
            uint64_t label = r.varint();
            uint64_t length = r.varint();
            if (r.failed || label > maxLabel || length == 0 || length > maxCells - out.size()) {
                r.failed = true;
                return {};
            }
            out.insert(out.end(), size_t(length), uint32_t(label));
        }
        return out;
    }
}
