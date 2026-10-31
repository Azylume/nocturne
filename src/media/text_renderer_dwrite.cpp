#include "media/text_renderer.hpp"

#include "media/picture_upload.hpp"

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace azl {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kMaxSide = 8192;
constexpr const wchar_t* kSystemFont = L"Segoe UI";

std::wstring wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(std::max(0, length)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), length);
    return out;
}

struct Factories {
    ComPtr<IDWriteFactory> write;
    ComPtr<ID2D1Factory> d2d;
};

// Shared by every thread: both factories are created thread-safe.
const Factories& factories()
{
    static const Factories shared = [] {
        Factories f;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(f.write.GetAddressOf()));
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, f.d2d.GetAddressOf());
        return f;
    }();
    return shared;
}

bool sameName(const std::wstring& a, const std::wstring& b)
{
    return !a.empty() && CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

bool hasName(IDWriteLocalizedStrings* names, const std::wstring& name)
{
    for (UINT32 i = 0; names && i < names->GetCount(); ++i) {
        UINT32 length = 0;
        names->GetStringLength(i, &length);
        std::wstring text(length + 1, L'\0');
        names->GetString(i, text.data(), length + 1);
        text.resize(length);
        if (sameName(text, name))
            return true;
    }
    return false;
}

bool hasInformationalName(IDWriteFont* font, DWRITE_INFORMATIONAL_STRING_ID id, const std::wstring& name)
{
    ComPtr<IDWriteLocalizedStrings> names;
    BOOL exists = FALSE;
    return SUCCEEDED(font->GetInformationalStrings(id, names.GetAddressOf(), &exists)) && exists &&
           hasName(names.Get(), name);
}

// The installed font with this full, PostScript or family name (the family's regular face); null when none has it.
ComPtr<IDWriteFont> installedFont(const std::wstring& name)
{
    ComPtr<IDWriteFontCollection> fonts;
    factories().write->GetSystemFontCollection(fonts.GetAddressOf());
    if (!fonts)
        return nullptr;
    for (UINT32 f = 0; f < fonts->GetFontFamilyCount(); ++f) {
        ComPtr<IDWriteFontFamily> family;
        fonts->GetFontFamily(f, family.GetAddressOf());
        for (UINT32 i = 0; family && i < family->GetFontCount(); ++i) {
            ComPtr<IDWriteFont> font;
            family->GetFont(i, font.GetAddressOf());
            if (font && (hasInformationalName(font.Get(), DWRITE_INFORMATIONAL_STRING_FULL_NAME, name) ||
                         hasInformationalName(font.Get(), DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, name)))
                return font;
        }
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    if (SUCCEEDED(fonts->FindFamilyName(name.c_str(), &index, &exists)) && exists &&
        SUCCEEDED(fonts->GetFontFamily(index, family.GetAddressOf())))
        family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     font.GetAddressOf());
    return font;
}

// The font Windows draws in place of `name` (FontSubstitutes: Helvetica is Arial), which GDI applies and DirectWrite
// does not; empty when it names none.
std::wstring substituteOf(const std::wstring& name)
{
    wchar_t value[LF_FACESIZE] = {};
    DWORD bytes = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
                     name.c_str(), RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
        return {};
    return value;
}

// The installed font of that name, else the one Windows substitutes for it; null when neither is installed.
ComPtr<IDWriteFont> findFont(const std::wstring& name)
{
    if (ComPtr<IDWriteFont> font = installedFont(name))
        return font;
    const std::wstring substitute = substituteOf(name);
    return substitute.empty() ? nullptr : installedFont(substitute);
}

// Looking a name up walks every installed font, so each name is looked up once per process.
ComPtr<IDWriteFont> createFont(const std::string& name, bool& missing)
{
    static std::mutex mutex;
    static std::map<std::string, ComPtr<IDWriteFont>> found;
    static const ComPtr<IDWriteFont> system = installedFont(kSystemFont);
    std::lock_guard lock(mutex);
    auto it = found.find(name);
    if (it == found.end())
        it = found.emplace(name, findFont(wide(name))).first;
    if (it->second)
        return it->second;
    missing = true;
    return system;
}

std::wstring familyName(IDWriteFont* font)
{
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(font->GetFontFamily(family.GetAddressOf())) || FAILED(family->GetFamilyNames(names.GetAddressOf())))
        return kSystemFont;
    UINT32 index = 0;
    BOOL exists = FALSE;
    names->FindLocaleName(L"en-us", &index, &exists);
    UINT32 length = 0;
    names->GetStringLength(exists ? index : 0, &length);
    std::wstring text(length + 1, L'\0');
    names->GetString(exists ? index : 0, text.data(), length + 1);
    text.resize(length);
    return text;
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

// One glyph run's outline, with its pen position relative to the line's baseline origin.
struct Run {
    ComPtr<ID2D1PathGeometry> outline;
    float x = 0.0f;
    float y = 0.0f;
};

// Collects a layout's glyph runs as outlines; draws nothing itself.
class OutlineCollector final : public IDWriteTextRenderer {
public:
    OutlineCollector(float baseline, std::vector<Run>& runs) : baseline_(baseline), runs_(runs) {}

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE,
                                           const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) noexcept override
    {
        Run r{nullptr, x, y - baseline_};
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factories().d2d->CreatePathGeometry(r.outline.GetAddressOf())) ||
            FAILED(r.outline->Open(sink.GetAddressOf())))
            return E_FAIL;
        const HRESULT drawn = run->fontFace->GetGlyphRunOutline(
            run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets, run->glyphCount,
            run->isSideways, run->bidiLevel % 2, sink.Get());
        sink->Close();
        if (SUCCEEDED(drawn))
            runs_.push_back(std::move(r));
        return drawn;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*,
                                            IUnknown*) noexcept override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*,
                                                IUnknown*) noexcept override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
                                               IUnknown*) noexcept override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept override
    {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept override
    {
        *transform = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) noexcept override
    {
        *pixels = 1.0f;
        return S_OK;
    }
    // Lives on the stack for one Draw call: the counts are for COM's sake only.
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override
    {
        if (id == __uuidof(IDWriteTextRenderer) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IUnknown)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

private:
    float baseline_;
    std::vector<Run>& runs_;
};

struct Line {
    std::vector<Run> runs;
    double width = 0.0;
    // The ink's left and right edge from the line's origin.
    double inkMin = 0.0;
    double inkMax = 0.0;
};

Line makeLine(const std::string& text, IDWriteFont* font, double px)
{
    Line line;
    const std::wstring string = wide(text);
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factories().write->CreateTextFormat(familyName(font).c_str(), nullptr, font->GetWeight(),
                                                   font->GetStyle(), font->GetStretch(), float(px), L"en-us",
                                                   format.GetAddressOf())) ||
        FAILED(factories().write->CreateTextLayout(string.c_str(), UINT32(string.size()), format.Get(), 1e6f, 1e6f,
                                                   layout.GetAddressOf())))
        return line;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    line.width = metrics.width;
    DWRITE_LINE_METRICS lineMetrics{};
    UINT32 lines = 0;
    layout->GetLineMetrics(&lineMetrics, 1, &lines);
    OutlineCollector collector(lineMetrics.baseline, line.runs);
    layout->Draw(nullptr, &collector, 0.0f, 0.0f);
    bool any = false;
    for (const Run& run : line.runs) {
        D2D1_RECT_F bounds{};
        if (FAILED(run.outline->GetBounds(nullptr, &bounds)) || bounds.left > bounds.right)
            continue;
        line.inkMin = any ? std::min(line.inkMin, double(run.x + bounds.left)) : run.x + bounds.left;
        line.inkMax = any ? std::max(line.inkMax, double(run.x + bounds.right)) : run.x + bounds.right;
        any = true;
    }
    return line;
}

