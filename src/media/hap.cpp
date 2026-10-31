#include "media/hap.hpp"

#include "media/snappy.hpp"
#include "platform/work_queue.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include "../../third_party/stb/stb_dxt.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace azl {

namespace {

constexpr uint32_t fourcc(const char (&code)[5])
{
    return uint32_t(uint8_t(code[0])) << 24 | uint32_t(uint8_t(code[1])) << 16 | uint32_t(uint8_t(code[2])) << 8 |
           uint32_t(uint8_t(code[3]));
}

constexpr uint8_t kNone = 0x0a;
constexpr uint8_t kSnappy = 0x0b;
constexpr uint8_t kChunked = 0x0c;
constexpr uint8_t kMultiple = 0x0d;
constexpr uint8_t kInstructions = 0x01;
constexpr uint8_t kChunkCompressors = 0x02;
constexpr uint8_t kChunkSizes = 0x03;
constexpr uint8_t kChunkOffsets = 0x04;

struct Section {
    uint8_t type = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// The section at `p`, within `end`; none when its header or data runs past the end.
std::optional<Section> section(const uint8_t* p, const uint8_t* end)
{
    if (end - p < 4)
        return std::nullopt;
    size_t size = size_t(p[0]) | size_t(p[1]) << 8 | size_t(p[2]) << 16;
    const uint8_t type = p[3];
    p += 4;
    if (size == 0) {
        if (end - p < 4)
            return std::nullopt;
        size = le32(p);
        p += 4;
    }
    if (size_t(end - p) < size)
        return std::nullopt;
    return Section{type, p, size};
}

std::vector<Section> sections(const uint8_t* p, const uint8_t* end, bool& complete)
{
    std::vector<Section> found;
    complete = true;
    while (p < end) {
        const std::optional<Section> s = section(p, end);
        if (!s) {
            complete = false;
            break;
        }
        found.push_back(*s);
        p = s->data + s->size;
    }
    return found;
}

bool knownTexture(uint8_t format)
{
    return format == uint8_t(HapTexture::BC1) || format == uint8_t(HapTexture::BC3) ||
           format == uint8_t(HapTexture::YCoCgBC3) || format == uint8_t(HapTexture::BC4);
}

struct Chunk {
    uint8_t compressor = kNone;
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t outOffset = 0;
    size_t outSize = 0;
};

std::string decodeChunked(const Section& s, size_t expected, std::vector<uint8_t>& out)
{
    const std::optional<Section> instructions = section(s.data, s.data + s.size);
    if (!instructions || instructions->type != kInstructions)
        return "chunked texture without decode instructions";
    const uint8_t* frameData = instructions->data + instructions->size;
    const size_t frameSize = size_t(s.data + s.size - frameData);
    bool complete = false;
    const std::vector<Section> tables =
        sections(instructions->data, instructions->data + instructions->size, complete);
    if (!complete)
        return "truncated decode instructions";
    const Section* compressors = nullptr;
    const Section* sizes = nullptr;
    const Section* offsets = nullptr;
    for (const Section& t : tables) {
        if (t.type == kChunkCompressors)
            compressors = &t;
        else if (t.type == kChunkSizes)
            sizes = &t;
        else if (t.type == kChunkOffsets)
            offsets = &t;
    }
    if (!compressors || !sizes || compressors->size == 0 || sizes->size != compressors->size * 4 ||
        (offsets && offsets->size != sizes->size))
        return "inconsistent chunk tables";
    std::vector<Chunk> chunks(compressors->size);
    size_t at = 0;
    size_t total = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        Chunk& c = chunks[i];
        c.compressor = compressors->data[i];
        c.size = le32(sizes->data + 4 * i);
        const size_t offset = offsets ? le32(offsets->data + 4 * i) : at;
        if (offset > frameSize || frameSize - offset < c.size)
            return "a chunk lies outside the frame";
        c.data = frameData + offset;
        at = offset + c.size;
        if (c.compressor == kSnappy) {
            const std::optional<size_t> length = snappyLength(c.data, c.size);
            if (!length)
                return "a chunk's Snappy header is malformed";
            c.outSize = *length;
        } else if (c.compressor == kNone) {
            c.outSize = c.size;
        } else {
            return "unknown chunk compressor";
        }
        if (c.outSize > expected - total)
            return "chunks hold more than the texture's " + std::to_string(expected) + " bytes";
        c.outOffset = total;
        total += c.outSize;
    }
    if (total != expected)
        return "chunks hold " + std::to_string(total) + " bytes, the texture needs " + std::to_string(expected);
    out.resize(expected);
    std::atomic<bool> failed{false};
    auto decode = [&](size_t i) {
        const Chunk& c = chunks[i];
        if (c.compressor == kNone)
            std::memcpy(out.data() + c.outOffset, c.data, c.size);
        else if (!snappyDecompress(c.data, c.size, out.data() + c.outOffset, c.outSize))
            failed = true;
    };
    if (chunks.size() == 1) {
        decode(0);
    } else {
        parallelFor(chunks.size(), ThreadRole::Decode, decode);
    }
    return failed ? "a chunk's Snappy data is malformed" : "";
}

std::string decodeImage(const Section& s, int width, int height, HapImage& image)
{
    const uint8_t format = s.type & 0x0f;
    const uint8_t compressor = s.type >> 4;
    if (!knownTexture(format))
        return "unsupported texture format " + std::to_string(format);
    image.texture = HapTexture(format);
    const size_t expected = hapTextureBytes(image.texture, width, height);
    switch (compressor) {
    case kNone:
        if (s.size != expected)
            return "uncompressed texture of " + std::to_string(s.size) + " bytes, expected " + std::to_string(expected);
        image.blocks.assign(s.data, s.data + expected);
        return "";
    case kSnappy:
        if (snappyLength(s.data, s.size) != expected)
            return "malformed Snappy data";
        image.blocks.resize(expected);
        return snappyDecompress(s.data, s.size, image.blocks.data(), expected) ? "" : "malformed Snappy data";
    case kChunked:
        return decodeChunked(s, expected, image.blocks);
    }
    return "unknown section type " + std::to_string(s.type);
}

void putHeader(std::vector<uint8_t>& out, size_t size, uint8_t type)
{
    if (size < 0x1000000 && size > 0) {
        out.insert(out.end(), {uint8_t(size), uint8_t(size >> 8), uint8_t(size >> 16), type});
        return;
    }
    out.insert(out.end(), {0, 0, 0, type, uint8_t(size), uint8_t(size >> 8), uint8_t(size >> 16), uint8_t(size >> 24)});
}

void put32(std::vector<uint8_t>& out, size_t value)
{
    out.insert(out.end(), {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)});
}

