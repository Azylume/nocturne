#include "app/safe_mode.hpp"

#include "app/engine_main.hpp"
#include "app/main_thread.hpp"
#include "app/safe_mode_protocol.hpp"
#include "platform/main_loop.hpp"
#include "platform/win_monitors.hpp"
#include "platform/win_output_window.hpp"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace azl {

namespace {

struct Picture {
    uint32_t width = 0;
    uint32_t height = 0;
    // Premultiplied BGRA, top row first.
    std::vector<uint8_t> pixels;
};

// The image through the Windows Imaging Component; empty when it cannot be read.
Picture readPicture(const std::filesystem::path& file)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICBitmapSource> converted;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, frame.Get(), &converted)))
        return {};
    Picture picture;
    if (FAILED(converted->GetSize(&picture.width, &picture.height)))
        return {};
    picture.pixels.resize(size_t(picture.width) * picture.height * 4);
    if (FAILED(converted->CopyPixels(nullptr, picture.width * 4, UINT(picture.pixels.size()), picture.pixels.data())))
        return {};
    return picture;
}

// The picture scaled to fit the area without distortion, centred.
void drawPicture(HDC dc, const Picture& picture, int width, int height)
{
    const double scale = std::min(double(width) / picture.width, double(height) / picture.height);
    const int w = int(picture.width * scale);
    const int h = int(picture.height * scale);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = LONG(picture.width);
    info.bmiHeader.biHeight = -LONG(picture.height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, (width - w) / 2, (height - h) / 2, w, h, 0, 0, int(picture.width), int(picture.height),
                  picture.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}

// Black, the picture if there is one, and the reason along the bottom in grey.
void paint(HDC dc, int width, int height, const Picture& picture, const std::wstring& text)
{
    RECT all{0, 0, width, height};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (!picture.pixels.empty())
        drawPicture(dc, picture, width, height);
    const int dpi = int(GetDpiForWindow(WindowFromDC(dc)));
    HFONT font = CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, RGB(153, 153, 153));
    SetBkMode(dc, TRANSPARENT);
    const int margin = MulDiv(16, dpi, 96);
    RECT label{margin, height - MulDiv(72, dpi, 96), width - margin, height - MulDiv(12, dpi, 96)};
    DrawTextW(dc, text.c_str(), -1, &label, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(font);
}

std::wstring wide(const std::string& text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(size_t(std::max(size, 1) - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

// Where the notice goes: a window centred on the primary monitor when windowed, else every monitor that shows a picture
// of its own; none offscreen.
std::vector<win::Area> noticeAreas(bool windowed, bool offscreen)
{
    std::vector<win::Area> areas;
    if (offscreen)
        return areas;
    for (const win::Monitor& m : win::activeMonitors()) {
        const DisplayInfo& d = m.info;
        if (!d.mirrorOf.empty())
            continue;
        if (windowed)
            return {{d.x + int(d.width) / 2 - 320, d.y + int(d.height) / 2 - 180, 640, 360}};
        areas.push_back({d.x, d.y, int(d.width), int(d.height)});
    }
    return areas;
}

} // namespace

int runSafeMode(const app::EngineConfig& opt)
{
    MainLoop* loop = MainLoop::current();
    if (!loop)
        throw std::logic_error("safe mode: the main loop is not running");
    const bool windowed = opt.windowed || opt.virtualDisplays > 0;
    std::printf("safe mode: %s\n", opt.safeMode.c_str());
    std::fflush(stdout);
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    win::usePhysicalPixels();

    auto picture = std::make_shared<Picture>();
    if (!opt.fallbackImage.empty()) {
        *picture = readPicture(opt.fallbackImage);
        if (picture->pixels.empty())
            std::printf("safe mode: cannot read the fallback image %s\n", opt.fallbackImage.c_str());
    }
    const std::wstring text = wide("Nocturne safe mode: " + opt.safeMode);
    std::vector<std::unique_ptr<win::OutputWindow>> windows;
    for (const win::Area& area : noticeAreas(windowed, opt.offscreen)) {
        const auto style = windowed ? win::OutputWindow::Style::Titled : win::OutputWindow::Style::Borderless;
        auto window = std::make_unique<win::OutputWindow>(style, area, "Nocturne safe mode", !windowed, nullptr);
        window->onPaint([picture, text](void* dc, int width, int height) {
            paint(static_cast<HDC>(dc), width, height, *picture, text);
        });
        windows.push_back(std::move(window));
    }
    std::fflush(stdout);

    int exitCode = 0;
    auto protocol = std::make_unique<SafeModeProtocol>(opt, restartRequest(&exitCode, app::stopMain),
                                                       stopRequest(&exitCode, app::stopMain));
    app::Running stopRequests = app::quitOnStopRequests();
    loop->run();
    stopRequests.reset();
    protocol.reset();
    windows.clear();
    CoUninitialize();
    SetThreadExecutionState(ES_CONTINUOUS);
    std::printf("stopped\n");
    return exitCode;
}

} // namespace azl
