#include "media/text_renderer.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace azl {

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(10);
constexpr uint32_t kMaxSide = 8192;

template <typename T>
struct CfRelease {
    void operator()(T ref) const
    {
        if (ref)
            CFRelease(ref);
    }
};
template <typename T>
using Cf = std::unique_ptr<std::remove_pointer_t<T>, CfRelease<T>>;

Cf<CFStringRef> cfString(const std::string& s)
{
    return Cf<CFStringRef>(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()), CFIndex(s.size()),
                                                   kCFStringEncodingUTF8, false));
}

bool sameName(CFStringRef a, CFStringRef b)
{
    return a && b && CFStringCompare(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

// CoreText substitutes a default font for an unknown name, so the font it returns must carry the name asked for.
bool hasName(CTFontRef font, CFStringRef name)
{
    const Cf<CFStringRef> postScript(CTFontCopyPostScriptName(font));
    const Cf<CFStringRef> full(CTFontCopyFullName(font));
    const Cf<CFStringRef> family(CTFontCopyFamilyName(font));
    return sameName(postScript.get(), name) || sameName(full.get(), name) || sameName(family.get(), name);
}

Cf<CTFontRef> createFont(const std::string& name, double px, bool& missing)
{
    const Cf<CFStringRef> cfName = cfString(name);
    if (cfName) {
        Cf<CTFontRef> font(CTFontCreateWithName(cfName.get(), px, nullptr));
        if (font && hasName(font.get(), cfName.get()))
            return font;
    }
    missing = true;
    return Cf<CTFontRef>(CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, px, nullptr));
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines(1);
    for (char c : text) {
        if (c == '\n')
            lines.emplace_back();
        else
            lines.back() += c;
    }
    return lines;
}

struct Line {
    Cf<CTLineRef> line;
    double width = 0.0;
    CGRect ink{};
};

Line makeLine(const std::string& text, CTFontRef font)
{
    Cf<CFStringRef> string = cfString(text);
    if (!string)
        string = cfString("");
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
    const void* values[] = {font, kCFBooleanTrue};
    const Cf<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks));
    const Cf<CFAttributedStringRef> attributed(CFAttributedStringCreate(nullptr, string.get(), attributes.get()));
    Line line{Cf<CTLineRef>(CTLineCreateWithAttributedString(attributed.get()))};
    line.width = CTLineGetTypographicBounds(line.line.get(), nullptr, nullptr, nullptr) -
                 CTLineGetTrailingWhitespaceWidth(line.line.get());
    line.ink = CTLineGetImageBounds(line.line.get(), nullptr);
    return line;
}

// One coverage plane: the lines filled, or stroked `strokeWidth` pixels wide.
std::vector<uint8_t> drawPlane(const std::vector<Line>& lines, const std::vector<CGPoint>& origins, uint32_t width,
                               uint32_t height, double strokeWidth)
{
    std::vector<uint8_t> plane(size_t(width) * height);
    CGContextRef context =
        CGBitmapContextCreate(plane.data(), width, height, 8, width, nullptr, CGBitmapInfo(kCGImageAlphaOnly));
    if (!context)
        return plane;
    CGContextSetShouldSmoothFonts(context, false);
    CGContextSetShouldSubpixelQuantizeFonts(context, false);
    CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    CGContextSetRGBStrokeColor(context, 1, 1, 1, 1);
    if (strokeWidth > 0.0) {
        CGContextSetTextDrawingMode(context, kCGTextStroke);
        CGContextSetLineWidth(context, strokeWidth);
        CGContextSetLineJoin(context, kCGLineJoinRound);
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        CGContextSetTextPosition(context, origins[i].x, origins[i].y);
        CTLineDraw(lines[i].line.get(), context);
    }
    CGContextRelease(context);
    return plane;
}

// The next mip level of a two-channel plane: each texel the mean of the 2x2 below it.
std::vector<uint8_t> halved(const std::vector<uint8_t>& rg, uint32_t width, uint32_t height)
{
    const uint32_t w = std::max(1u, width / 2), h = std::max(1u, height / 2);
    std::vector<uint8_t> out(size_t(w) * h * 2);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t c = 0; c < 2; ++c) {
                const uint32_t x0 = std::min(2 * x, width - 1), x1 = std::min(2 * x + 1, width - 1);
                const uint32_t y0 = std::min(2 * y, height - 1), y1 = std::min(2 * y + 1, height - 1);
                const auto at = [&](uint32_t u, uint32_t v) { return uint32_t(rg[(size_t(v) * width + u) * 2 + c]); };
                out[(size_t(y) * w + x) * 2 + c] = uint8_t((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
            }
    return out;
}

double alignShift(TextAlign align, double width)
{
    switch (align) {
    case TextAlign::Left: return 0.0;
    case TextAlign::Center: return width / 2.0;
    case TextAlign::Right: return width;
    }
    return 0.0;
}

} // namespace