// A 4x4 block of RGBA at block (bx, by), edge pixels repeated past the picture.
std::array<uint8_t, 64> blockAt(const uint8_t* rgba, int width, int height, int bx, int by)
{
    std::array<uint8_t, 64> block{};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const int sx = std::min(bx * 4 + x, width - 1);
            const int sy = std::min(by * 4 + y, height - 1);
            std::memcpy(&block[size_t(y * 4 + x) * 4], rgba + (size_t(sy) * width + sx) * 4, 4);
        }
    return block;
}

uint16_t pack565(int r5, int g6, int b5)
{
    return uint16_t(r5 << 11 | g6 << 5 | b5);
}

int expand5(int v)
{
    return v << 3 | v >> 2;
}

int expand6(int v)
{
    return v << 2 | v >> 4;
}

// The 5- or 6-bit endpoint value that expands closest to `v`.
int nearest(int v, int bits)
{
    int best = 0;
    for (int k = 1; k < (1 << bits); ++k)
        if (std::abs((bits == 5 ? expand5(k) : expand6(k)) - v) < std::abs((bits == 5 ? expand5(best) : expand6(best)) - v))
            best = k;
    return best;
}

// Scaled YCoCg DXT5 (van Waveren and Castano): Co and Cg, scaled up by 1, 2 or 4 to use the colour endpoints' range,
// in red and green, the scale in blue, luma in alpha.
void encodeYCoCgBlock(const std::array<uint8_t, 64>& rgba, uint8_t* out)
{
    std::array<int, 16> co{}, cg{};
    std::array<uint8_t, 16> luma{};
    int spread = 0;
    for (int i = 0; i < 16; ++i) {
        const int r = rgba[size_t(i) * 4], g = rgba[size_t(i) * 4 + 1], b = rgba[size_t(i) * 4 + 2];
        luma[size_t(i)] = uint8_t((r + 2 * g + b + 2) >> 2);
        co[size_t(i)] = ((2 * r - 2 * b + 2) >> 2);
        cg[size_t(i)] = ((-r + 2 * g - b + 2) >> 2);
        spread = std::max({spread, std::abs(co[size_t(i)]), std::abs(cg[size_t(i)])});
    }
    const int scale = spread < 32 ? (spread < 16 ? 4 : 2) : 1;
    int minCo = 255, maxCo = 0, minCg = 255, maxCg = 0;
    double meanCo = 0, meanCg = 0;
    for (int i = 0; i < 16; ++i) {
        co[size_t(i)] = std::clamp(co[size_t(i)] * scale + 128, 0, 255);
        cg[size_t(i)] = std::clamp(cg[size_t(i)] * scale + 128, 0, 255);
        minCo = std::min(minCo, co[size_t(i)]), maxCo = std::max(maxCo, co[size_t(i)]);
        minCg = std::min(minCg, cg[size_t(i)]), maxCg = std::max(maxCg, cg[size_t(i)]);
        meanCo += co[size_t(i)] / 16.0, meanCg += cg[size_t(i)] / 16.0;
    }
    const int insetCo = (maxCo - minCo) >> 4, insetCg = (maxCg - minCg) >> 4;
    minCo += insetCo, maxCo -= insetCo, minCg += insetCg, maxCg -= insetCg;
    double covariance = 0;
    for (int i = 0; i < 16; ++i)
        covariance += (co[size_t(i)] - meanCo) * (cg[size_t(i)] - meanCg);
    if (covariance < 0)
        std::swap(minCg, maxCg);

    const int blue = scale - 1;
    uint16_t c0 = pack565(nearest(maxCo, 5), nearest(maxCg, 6), blue);
    uint16_t c1 = pack565(nearest(minCo, 5), nearest(minCg, 6), blue);
    if (c0 < c1)
        std::swap(c0, c1);
    const std::array<int, 2> e0{expand5(c0 >> 11), expand6((c0 >> 5) & 63)};
    const std::array<int, 2> e1{expand5(c1 >> 11), expand6((c1 >> 5) & 63)};
    const std::array<std::array<int, 2>, 4> palette{{
        e0,
        e1,
        {(2 * e0[0] + e1[0]) / 3, (2 * e0[1] + e1[1]) / 3},
        {(e0[0] + 2 * e1[0]) / 3, (e0[1] + 2 * e1[1]) / 3},
    }};
    uint32_t indices = 0;
    if (c0 != c1)
        for (int i = 0; i < 16; ++i) {
            int best = 0, bestError = 1 << 30;
            for (int k = 0; k < 4; ++k) {
                const int dco = co[size_t(i)] - palette[size_t(k)][0], dcg = cg[size_t(i)] - palette[size_t(k)][1];
                if (const int error = dco * dco + dcg * dcg; error < bestError)
                    best = k, bestError = error;
            }
            indices |= uint32_t(best) << (2 * i);
        }
    stb_compress_bc4_block(out, luma.data());
    out[8] = uint8_t(c0), out[9] = uint8_t(c0 >> 8), out[10] = uint8_t(c1), out[11] = uint8_t(c1 >> 8);
    for (int i = 0; i < 4; ++i)
        out[12 + i] = uint8_t(indices >> (8 * i));
}

