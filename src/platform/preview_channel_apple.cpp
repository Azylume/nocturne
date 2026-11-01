#include "platform/preview_channel.hpp"

#include "platform/preview_channel_apple.hpp"
#include "protocol/preview_surface.hpp"

#include <IOSurface/IOSurface.h>

#include <cstring>

namespace azl {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// The deprecated global flag still works for unsandboxed processes; passing a mach port instead needs a channel the
// protocol does not have.
IOSurfaceRef createPreviewSurface(uint32_t width, uint32_t height)
{
    CFMutableDictionaryRef props =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto number = [&](CFStringRef key, int value) {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &value);
        CFDictionarySetValue(props, key, n);
        CFRelease(n);
    };
    number(kIOSurfaceWidth, int(width));
    number(kIOSurfaceHeight, int(height));
    number(kIOSurfaceBytesPerElement, 4);
    number(kIOSurfacePixelFormat, int('BGRA'));
    CFDictionarySetValue(props, kIOSurfaceIsGlobal, kCFBooleanTrue);
    IOSurfaceRef surface = IOSurfaceCreate(props);
    CFRelease(props);
    return surface;
}
#pragma clang diagnostic pop

void markPreviewSurface(IOSurfaceRef surface, int64_t frame)
{
    CFNumberRef value = CFNumberCreate(nullptr, kCFNumberSInt64Type, &frame);
    IOSurfaceSetValue(surface, previewFrameKey(), value);
    CFRelease(value);
}

namespace {

std::optional<int64_t> frameIn(IOSurfaceRef surface)
{
    CFTypeRef value = IOSurfaceCopyValue(surface, previewFrameKey());
    if (!value)
        return std::nullopt;
    int64_t frame = -1;
    if (CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &frame);
    CFRelease(value);
    return frame;
}

} // namespace

struct PreviewChannelWriter::Impl {
    IOSurfaceRef surface = nullptr;
    ~Impl() { CFRelease(surface); }
};

PreviewChannelWriter::PreviewChannelWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PreviewChannelWriter::~PreviewChannelWriter() = default;

std::unique_ptr<PreviewChannelWriter> PreviewChannelWriter::create(uint32_t width, uint32_t height)
{
    IOSurfaceRef surface = createPreviewSurface(width, height);
    if (!surface)
        return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->surface = surface;
    markPreviewSurface(surface, -1);
    return std::unique_ptr<PreviewChannelWriter>(new PreviewChannelWriter(std::move(impl)));
}

uint64_t PreviewChannelWriter::id() const
{
    return IOSurfaceGetID(impl_->surface);
}

uint32_t PreviewChannelWriter::width() const
{
    return uint32_t(IOSurfaceGetWidth(impl_->surface));
}

uint32_t PreviewChannelWriter::height() const
{
    return uint32_t(IOSurfaceGetHeight(impl_->surface));
}

size_t PreviewChannelWriter::stride() const
{
    return IOSurfaceGetBytesPerRow(impl_->surface);
}

uint8_t* PreviewChannelWriter::begin()
{
    markPreviewSurface(impl_->surface, -1);
    IOSurfaceLock(impl_->surface, 0, nullptr);
    return static_cast<uint8_t*>(IOSurfaceGetBaseAddress(impl_->surface));
}

void PreviewChannelWriter::finish(int64_t frame)
{
    IOSurfaceUnlock(impl_->surface, 0, nullptr);
    markPreviewSurface(impl_->surface, frame);
}

void PreviewChannelWriter::write(const uint8_t* bgra, size_t bgraStride, int64_t frame)
{
    uint8_t* base = begin();
    const size_t row = size_t(width()) * 4;
    for (uint32_t y = 0; y < height(); ++y)
        std::memcpy(base + y * stride(), bgra + y * bgraStride, row);
    finish(frame);
}

struct PreviewChannelReader::Impl {
    IOSurfaceRef surface = nullptr;
    ~Impl() { CFRelease(surface); }
};

PreviewChannelReader::PreviewChannelReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PreviewChannelReader::~PreviewChannelReader() = default;

std::unique_ptr<PreviewChannelReader> PreviewChannelReader::open(uint64_t id)
{
    IOSurfaceRef surface = IOSurfaceLookup(IOSurfaceID(id));
    if (!surface)
        return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->surface = surface;
    return std::unique_ptr<PreviewChannelReader>(new PreviewChannelReader(std::move(impl)));
}

bool PreviewChannelReader::read(Picture& picture) const
{
    IOSurfaceRef surface = impl_->surface;
    const std::optional<int64_t> before = frameIn(surface);
    if (before && *before < 0)
        return false;
    picture.width = uint32_t(IOSurfaceGetWidth(surface));
    picture.height = uint32_t(IOSurfaceGetHeight(surface));
    const size_t row = size_t(picture.width) * 4;
    picture.bgra.resize(row * picture.height);
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
    const size_t stride = IOSurfaceGetBytesPerRow(surface);
    for (uint32_t y = 0; y < picture.height; ++y)
        std::memcpy(picture.bgra.data() + y * row, base + y * stride, row);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    picture.frame.reset();
    if (!before)
        return true;
    if (frameIn(surface) != before)
        return false;
    picture.frame = uint64_t(*before);
    return true;
}

} // namespace azl
