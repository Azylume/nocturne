#include "platform/preview_channel.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

namespace azl {

namespace {

// The start of every mapping; the rows follow at kPixels.
struct Header {
    std::atomic<uint32_t> magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t reserved;
    std::atomic<int64_t> frame;
};
static_assert(std::atomic<int64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free,
              "the header is shared between processes");

constexpr uint32_t kMagic = 0x3156504e; // "NPV1"
constexpr uint32_t kBgra8 = 1;
constexpr size_t kPixels = 64;

// Session-local, so a UI and an engine of the same logged-in user meet.
std::wstring mappingName(uint64_t id)
{
    return L"Local\\NocturnePreview-" + std::to_wstring(id);
}

// Unique on this machine while this process runs.
uint64_t nextId()
{
    static std::atomic<uint32_t> counter{0};
    return uint64_t(GetCurrentProcessId()) << 32 | ++counter;
}

struct Mapping {
    HANDLE handle = nullptr;
    void* view = nullptr;

    ~Mapping()
    {
        if (view)
            UnmapViewOfFile(view);
        if (handle)
            CloseHandle(handle);
    }
    Header& header() const { return *static_cast<Header*>(view); }
    uint8_t* pixels() const { return static_cast<uint8_t*>(view) + kPixels; }
};

} // namespace

struct PreviewChannelWriter::Impl {
    Mapping mapping;
    uint64_t id = 0;
};

PreviewChannelWriter::PreviewChannelWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PreviewChannelWriter::~PreviewChannelWriter() = default;

std::unique_ptr<PreviewChannelWriter> PreviewChannelWriter::create(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->id = nextId();
    const uint32_t stride = width * 4;
    const uint64_t size = kPixels + uint64_t(stride) * height;
    impl->mapping.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(size >> 32),
                                              DWORD(size), mappingName(impl->id).c_str());
    if (!impl->mapping.handle || GetLastError() == ERROR_ALREADY_EXISTS)
        return nullptr;
    impl->mapping.view = MapViewOfFile(impl->mapping.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!impl->mapping.view)
        return nullptr;
    Header& header = impl->mapping.header();
    header.width = width;
    header.height = height;
    header.stride = stride;
    header.format = kBgra8;
    header.frame.store(-1, std::memory_order_relaxed);
    header.magic.store(kMagic, std::memory_order_release);
    return std::unique_ptr<PreviewChannelWriter>(new PreviewChannelWriter(std::move(impl)));
}

uint64_t PreviewChannelWriter::id() const
{
    return impl_->id;
}

uint32_t PreviewChannelWriter::width() const
{
    return impl_->mapping.header().width;
}

uint32_t PreviewChannelWriter::height() const
{
    return impl_->mapping.header().height;
}

size_t PreviewChannelWriter::stride() const
{
    return impl_->mapping.header().stride;
}

uint8_t* PreviewChannelWriter::begin()
{
    impl_->mapping.header().frame.store(-1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    return impl_->mapping.pixels();
}

void PreviewChannelWriter::finish(int64_t frame)
{
    impl_->mapping.header().frame.store(frame, std::memory_order_release);
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
    Mapping mapping;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
};

PreviewChannelReader::PreviewChannelReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PreviewChannelReader::~PreviewChannelReader() = default;

// The mapping comes from another process: its header is checked against the size actually mapped before any row is
// read.
std::unique_ptr<PreviewChannelReader> PreviewChannelReader::open(uint64_t id)
{
    auto impl = std::make_unique<Impl>();
    impl->mapping.handle = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName(id).c_str());
    if (!impl->mapping.handle)
        return nullptr;
    impl->mapping.view = MapViewOfFile(impl->mapping.handle, FILE_MAP_READ, 0, 0, 0);
    if (!impl->mapping.view)
        return nullptr;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(impl->mapping.view, &region, sizeof region) || region.RegionSize < kPixels)
        return nullptr;
    const Header& header = impl->mapping.header();
    if (header.magic.load(std::memory_order_acquire) != kMagic ||
        header.format != kBgra8 || header.width == 0 || header.height == 0 ||
        header.stride < uint64_t(header.width) * 4 ||
        kPixels + uint64_t(header.stride) * header.height > region.RegionSize)
        return nullptr;
    impl->width = header.width;
    impl->height = header.height;
    impl->stride = header.stride;
    return std::unique_ptr<PreviewChannelReader>(new PreviewChannelReader(std::move(impl)));
}

bool PreviewChannelReader::read(Picture& picture) const
{
    const Header& header = impl_->mapping.header();
    const int64_t before = header.frame.load(std::memory_order_acquire);
    if (before < 0)
        return false;
    picture.width = impl_->width;
    picture.height = impl_->height;
    const size_t row = size_t(picture.width) * 4;
    picture.bgra.resize(row * picture.height);
    const uint8_t* base = impl_->mapping.pixels();
    for (uint32_t y = 0; y < picture.height; ++y)
        std::memcpy(picture.bgra.data() + y * row, base + size_t(y) * impl_->stride, row);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (header.frame.load(std::memory_order_relaxed) != before)
        return false;
    picture.frame = uint64_t(before);
    return true;
}

} // namespace azl
