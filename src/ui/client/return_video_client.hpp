#pragma once

#include "output/h264_stream.hpp"
#include "protocol/messages.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace azl::ui {

// The newest picture of one output of the engine's machine, as its return video decoded.
struct VideoTile {
    std::string display;
    uint64_t screen = 0;
    uint64_t frame = 0;
    H264Decoder::Picture picture;
};

// The return video this UI receives ("video" messages, output/return_video.hpp): a decoder per output, keeping each
// output's newest picture. Thread-safe.
class ReturnVideoClient {
public:
    // True when an output has a new picture.
    bool receive(const Message& message);
    std::optional<VideoTile> tile(const std::string& display) const;
    // Received bytes, for the bit rate.
    uint64_t bytes() const;
    void clear();

private:
    struct Output {
        std::unique_ptr<H264Decoder> decoder = std::make_unique<H264Decoder>();
        std::optional<VideoTile> tile;
    };

    mutable std::mutex mutex_;
    std::map<std::string, Output> outputs_;
    uint64_t bytes_ = 0;
};

} // namespace azl::ui
