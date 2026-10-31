#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace azl {

// A preview picture shared between the engine and a UI on the same machine: one BGRA8 picture in memory both
// processes map (a global IOSurface on Apple, a named file mapping on Windows), named by a number the engine sends in
// its "preview" messages. It holds the index of the frame it shows, -1 while the writer paints it, so a reader takes a
// picture only when the index is the same before and after its copy. Neither side ever waits for the other.

// The engine's side. One thread at a time.
class PreviewChannelWriter {
public:
    // Null when the system refuses the memory.
    static std::unique_ptr<PreviewChannelWriter> create(uint32_t width, uint32_t height);
    ~PreviewChannelWriter();
    PreviewChannelWriter(const PreviewChannelWriter&) = delete;
    PreviewChannelWriter& operator=(const PreviewChannelWriter&) = delete;

    // What a reader opens it by.
    uint64_t id() const;
    uint32_t width() const;
    uint32_t height() const;
    // Bytes from one row of pixels() to the next.
    size_t stride() const;

    // Marks the picture as being painted and returns its rows for the caller to fill, until finish().
    uint8_t* begin();
    // The picture painted since begin() is frame `frame` (0 or more).
    void finish(int64_t frame);
    // begin(), a copy of `bgra` (rows `bgraStride` bytes apart), finish(frame).
    void write(const uint8_t* bgra, size_t bgraStride, int64_t frame);

    struct Impl;

private:
    explicit PreviewChannelWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// A UI's side. One thread at a time.
class PreviewChannelReader {
public:
    struct Picture {
        uint32_t width = 0;
        uint32_t height = 0;
        // Rows of width * 4 bytes.
        std::vector<uint8_t> bgra;
        // Unknown when the writer never names its frames (an older engine on Apple).
        std::optional<uint64_t> frame;
    };

    // Null when no picture of that id exists on this machine.
    static std::unique_ptr<PreviewChannelReader> open(uint64_t id);
    ~PreviewChannelReader();
    PreviewChannelReader(const PreviewChannelReader&) = delete;
    PreviewChannelReader& operator=(const PreviewChannelReader&) = delete;

    // Copies the picture out; false while it is being painted or when it was painted again during the copy.
    bool read(Picture& picture) const;

    struct Impl;

private:
    explicit PreviewChannelReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace azl
