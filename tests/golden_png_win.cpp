#include "golden.hpp"

#include <wincodec.h>
#include <windows.h>

#include <filesystem>

// PNG files through WIC, as golden.cpp does through ImageIO on Apple: images hold premultiplied RGBA, files plain RGBA.

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com()
    {
        if (p)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

struct ComInit {
    ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { CoUninitialize(); }
};

bool makeFactory(Com<IWICImagingFactory>& factory)
{
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory)));
}

} // namespace

void savePng(azl::Image image, const std::string& path)
{
    ComInit com;
    Com<IWICImagingFactory> factory;
    Com<IWICBitmap> bitmap;
    Com<IWICFormatConverter> plain;
    Com<IWICStream> stream;
    Com<IWICBitmapEncoder> encoder;
    Com<IWICBitmapFrameEncode> frame;
    const UINT stride = image.width * 4;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    const std::wstring file = std::filesystem::path(path).wstring();
    if (!makeFactory(factory) ||
        FAILED(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppPRGBA, stride,
                                               UINT(image.rgba.size()), image.rgba.data(), &bitmap)) ||
        FAILED(factory->CreateFormatConverter(&plain)) ||
        FAILED(plain->Initialize(bitmap.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom)) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(image.width, image.height)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(plain.p, nullptr)) || FAILED(frame->Commit()))
        return;
    encoder->Commit();
}

std::optional<azl::Image> loadPng(const std::string& path)
{
    ComInit com;
    Com<IWICImagingFactory> factory;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> premultiplied;
    const std::wstring file = std::filesystem::path(path).wstring();
    UINT width = 0, height = 0;
    if (!makeFactory(factory) ||
        FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        FAILED(factory->CreateFormatConverter(&premultiplied)) ||
        FAILED(premultiplied->Initialize(frame.p, GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
        return std::nullopt;
    azl::Image image{width, height, {}};
    image.rgba.resize(size_t(width) * height * 4);
    if (FAILED(premultiplied->CopyPixels(nullptr, width * 4, UINT(image.rgba.size()), image.rgba.data())))
        return std::nullopt;
    return image;
}
