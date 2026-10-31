#include "media/ffmpeg_util.hpp"

#include <cstddef>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace azl::ff {

#ifdef _WIN32

namespace {

constexpr int kBufferBytes = 64 * 1024;

int readFile(void* opaque, uint8_t* into, int size)
{
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(opaque), into, DWORD(size), &read, nullptr))
        return AVERROR(EIO);
    return read == 0 ? AVERROR_EOF : int(read);
}

int64_t seekFile(void* opaque, int64_t offset, int whence)
{
    const HANDLE file = static_cast<HANDLE>(opaque);
    LARGE_INTEGER at{};
    if (whence & AVSEEK_SIZE)
        return GetFileSizeEx(file, &at) ? at.QuadPart : AVERROR(EIO);
    const DWORD method = (whence & 3) == SEEK_CUR ? FILE_CURRENT : (whence & 3) == SEEK_END ? FILE_END : FILE_BEGIN;
    LARGE_INTEGER distance{};
    distance.QuadPart = offset;
    return SetFilePointerEx(file, distance, &at, method) ? at.QuadPart : AVERROR(EIO);
}

// The FFmpeg reader over `file`, which it owns from then on; null when FFmpeg refuses the memory.
AVIOContext* readerOver(HANDLE file)
{
    auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferBytes));
    AVIOContext* io =
        buffer ? avio_alloc_context(buffer, kBufferBytes, 0, file, readFile, nullptr, seekFile) : nullptr;
    if (!io) {
        av_free(buffer);
        CloseHandle(file);
    }
    return io;
}

void closeReader(AVIOContext* io)
{
    if (!io)
        return;
    CloseHandle(static_cast<HANDLE>(io->opaque));
    av_freep(&io->buffer);
    avio_context_free(&io);
}

} // namespace

void FormatFree::operator()(AVFormatContext* f) const
{
    AVIOContext* custom = f && (f->flags & AVFMT_FLAG_CUSTOM_IO) ? f->pb : nullptr;
    avformat_close_input(&f);
    closeReader(custom);
}

Format openFormat(const std::filesystem::path& file, int& error)
{
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = AVERROR(ENOENT);
        return nullptr;
    }
    AVIOContext* io = readerOver(handle);
    AVFormatContext* raw = io ? avformat_alloc_context() : nullptr;
    if (!raw) {
        closeReader(io);
        error = AVERROR(ENOMEM);
        return nullptr;
    }
    raw->pb = io;
    raw->flags |= AVFMT_FLAG_CUSTOM_IO;
    const std::u8string name = file.u8string();
    error = avformat_open_input(&raw, reinterpret_cast<const char*>(name.c_str()), nullptr, nullptr);
    if (error < 0) {
        closeReader(io);
        return nullptr;
    }
    return Format(raw);
}

#else

void FormatFree::operator()(AVFormatContext* f) const
{
    avformat_close_input(&f);
}

// Paths are UTF-8, which FFmpeg's file protocol expects.
Format openFormat(const std::filesystem::path& file, int& error)
{
    AVFormatContext* raw = nullptr;
    const std::u8string path = file.u8string();
    error = avformat_open_input(&raw, reinterpret_cast<const char*>(path.c_str()), nullptr, nullptr);
    return Format(error < 0 ? nullptr : raw);
}

#endif

namespace {

bool fits(const AVFrame* frame, AVPixelFormat format, int width, int height)
{
    return frame && frame->format == format && frame->width == width && frame->height == height;
}

} // namespace

std::vector<uint8_t> scalePacked(SwsContext* scale, const uint8_t* const data[], const int linesize[], int sourceHeight,
                                 AVPixelFormat format, int width, int height, Frame& padded)
{
    if (!fits(padded.get(), format, width, height)) {
        padded.reset(av_frame_alloc());
        if (!padded)
            return {};
        padded->format = format;
        padded->width = width;
        padded->height = height;
        if (av_frame_get_buffer(padded.get(), 0) < 0) {
            padded.reset();
            return {};
        }
    }
    if (sws_scale(scale, data, linesize, 0, sourceHeight, padded->data, padded->linesize) <= 0)
        return {};
    const size_t row = size_t(width) * 4;
    std::vector<uint8_t> out(row * size_t(height));
    for (int y = 0; y < height; ++y)
        std::memcpy(out.data() + size_t(y) * row, padded->data[0] + ptrdiff_t(y) * padded->linesize[0], row);
    return out;
}

} // namespace azl::ff
