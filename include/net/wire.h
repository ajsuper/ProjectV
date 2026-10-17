#ifndef PROJV_NET_WIRE_H
#define PROJV_NET_WIRE_H

#include <cstdint>
#include <span>
#include <vector>

#include "core/math.h"

// What goes on the wire, bit by bit (docs/plans/networking.md): a writer and a reader of bits, the
// quantisations bodies are sent with, and run-length labels for voxel splits.
//
// **What arrives is never trusted.** A reader that runs out of bytes, or reads a count past its
// limit, does not assert: it sets `failed`, returns zeros from then on, and the caller drops the
// message. Every limit is the caller's to give.
//
// No EnTT, no runtime.
namespace projv::net {
    class BitWriter {
    public:
        void bits(uint64_t value, uint32_t count);        // the low `count` bits (0..64)
        void boolean(bool value) { bits(value ? 1 : 0, 1); }
        void varint(uint64_t value);                      // 7 bits a group: small numbers are small
        void signedVarint(int64_t value);                 // zigzag, then varint
        void f32(float value);                            // as it is, 32 bits
        void bytes(std::span<const uint8_t> data);        // a varint length, then the bytes
        // `value` in [min, max] in `count` bits: within (max - min) / (2^count - 1) / 2 of it.
        void quantized(float value, float min, float max, uint32_t count);

        size_t bitCount() const { return bitsWritten; }
        // The bytes, the last one padded with zeros.
        const std::vector<uint8_t>& data() const { return out; }
        std::vector<uint8_t> take() { return std::move(out); }

    private:
        std::vector<uint8_t> out;
        size_t bitsWritten = 0;
    };

    class BitReader {
    public:
        explicit BitReader(std::span<const uint8_t> data) : in(data) {}
        uint64_t bits(uint32_t count);
        bool     boolean() { return bits(1) != 0; }
        uint64_t varint();
        int64_t  signedVarint();
        float    f32();
        // Fails (and returns nothing) past `maxLength` bytes.
        std::vector<uint8_t> bytes(size_t maxLength);
        float    quantized(float min, float max, uint32_t count);

        bool   failed = false;   // read past the end, or a value past a limit
        size_t bitsLeft() const { return in.size() * 8 - std::min(at, in.size() * 8); }

    private:
        std::span<const uint8_t> in;
        size_t at = 0;
    };

    // ---- Bodies ------------------------------------------------------------------------------------

    // A position as whole steps of `step` metres from `origin` (a cell corner): signed varints, so a
    // body near its cell's corner costs a few bytes.
    void       writePosition(BitWriter& w, core::vec3 position, core::vec3 origin, float step);
    core::vec3 readPosition(BitReader& r, core::vec3 origin, float step);

    // A rotation, smallest three: the largest component's index (2 bits) and the other three, each in
    // [-1/sqrt2, 1/sqrt2] in `bitsEach` bits. q and -q are the same rotation; the one read back has the
    // largest component positive.
    void       writeRotation(BitWriter& w, core::quat q, uint32_t bitsEach = 9);
    core::quat readRotation(BitReader& r, uint32_t bitsEach = 9);

    // A vector each component in [-limit, limit] in `bitsEach` bits.
    void       writeVector(BitWriter& w, core::vec3 v, float limit, uint32_t bitsEach);
    core::vec3 readVector(BitReader& r, float limit, uint32_t bitsEach);

    // ---- Voxel splits ------------------------------------------------------------------------------

    // A label per cell (which piece a voxel went to, 0 for stays), as runs of (label, length): what a
    // split sends. Reading stops -- failed -- past `maxCells` cells or a label past `maxLabel`.
    void writeLabels(BitWriter& w, std::span<const uint32_t> labels);
    std::vector<uint32_t> readLabels(BitReader& r, size_t maxCells, uint32_t maxLabel);
}

#endif
