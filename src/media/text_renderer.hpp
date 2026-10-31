#pragma once

#include "document/text.hpp"
#include "media/video_decoder.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace azl {

// Rasterises text clips on its own thread (CoreText on Apple, DirectWrite on Windows), once per layout, so the render
// thread never waits for text layout. A font that is not installed falls back to the system font with a logged
// warning.
class TextRenderer {
public:
    // Text sizes are fractions of `height`, the composition's height in pixels.
    TextRenderer(MediaDevice* device, uint32_t height);
    ~TextRenderer();

    // Any thread, never blocks: rasterises the layouts not done yet and drops the rest.
    void want(std::set<TextLayout> layouts);
    // The layout's picture, or null until it is rasterised. Never blocks unless `wait`, which waits until it is.
    VideoFrameRef frame(const TextLayout& layout, bool wait);
    // The font names that were not installed and fell back to the system font.
    std::set<std::string> missingFonts() const;

    // Blocking, on any thread: the picture of one layout.
    static VideoFrameRef rasterise(MediaDevice* device, uint32_t height, const TextLayout& layout,
                                   std::set<std::string>& missingFonts);

private:
    void run();

#ifdef __APPLE__
    MediaDevice* device() const { return device_.get(); }
    NS::SharedPtr<MTL::Device> device_;
#else
    MediaDevice* device() const { return nullptr; }
#endif
    uint32_t height_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::set<TextLayout> wanted_;
    std::map<TextLayout, VideoFrameRef> done_;
    std::set<std::string> missing_;
    bool running_ = true;
    std::thread thread_;
};

} // namespace azl