std::vector<uint8_t> encodeTexture(const uint8_t* rgba, int width, int height, HapTexture texture)
{
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    const size_t blockBytes = texture == HapTexture::BC1 || texture == HapTexture::BC4 ? 8 : 16;
    std::vector<uint8_t> out(size_t(bw) * bh * blockBytes);
    uint8_t* const base = out.data();
    auto encodeRow = [&](size_t by) {
        for (int bx = 0; bx < bw; ++bx) {
            const std::array<uint8_t, 64> block = blockAt(rgba, width, height, bx, int(by));
            uint8_t* dest = base + (by * size_t(bw) + size_t(bx)) * blockBytes;
            switch (texture) {
            case HapTexture::BC1: stb_compress_dxt_block(dest, block.data(), 0, STB_DXT_HIGHQUAL); break;
            case HapTexture::BC3: stb_compress_dxt_block(dest, block.data(), 1, STB_DXT_HIGHQUAL); break;
            case HapTexture::YCoCgBC3: encodeYCoCgBlock(block, dest); break;
            case HapTexture::BC4: {
                std::array<uint8_t, 16> alpha{};
                for (size_t i = 0; i < 16; ++i)
                    alpha[i] = block[i * 4 + 3];
                stb_compress_bc4_block(dest, alpha.data());
                break;
            }
            }
        }
    };
    parallelFor(size_t(bh), ThreadRole::Background, encodeRow);
    return out;
}

