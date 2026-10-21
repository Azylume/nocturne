#include "media/snappy.hpp"

#include <cstring>

namespace azl {

namespace {

std::optional<size_t> varint(const uint8_t*& p, const uint8_t* end)
{
    uint64_t value = 0;
    for (int shift = 0; shift < 35 && p < end; shift += 7) {
        const uint8_t byte = *p++;
        value |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return value <= 0xffffffffu ? std::optional<size_t>(size_t(value)) : std::nullopt;
    }
    return std::nullopt;
}

uint32_t littleEndian(const uint8_t* p, size_t bytes)
{
    uint32_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
        value |= uint32_t(p[i]) << (8 * i);
    return value;
}

uint32_t load32(const uint8_t* p)
{
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}

void putVarint(std::vector<uint8_t>& out, size_t value)
{
    while (value >= 0x80) {
        out.push_back(uint8_t(value | 0x80));
        value >>= 7;
    }
    out.push_back(uint8_t(value));
}

void putLiteral(std::vector<uint8_t>& out, const uint8_t* p, size_t length)
{
    if (length == 0)
        return;
    const size_t n = length - 1;
    if (n < 60) {
        out.push_back(uint8_t(n << 2));
    } else {
        const size_t bytes = n < 0x100 ? 1 : n < 0x10000 ? 2 : n < 0x1000000 ? 3 : 4;
        out.push_back(uint8_t((59 + bytes) << 2));
        for (size_t i = 0; i < bytes; ++i)
            out.push_back(uint8_t(n >> (8 * i)));
    }
    out.insert(out.end(), p, p + length);
}

void putCopy(std::vector<uint8_t>& out, size_t offset, size_t length)
{
    auto copy2 = [&](size_t n) {
        out.push_back(uint8_t(2 | ((n - 1) << 2)));
        out.push_back(uint8_t(offset));
        out.push_back(uint8_t(offset >> 8));
    };
    while (length >= 68) {
        copy2(64);
        length -= 64;
    }
    if (length > 64) {
        copy2(60);
        length -= 60;
    }
    if (length < 12 && offset < 2048) {
        out.push_back(uint8_t(1 | ((length - 4) << 2) | ((offset >> 8) << 5)));
        out.push_back(uint8_t(offset));
        return;
    }
    copy2(length);
}

} // namespace

std::optional<size_t> snappyLength(const uint8_t* data, size_t size)
{
    const uint8_t* p = data;
    return varint(p, data + size);
}

bool snappyDecompress(const uint8_t* data, size_t size, uint8_t* out, size_t outSize)
{
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    const std::optional<size_t> declared = varint(p, end);
    if (!declared || *declared != outSize)
        return false;
    uint8_t* o = out;
    uint8_t* const oend = out + outSize;
    while (p < end) {
        const uint8_t tag = *p++;
        size_t length = 0;
        size_t offset = 0;
        switch (tag & 3) {
        case 0: {
            length = tag >> 2;
            if (length >= 60) {
                const size_t bytes = length - 59;
                if (size_t(end - p) < bytes)
                    return false;
                length = littleEndian(p, bytes);
                p += bytes;
            }
            length += 1;
            if (size_t(end - p) < length || size_t(oend - o) < length)
                return false;
            std::memcpy(o, p, length);
            o += length;
            p += length;
            continue;
        }
        case 1:
            if (p == end)
                return false;
            length = 4 + ((tag >> 2) & 7);
            offset = size_t(tag >> 5) << 8 | *p++;
            break;
        case 2:
            if (end - p < 2)
                return false;
            length = size_t(tag >> 2) + 1;
            offset = littleEndian(p, 2);
            p += 2;
            break;
        default:
            if (end - p < 4)
                return false;
            length = size_t(tag >> 2) + 1;
            offset = littleEndian(p, 4);
            p += 4;
            break;
        }
        if (offset == 0 || offset > size_t(o - out) || size_t(oend - o) < length)
            return false;
        const uint8_t* from = o - offset;
        if (offset >= length) {
            std::memcpy(o, from, length);
        } else {
            for (size_t i = 0; i < length; ++i)
                o[i] = from[i];
        }
        o += length;
    }
    return o == oend;
}

std::vector<uint8_t> snappyCompress(const uint8_t* data, size_t size)
{
    constexpr int kHashBits = 14;
    constexpr size_t kMaxOffset = 65535;
    std::vector<uint8_t> out;
    out.reserve(size / 2 + 16);
    putVarint(out, size);
    // Positions plus one, so zero is empty.
    std::vector<uint32_t> table(size_t(1) << kHashBits, 0);
    size_t literal = 0;
    size_t i = 0;
    while (size >= 4 && i + 4 <= size) {
        const uint32_t v = load32(data + i);
        const uint32_t h = (v * 0x1e35a7bdu) >> (32 - kHashBits);
        const size_t candidate = table[h];
        table[h] = uint32_t(i + 1);
        if (candidate && i - (candidate - 1) <= kMaxOffset && load32(data + candidate - 1) == v) {
            const size_t from = candidate - 1;
            size_t length = 4;
            while (i + length < size && data[from + length] == data[i + length])
                ++length;
            putLiteral(out, data + literal, i - literal);
            putCopy(out, i - from, length);
            i += length;
            literal = i;
            continue;
        }
        i += 1 + ((i - literal) >> 5);
    }
    putLiteral(out, data + literal, size - literal);
    return out;
}

} // namespace azl