VideoFrameRef TextRenderer::rasterise(MTL::Device* device, uint32_t height, const TextLayout& layout,
                                      std::set<std::string>& missingFonts)
{
    const double px = layout.size * height;
    bool missing = false;
    const Cf<CTFontRef> font = createFont(layout.font, px, missing);
    if (missing)
        missingFonts.insert(layout.font);
    const double ascent = CTFontGetAscent(font.get());
    const double descent = CTFontGetDescent(font.get());
    const double lineHeight = (ascent + descent + CTFontGetLeading(font.get())) * layout.lineSpacing;

    std::vector<Line> lines;
    double blockWidth = 0.0;
    for (const std::string& text : splitLines(layout.text)) {
        lines.push_back(makeLine(text, font.get()));
        blockWidth = std::max(blockWidth, lines.back().width);
    }
    const double blockHeight = lineHeight * double(lines.size() - 1) + ascent + descent;
    const double outline = layout.outline * px;
    const double pad = std::ceil(outline) + 2.0;
    double overhangLeft = 0.0;
    double overhangRight = 0.0;
    for (const Line& line : lines) {
        const double shift = alignShift(layout.align, blockWidth - line.width);
        overhangLeft = std::max(overhangLeft, -(shift + CGRectGetMinX(line.ink)));
        overhangRight = std::max(overhangRight, shift + CGRectGetMaxX(line.ink) - blockWidth);
    }
    const double padLeft = pad + std::ceil(overhangLeft);
    const auto width =
        uint32_t(std::min(std::ceil(blockWidth) + padLeft + pad + std::ceil(overhangRight), double(kMaxSide)));
    const auto rows = uint32_t(std::min(std::ceil(blockHeight) + 2.0 * pad, double(kMaxSide)));

    std::vector<CGPoint> origins;
    for (size_t i = 0; i < lines.size(); ++i)
        origins.push_back({padLeft + alignShift(layout.align, blockWidth - lines[i].width),
                           double(rows) - pad - ascent - double(i) * lineHeight});
    const std::vector<uint8_t> fill = drawPlane(lines, origins, width, rows, 0.0);
    const std::vector<uint8_t> stroke =
        outline > 0.0 ? drawPlane(lines, origins, width, rows, 2.0 * outline) : std::vector<uint8_t>(fill.size());
    std::vector<uint8_t> rg(fill.size() * 2);
    for (size_t i = 0; i < fill.size(); ++i) {
        rg[2 * i] = fill[i];
        rg[2 * i + 1] = stroke[i];
    }

    auto desc = NS::RetainPtr(MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRG8Unorm, width, rows, true));
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeManaged);
    auto texture = std::make_shared<NS::SharedPtr<MTL::Texture>>(NS::TransferPtr(device->newTexture(desc.get())));
    if (!*texture)
        return nullptr;
    uint32_t w = width, h = rows;
    for (NS::UInteger level = 0; level < (*texture)->mipmapLevelCount(); ++level) {
        (*texture)->replaceRegion(MTL::Region(0, 0, w, h), level, rg.data(), NS::UInteger(w) * 2);
        rg = halved(rg, w, h);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }

    auto frame = std::make_shared<VideoFrame>();
    frame->luma = texture->get();
    frame->resources = texture;
    frame->text = VideoFrame::Text{{float(-padLeft - std::round(alignShift(layout.align, blockWidth))),
                                    float(-pad - std::round(blockHeight / 2.0)), float(width), float(rows)},
                                   float(layout.size)};
    return frame;
}

TextRenderer::TextRenderer(MTL::Device* device, uint32_t height)
    : device_(NS::RetainPtr(device)), height_(height), thread_([this] { run(); })
{
}

TextRenderer::~TextRenderer()
{
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        changed_.notify_all();
    }
    thread_.join();
}

void TextRenderer::want(std::set<TextLayout> layouts)
{
    std::lock_guard lock(mutex_);
    wanted_ = std::move(layouts);
    changed_.notify_all();
}

VideoFrameRef TextRenderer::frame(const TextLayout& layout, bool wait)
{
    std::unique_lock lock(mutex_);
    if (wait)
        changed_.wait_for(lock, kWaitTimeout, [&] { return done_.contains(layout) || !wanted_.contains(layout); });
    auto it = done_.find(layout);
    return it == done_.end() ? nullptr : it->second;
}

std::set<std::string> TextRenderer::missingFonts() const
{
    std::lock_guard lock(mutex_);
    return missing_;
}

void TextRenderer::run()
{
    std::unique_lock lock(mutex_);
    while (true) {
        auto pending = [&]() -> std::optional<TextLayout> {
            for (const TextLayout& layout : wanted_)
                if (!done_.contains(layout))
                    return layout;
            return std::nullopt;
        };
        changed_.wait(lock, [&] { return !running_ || pending(); });
        if (!running_)
            return;
        std::erase_if(done_, [&](const auto& entry) { return !wanted_.contains(entry.first); });
        const TextLayout layout = *pending();
        lock.unlock();
        std::set<std::string> missing;
        VideoFrameRef frame = rasterise(device_.get(), height_, layout, missing);
        lock.lock();
        for (const std::string& font : missing)
            if (missing_.insert(font).second)
                std::fprintf(stderr, "nocturne: font \"%s\" is not installed; text uses the system font\n",
                             font.c_str());
        if (wanted_.contains(layout))
            done_[layout] = std::move(frame);
        changed_.notify_all();
    }
}

} // namespace azl