struct Origin {
    double x = 0.0;
    // The baseline, from the top of the picture.
    double y = 0.0;
};

// One coverage plane: the lines filled, or stroked `strokeWidth` pixels wide.
std::vector<uint8_t> drawPlane(const std::vector<Line>& lines, const std::vector<Origin>& origins, uint32_t width,
                               uint32_t height, double strokeWidth)
{
    std::vector<uint8_t> plane(size_t(width) * height);
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> white;
    ComPtr<ID2D1StrokeStyle> round;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(imaging.GetAddressOf()))) ||
        FAILED(imaging->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                                     bitmap.GetAddressOf())) ||
        FAILED(factories().d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, target.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), white.GetAddressOf())))
        return plane;
    D2D1_STROKE_STYLE_PROPERTIES stroke = D2D1::StrokeStyleProperties();
    stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
    factories().d2d->CreateStrokeStyle(stroke, nullptr, 0, round.GetAddressOf());
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (size_t i = 0; i < lines.size(); ++i)
        for (const Run& run : lines[i].runs) {
            target->SetTransform(D2D1::Matrix3x2F::Translation(float(origins[i].x + run.x), float(origins[i].y + run.y)));
            if (strokeWidth > 0.0)
                target->DrawGeometry(run.outline.Get(), white.Get(), float(strokeWidth), round.Get());
            else
                target->FillGeometry(run.outline.Get(), white.Get());
        }
    if (FAILED(target->EndDraw()))
        return plane;
    const WICRect all{0, 0, INT(width), INT(height)};
    ComPtr<IWICBitmapLock> lock;
    UINT stride = 0, size = 0;
    BYTE* pixels = nullptr;
    if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, lock.GetAddressOf())) || FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&size, &pixels)))
        return plane;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            plane[size_t(y) * width + x] = pixels[size_t(y) * stride + x * 4 + 3];
    return plane;
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

