#include "media/text_renderer.hpp"

#include <chrono>
#include <cstdio>
#include <optional>
#include <utility>

namespace azl {

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(10);

} // namespace

TextRenderer::~TextRenderer()
{
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        changed_.notify_all();
    }
    thread_.join();
}

void TextRenderer::want(std::set<TextLayout> layouts)
{
    std::lock_guard lock(mutex_);
    wanted_ = std::move(layouts);
    changed_.notify_all();
}

VideoFrameRef TextRenderer::frame(const TextLayout& layout, bool wait)
{
    std::unique_lock lock(mutex_);
    if (wait)
        changed_.wait_for(lock, kWaitTimeout, [&] { return done_.contains(layout) || !wanted_.contains(layout); });
    auto it = done_.find(layout);
    return it == done_.end() ? nullptr : it->second;
}

std::set<std::string> TextRenderer::missingFonts() const
{
    std::lock_guard lock(mutex_);
    return missing_;
}

void TextRenderer::run()
{
    std::unique_lock lock(mutex_);
    while (true) {
        auto pending = [&]() -> std::optional<TextLayout> {
            for (const TextLayout& layout : wanted_)
                if (!done_.contains(layout))
                    return layout;
            return std::nullopt;
        };
        changed_.wait(lock, [&] { return !running_ || pending(); });
        if (!running_)
            return;
        std::erase_if(done_, [&](const auto& entry) { return !wanted_.contains(entry.first); });
        const TextLayout layout = *pending();
        lock.unlock();
        std::set<std::string> missing;
        VideoFrameRef frame = rasterise(device(), height_, layout, missing);
        lock.lock();
        for (const std::string& font : missing)
            if (missing_.insert(font).second)
                std::fprintf(stderr, "nocturne: font \"%s\" is not installed; text uses the system font\n",
                             font.c_str());
        if (wanted_.contains(layout))
            done_[layout] = std::move(frame);
        changed_.notify_all();
    }
}

} // namespace azl
