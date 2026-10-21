#pragma once

#include "output/return_video.hpp"

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace azl {

// Encodes pictures to H.264 with VideoToolbox, on the machine's hardware encoder where it has one: real time, no frame
// reordering, the bit rate capped, a key frame at least every return_video::kKeySeconds. Pictures are BGRA in the
// encoder's own IOSurface-backed buffers, BT.709. One thread at a time.
class H264Encoder {
public:
    // Throws std::runtime_error when no session can be made.
    H264Encoder(uint32_t width, uint32_t height, double fps, uint32_t kbps);
    ~H264Encoder();

    // A buffer from the encoder's pool, retained for the caller; null when none is free.
    CVPixelBufferRef newBuffer();
    // Encodes the picture and waits for it, off the render thread; null when the encoder dropped it.
    std::optional<return_video::Packet> encode(CVPixelBufferRef pixels, double seconds, bool key);
    // Whether it runs in hardware, once known.
    std::optional<bool> hardware() const { return hardware_; }

private:
    struct Session;
    std::unique_ptr<Session> session_;
    std::optional<bool> hardware_;
};

// Decodes return video packets to BGRA pictures (rows of width * 4 bytes). One thread at a time.
class H264Decoder {
public:
    struct Picture {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> bgra;
    };

    H264Decoder();
    ~H264Decoder();

    // Null until a key frame with its parameter sets arrives, and for a packet that does not decode.
    std::optional<Picture> decode(const return_video::Packet& packet);

private:
    struct Session;
    std::unique_ptr<Session> session_;
};

} // namespace azl
