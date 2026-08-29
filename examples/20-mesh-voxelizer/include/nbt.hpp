#ifndef PROJECTV_NBT_HPP
#define PROJECTV_NBT_HPP

// A reader for NBT (Named Binary Tag), the tagged binary format Minecraft stores its world data in.
//
// The format is a self-describing tree: every tag carries a one-byte type, a name, and a payload,
// with compounds and lists nesting arbitrarily. All integers are big-endian. That is the whole
// specification — which is why parsing it inline here is preferable to taking on a dependency for
// the few hundred lines it costs.
//
// Everything is parsed into one owning `Value` tree. Chunks are read and discarded one at a time,
// so the fat-struct representation (which reserves a field per payload kind rather than using a
// variant) costs a chunk's worth of memory at a time, and buys accessors that never have to be
// pattern-matched.

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace nbt {

enum class TagType : uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12
};

struct Value;
using Compound = std::map<std::string, Value>;
using List = std::vector<Value>;

struct Value {
    TagType type = TagType::End;
    int64_t number = 0;   // Byte, Short, Int, Long
    double decimal = 0.0; // Float, Double
    std::string text;     // String
    std::vector<int8_t> byteArray;
    std::vector<int32_t> intArray;
    std::vector<int64_t> longArray;
    List list;
    Compound compound;
};

namespace detail {

// Bounds-checked big-endian cursor. Every read past the end sets the failure flag and returns zero
// rather than throwing, so a truncated or corrupt chunk degrades to "this chunk failed to parse"
// instead of taking the tool down — region files in the wild are routinely a little broken.
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool failed() const { return failed_; }
    size_t remaining() const { return failed_ ? 0 : size_ - offset_; }

    uint8_t readByte() {
        if (!require(1)) return 0;
        return data_[offset_++];
    }

    int16_t readShort() {
        if (!require(2)) return 0;
        int16_t value = int16_t((uint16_t(data_[offset_]) << 8) | uint16_t(data_[offset_ + 1]));
        offset_ += 2;
        return value;
    }

    int32_t readInt() {
        if (!require(4)) return 0;
        uint32_t value = 0;
        for (int i = 0; i < 4; i++) value = (value << 8) | data_[offset_ + i];
        offset_ += 4;
        return int32_t(value);
    }

    int64_t readLong() {
        if (!require(8)) return 0;
        uint64_t value = 0;
        for (int i = 0; i < 8; i++) value = (value << 8) | data_[offset_ + i];
        offset_ += 8;
        return int64_t(value);
    }