// COM for the calling thread while it rasterises.
struct ComScope {
    ComScope() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope()
    {
        if (ok)
            CoUninitialize();
    }
    bool ok;
};

} // namespace

VideoFrameRef TextRenderer::rasterise(const MediaDevice* device, uint32_t height, const TextLayout& layout,
                                      std::set<std::string>& missingFonts)
{
    const ComScope com;
    const double px = layout.size * height;
    bool missing = false;
    const ComPtr<IDWriteFont> font = createFont(layout.font, missing);
    if (missing)
        missingFonts.insert(layout.font);
    if (!font)
        return nullptr;
    DWRITE_FONT_METRICS metrics{};
    font->GetMetrics(&metrics);
    const double unit = px / metrics.designUnitsPerEm;
    const double ascent = metrics.ascent * unit;
    const double descent = metrics.descent * unit;
    const double lineHeight = (ascent + descent + metrics.lineGap * unit) * layout.lineSpacing;

    std::vector<Line> lines;
    double blockWidth = 0.0;
    for (const std::string& text : splitLines(layout.text)) {
        lines.push_back(makeLine(text, font.Get(), px));
        blockWidth = std::max(blockWidth, lines.back().width);
    }
    const double blockHeight = lineHeight * double(lines.size() - 1) + ascent + descent;
    const double outline = layout.outline * px;
    const double pad = std::ceil(outline) + 2.0;
    double overhangLeft = 0.0;
    double overhangRight = 0.0;
    for (const Line& line : lines) {
        const double shift = alignShift(layout.align, blockWidth - line.width);
        overhangLeft = std::max(overhangLeft, -(shift + line.inkMin));
        overhangRight = std::max(overhangRight, shift + line.inkMax - blockWidth);
    }
    const double padLeft = pad + std::ceil(overhangLeft);
    const auto width =
        uint32_t(std::min(std::ceil(blockWidth) + padLeft + pad + std::ceil(overhangRight), double(kMaxSide)));
    const auto rows = uint32_t(std::min(std::ceil(blockHeight) + 2.0 * pad, double(kMaxSide)));

    std::vector<Origin> origins;
    for (size_t i = 0; i < lines.size(); ++i)
        origins.push_back({padLeft + alignShift(layout.align, blockWidth - lines[i].width),
                           pad + ascent + double(i) * lineHeight});
    const std::vector<uint8_t> fill = drawPlane(lines, origins, width, rows, 0.0);
    const std::vector<uint8_t> stroke =
        outline > 0.0 ? drawPlane(lines, origins, width, rows, 2.0 * outline) : std::vector<uint8_t>(fill.size());
    auto rg = std::make_shared<std::vector<uint8_t>>(fill.size() * 2);
    for (size_t i = 0; i < fill.size(); ++i) {
        (*rg)[2 * i] = fill[i];
        (*rg)[2 * i + 1] = stroke[i];
    }

    auto picture = std::make_shared<DecodedPicture>();
    picture->planes[0] = {PlaneFormat::RG8, int(width), int(rows), size_t(width) * 2, rg->data()};
    picture->planeCount = 1;
    picture->storage = std::move(rg);
    auto frame = std::make_shared<VideoFrame>();
    if (device) {
        auto texture = std::make_shared<gpu::Texture>(uploadPicture(*device, *picture, true)[0]);
        if (!*texture)
            return nullptr;
        frame->luma = *texture;
        frame->resources = std::move(texture);
    } else {
        frame->picture = std::move(picture);
    }
    frame->text = VideoFrame::Text{{float(-padLeft - std::round(alignShift(layout.align, blockWidth))),
                                    float(-pad - std::round(blockHeight / 2.0)), float(width), float(rows)},
                                   float(layout.size)};
    return frame;
}

TextRenderer::TextRenderer(const MediaDevice* device, uint32_t height)
    : device_(device ? *device : MediaDevice{}), height_(height), thread_([this] { run(); })
{
}

} // namespace azl