// One texture as a top-level section: Snappy where it pays off, in `chunks` chunks when more than one.
std::vector<uint8_t> textureSection(const std::vector<uint8_t>& blocks, HapTexture texture, int chunks)
{
    const uint8_t format = uint8_t(texture);
    std::vector<uint8_t> out;
    if (chunks <= 1) {
        const std::vector<uint8_t> packed = snappyCompress(blocks.data(), blocks.size());
        const bool snappy = packed.size() < blocks.size();
        const std::vector<uint8_t>& body = snappy ? packed : blocks;
        putHeader(out, body.size(), uint8_t((snappy ? kSnappy : kNone) << 4 | format));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    std::vector<std::vector<uint8_t>> parts;
    std::vector<uint8_t> compressors;
    for (int i = 0; i < chunks; ++i) {
        const size_t begin = blocks.size() * size_t(i) / size_t(chunks);
        const size_t end = blocks.size() * size_t(i + 1) / size_t(chunks);
        std::vector<uint8_t> packed = snappyCompress(blocks.data() + begin, end - begin);
        const bool snappy = packed.size() < end - begin;
        compressors.push_back(snappy ? kSnappy : kNone);
        parts.push_back(snappy ? std::move(packed) : std::vector<uint8_t>(blocks.begin() + long(begin),
                                                                           blocks.begin() + long(end)));
    }
    std::vector<uint8_t> instructions;
    putHeader(instructions, compressors.size(), kChunkCompressors);
    instructions.insert(instructions.end(), compressors.begin(), compressors.end());
    putHeader(instructions, parts.size() * 4, kChunkSizes);
    for (const std::vector<uint8_t>& part : parts)
        put32(instructions, part.size());
    std::vector<uint8_t> body;
    putHeader(body, instructions.size(), kInstructions);
    body.insert(body.end(), instructions.begin(), instructions.end());
    for (const std::vector<uint8_t>& part : parts)
        body.insert(body.end(), part.begin(), part.end());
    putHeader(out, body.size(), uint8_t(kChunked << 4 | format));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::array<std::array<uint8_t, 4>, 4> colourPalette(const uint8_t* block, bool threeColour)
{
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    auto rgb = [](uint16_t c) {
        return std::array<int, 3>{expand5(c >> 11), expand6((c >> 5) & 63), expand5(c & 31)};
    };
    const std::array<int, 3> a = rgb(c0), b = rgb(c1);
    std::array<std::array<uint8_t, 4>, 4> p{};
    for (int i = 0; i < 3; ++i) {
        p[0][size_t(i)] = uint8_t(a[size_t(i)]);
        p[1][size_t(i)] = uint8_t(b[size_t(i)]);
        if (c0 > c1 || !threeColour) {
            p[2][size_t(i)] = uint8_t((2 * a[size_t(i)] + b[size_t(i)]) / 3);
            p[3][size_t(i)] = uint8_t((a[size_t(i)] + 2 * b[size_t(i)]) / 3);
        } else {
            p[2][size_t(i)] = uint8_t((a[size_t(i)] + b[size_t(i)]) / 2);
        }
    }
    p[0][3] = p[1][3] = p[2][3] = 255;
    p[3][3] = c0 > c1 || !threeColour ? 255 : 0;
    return p;
}

std::array<uint8_t, 16> alphaBlock(const uint8_t* block)
{
    const int a0 = block[0], a1 = block[1];
    std::array<int, 8> levels{a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i)
            levels[size_t(i) + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; ++i)
            levels[size_t(i) + 1] = ((5 - i) * a0 + i * a1) / 5;
        levels[6] = 0, levels[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= uint64_t(block[2 + i]) << (8 * i);
    std::array<uint8_t, 16> out{};
    for (size_t i = 0; i < 16; ++i)
        out[i] = uint8_t(levels[(bits >> (3 * i)) & 7]);
    return out;
}

} // namespace

HapCodec hapCodecOf(uint32_t code)
{
    if (code == fourcc("Hap1"))
        return HapCodec::Hap;
    if (code == fourcc("Hap5"))
        return HapCodec::HapAlpha;
    if (code == fourcc("HapY"))
        return HapCodec::HapQ;
    if (code == fourcc("HapM"))
        return HapCodec::HapQAlpha;
    return HapCodec::None;
}

uint32_t fourccOf(HapCodec codec)
{
    switch (codec) {
    case HapCodec::Hap: return fourcc("Hap1");
    case HapCodec::HapAlpha: return fourcc("Hap5");
    case HapCodec::HapQ: return fourcc("HapY");
    case HapCodec::HapQAlpha: return fourcc("HapM");
    case HapCodec::None: break;
    }
    return 0;
}

size_t hapTextureBytes(HapTexture texture, int width, int height)
{
    const size_t blocks = size_t((width + 3) / 4) * size_t((height + 3) / 4);
    return blocks * (texture == HapTexture::BC1 || texture == HapTexture::BC4 ? 8 : 16);
}

bool pictureSizeOk(int width, int height)
{
    return width > 0 && height > 0 && width <= kMaxPictureSide && height <= kMaxPictureSide;
}

std::string decodeHapFrame(const uint8_t* data, size_t size, int width, int height, std::vector<HapImage>& images)
{
    if (!pictureSizeOk(width, height))
        return "a picture of " + std::to_string(width) + "x" + std::to_string(height);
    const std::optional<Section> top = section(data, data + size);
    if (!top)
        return "truncated HAP frame";
    if (top->data + top->size != data + size)
        return "bytes after the HAP frame's section";
    std::vector<Section> parts{*top};
    if (top->type == kMultiple) {
        bool complete = false;
        parts = sections(top->data, top->data + top->size, complete);
        if (!complete)
            return "truncated image in a multiple-image frame";
        std::erase_if(parts, [](const Section& s) { return !knownTexture(s.type & 0x0f) || (s.type >> 4) < kNone; });
        if (parts.empty() || parts.size() > 2)
            return "a multiple-image frame needs one or two images";
    }
    images.resize(parts.size());
    for (size_t i = 0; i < parts.size(); ++i)
        if (std::string error = decodeImage(parts[i], width, height, images[i]); !error.empty())
            return error;
    if (images.size() == 2 && !(images[0].texture == HapTexture::YCoCgBC3 && images[1].texture == HapTexture::BC4))
        std::swap(images[0], images[1]);
    if (images.size() == 2 && !(images[0].texture == HapTexture::YCoCgBC3 && images[1].texture == HapTexture::BC4))
        return "unsupported image combination";
    return "";
}

std::vector<uint8_t> encodeHapFrame(const uint8_t* rgba, int width, int height, HapCodec codec, int chunks)
{
    switch (codec) {
    case HapCodec::Hap:
        return textureSection(encodeTexture(rgba, width, height, HapTexture::BC1), HapTexture::BC1, chunks);
    case HapCodec::HapAlpha:
        return textureSection(encodeTexture(rgba, width, height, HapTexture::BC3), HapTexture::BC3, chunks);
    case HapCodec::HapQ:
        return textureSection(encodeTexture(rgba, width, height, HapTexture::YCoCgBC3), HapTexture::YCoCgBC3, chunks);
    case HapCodec::HapQAlpha: {
        std::vector<uint8_t> body =
            textureSection(encodeTexture(rgba, width, height, HapTexture::YCoCgBC3), HapTexture::YCoCgBC3, chunks);
        const std::vector<uint8_t> alpha =
            textureSection(encodeTexture(rgba, width, height, HapTexture::BC4), HapTexture::BC4, chunks);
        body.insert(body.end(), alpha.begin(), alpha.end());
        std::vector<uint8_t> out;
        putHeader(out, body.size(), kMultiple);
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    case HapCodec::None: break;
    }
    return {};
}

std::vector<uint8_t> hapToRgba(const std::vector<HapImage>& images, int width, int height)
{
    std::vector<uint8_t> rgba(size_t(width) * height * 4, 255);
    const int bw = (width + 3) / 4;
    for (const HapImage& image : images) {
        const size_t blockBytes = image.texture == HapTexture::BC1 || image.texture == HapTexture::BC4 ? 8 : 16;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const uint8_t* block = image.blocks.data() + (size_t(y / 4) * bw + size_t(x / 4)) * blockBytes;
                const size_t i = size_t((y % 4) * 4 + x % 4);
                uint8_t* out = &rgba[(size_t(y) * width + x) * 4];
                if (image.texture == HapTexture::BC4) {
                    out[3] = alphaBlock(block)[i];
                    continue;
                }
                const uint8_t* colour = blockBytes == 16 ? block + 8 : block;
                const uint32_t indices = le32(colour + 4);
                const auto palette = colourPalette(colour, image.texture == HapTexture::BC1);
                const std::array<uint8_t, 4>& c = palette[(indices >> (2 * i)) & 3];
                if (image.texture == HapTexture::BC1) {
                    std::memcpy(out, c.data(), 3);
                    continue;
                }
                const uint8_t alpha = alphaBlock(block)[i];
                if (image.texture == HapTexture::BC3) {
                    std::memcpy(out, c.data(), 3);
                    out[3] = alpha;
                    continue;
                }
                const double scale = c[2] / 8.0 + 1.0;
                const double co = (c[0] - 128.0) / scale, cg = (c[1] - 128.0) / scale, luma = alpha;
                out[0] = uint8_t(std::clamp(luma + co - cg + 0.5, 0.0, 255.0));
                out[1] = uint8_t(std::clamp(luma + cg + 0.5, 0.0, 255.0));
                out[2] = uint8_t(std::clamp(luma - co - cg + 0.5, 0.0, 255.0));
            }
    }
    return rgba;
}

} // namespace azl
