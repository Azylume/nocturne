#include "ui/client/return_audio_player.hpp"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

bool realAudio()
{
    const char* value = std::getenv("NOCTURNE_REAL_AUDIO");
    return value && std::string(value) == "1";
}

} // namespace

// The player opens its output on the thread return audio arrives on, never the message thread.
TEST_CASE("listen: the default output pulls return audio at 48 kHz from another thread" * doctest::skip(!realAudio()))
{
    std::atomic<size_t> pulled{0};
    std::unique_ptr<azl::ui::ReturnAudioPlayer::Output> output;
    std::thread([&] {
        output = azl::ui::ReturnAudioPlayer::openOutput([&](float* out, size_t count) {
            std::fill(out, out + count, 0.0f);
            pulled += count;
        });
    }).join();
    REQUIRE(output);
    const size_t before = pulled;
    output->start();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const size_t during = pulled - before;
    output.reset();
    const size_t after = pulled;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    MESSAGE(during << " samples pulled in 2 s");
    CHECK(during > 2 * 48000 * 8 / 10);
    CHECK(during < 2 * 48000 * 12 / 10);
    CHECK(pulled == after); // Nothing is pulled once the output is gone.
}