    float readFloat() {
        uint32_t bits = uint32_t(readInt());
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    double readDouble() {
        uint64_t bits = uint64_t(readLong());
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::string readString() {
        uint16_t length = uint16_t(readShort());
        if (!require(length)) return {};
        std::string text(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return text;
    }

private:
    bool require(size_t bytes) {
        if (failed_ || offset_ + bytes > size_) {
            failed_ = true;
            return false;
        }
        return true;
    }

    const uint8_t* data_;
    size_t size_;
    size_t offset_ = 0;
    bool failed_ = false;
};

// Nesting is bounded so that a hand-crafted file cannot drive the parser into a stack overflow.
constexpr int MAX_NESTING_DEPTH = 64;

inline bool readPayload(Reader& reader, TagType type, Value& value, int depth);

inline bool readCompoundBody(Reader& reader, Compound& compound, int depth) {
    while (true) {
        TagType type = TagType(reader.readByte());
        if (reader.failed()) return false;
        if (type == TagType::End) return true;

        std::string name = reader.readString();
        Value value;
        if (!readPayload(reader, type, value, depth + 1)) return false;
        compound.emplace(std::move(name), std::move(value));
    }
}

inline bool readPayload(Reader& reader, TagType type, Value& value, int depth) {
    if (depth > MAX_NESTING_DEPTH) return false;
    value.type = type;

    switch (type) {
        case TagType::Byte:   value.number = int8_t(reader.readByte()); break;
        case TagType::Short:  value.number = reader.readShort(); break;
        case TagType::Int:    value.number = reader.readInt(); break;
        case TagType::Long:   value.number = reader.readLong(); break;
        case TagType::Float:  value.decimal = reader.readFloat(); break;
        case TagType::Double: value.decimal = reader.readDouble(); break;
        case TagType::String: value.text = reader.readString(); break;

        case TagType::ByteArray: {
            int32_t length = reader.readInt();
            if (length < 0 || size_t(length) > reader.remaining()) return false;
            value.byteArray.resize(size_t(length));
            for (int32_t i = 0; i < length; i++) value.byteArray[size_t(i)] = int8_t(reader.readByte());
            break;
        }
        case TagType::IntArray: {
            int32_t length = reader.readInt();
            if (length < 0 || size_t(length) * 4 > reader.remaining()) return false;
            value.intArray.resize(size_t(length));
            for (int32_t i = 0; i < length; i++) value.intArray[size_t(i)] = reader.readInt();
            break;
        }
        case TagType::LongArray: {
            int32_t length = reader.readInt();
            if (length < 0 || size_t(length) * 8 > reader.remaining()) return false;
            value.longArray.resize(size_t(length));
            for (int32_t i = 0; i < length; i++) value.longArray[size_t(i)] = reader.readLong();
            break;
        }

        case TagType::List: {
            TagType elementType = TagType(reader.readByte());
            int32_t length = reader.readInt();
            if (length < 0) return false;
            // An empty list is written with element type End, which is legal and means nothing to read.
            if (elementType == TagType::End) {
                if (length != 0) return false;
                break;
            }
            value.list.resize(size_t(length));
            for (int32_t i = 0; i < length; i++) {
                if (!readPayload(reader, elementType, value.list[size_t(i)], depth + 1)) return false;
            }
            break;
        }

        case TagType::Compound:
            if (!readCompoundBody(reader, value.compound, depth)) return false;
            break;

        case TagType::End:
        default:
            return false;
    }

    return !reader.failed();
}

} // namespace detail

/**
 * Parses an uncompressed NBT document.
 * @param data Pointer to the document bytes.
 * @param size Number of bytes available.
 * @param root Receives the root tag, which the format requires to be a compound.
 * @return bool True if the document parsed cleanly.
 */
inline bool parse(const uint8_t* data, size_t size, Value& root) {
    detail::Reader reader(data, size);
    TagType type = TagType(reader.readByte());
    if (type != TagType::Compound) return false;
    reader.readString(); // The root tag's name, conventionally empty.
    return detail::readPayload(reader, TagType::Compound, root, 0);
}

/**
 * Looks up a child tag by name.
 * @param compound The compound to search.
 * @param name The child's name.
 * @return const Value* The child, or nullptr if absent.
 */
inline const Value* find(const Compound& compound, const std::string& name) {
    auto it = compound.find(name);
    return it == compound.end() ? nullptr : &it->second;
}

/**
 * Looks up a child tag by name, requiring it to have a given type.
 * @param compound The compound to search.
 * @param name The child's name.
 * @param type The type the child must have.
 * @return const Value* The child, or nullptr if absent or of another type.
 */
inline const Value* find(const Compound& compound, const std::string& name, TagType type) {
    const Value* value = find(compound, name);
    return (value != nullptr && value->type == type) ? value : nullptr;
}

/**
 * Reads an integer child, whatever its integral width.
 * @param compound The compound to search.
 * @param name The child's name.
 * @param fallback Returned when the child is absent or not an integer.
 * @return int64_t The value.
 */
inline int64_t number(const Compound& compound, const std::string& name, int64_t fallback) {
    const Value* value = find(compound, name);
    if (value == nullptr) return fallback;
    switch (value->type) {
        case TagType::Byte:
        case TagType::Short:
        case TagType::Int:
        case TagType::Long:
            return value->number;
        default:
            return fallback;
    }
}

/**
 * Reads a string child.
 * @param compound The compound to search.
 * @param name The child's name.
 * @return std::string The value, or an empty string if absent or not a string.
 */
inline std::string text(const Compound& compound, const std::string& name) {
    const Value* value = find(compound, name, TagType::String);
    return value == nullptr ? std::string() : value->text;
}

} // namespace nbt

#endif // PROJECTV_NBT_HPP
