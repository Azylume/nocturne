#include "engine/transport.hpp"

#include <algorithm>
#include <cmath>

namespace azl {

namespace {

// Positions computed from beats or frame times land a hair below a frame boundary; this keeps them on it.
constexpr double kFrameEpsilon = 1e-6;

uint64_t splitmix(uint64_t& state)
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

uint64_t mixKey(uint64_t a, uint64_t b)
{
    uint64_t state = a ^ (b * 0xD6E8FEB86659FD93ull);
    return splitmix(state);
}

uint32_t keyedUniform(uint64_t key, uint32_t count)
{
    const uint64_t limit = UINT64_MAX - UINT64_MAX % count;
    uint64_t value = 0;
    do
        value = splitmix(key);
    while (value >= limit);
    return uint32_t(value % count);
}

uint64_t layerKey(uint32_t seed, Choice choice, uint64_t layer)
{
    return mixKey(mixKey(seed, uint64_t(choice)), layer);
}

int autoBeats(double seconds)
{
    if (!(seconds > 0.0))
        return 1;
    for (int beats = 1; beats <= (1 << 30); beats *= 2)
        if (60.0 * beats / seconds >= 80.0)
            return beats;
    return 1 << 30;
}

FramePick pickFrame(PlayMode mode, double progress, int frames)
{
    const double x = std::max(0.0, progress) * frames + kFrameEpsilon;
    const int last = frames - 1;
    switch (mode) {
    case PlayMode::Loop:
    case PlayMode::RandomJump:
        return {std::min(int(std::fmod(x, frames)), last)};
    case PlayMode::Reverse:
        return {std::clamp(int(std::ceil(frames - std::fmod(x, frames))) - 1, 0, last), false};
    case PlayMode::Bounce: {
        const double m = std::fmod(x, 2.0 * frames);
        if (m < frames)
            return {std::min(int(m), last)};
        return {std::clamp(int(std::ceil(2.0 * frames - m)) - 1, 0, last), false};
    }
    case PlayMode::OnceClear:
        return {std::min(int(x), last), true, x < frames};
    case PlayMode::OnceHold:
        return {std::min(int(x), last)};
    }
    return {};
}

double clipPhase(PlayMode mode, double progress)
{
    switch (mode) {
    case PlayMode::Loop:
    case PlayMode::RandomJump:
        return progress - std::floor(progress);
    case PlayMode::Reverse:
        return 1.0 - (progress - std::floor(progress));
    case PlayMode::Bounce: {
        const double m = std::fmod(progress, 2.0);
        return m < 1.0 ? m : 2.0 - m;
    }
    case PlayMode::OnceClear:
    case PlayMode::OnceHold:
        return std::min(progress, 1.0);
    }
    return 0.0;
}

bool playsBackward(PlayMode mode, double progress)
{
    return mode == PlayMode::Reverse || (mode == PlayMode::Bounce && std::fmod(progress, 2.0) >= 1.0);
}

double progressAt(PlayMode mode, double phase, bool backward)
{
    if (mode == PlayMode::Reverse)
        return 1.0 - phase;
    if (mode == PlayMode::Bounce && backward)
        return 2.0 - phase;
    return phase;
}

RandomJump::RandomJump(double start, double interval, double distance, int beats, uint64_t key, int64_t slot)
    : interval_(interval), beats_(beats), key_(key), slot_(slot)
{
    grid_ = std::clamp(interval, 1.0 / 128.0, beats < 2 ? 0.25 : 1.0);
    const double reach = std::min(distance, beats - grid_);
    steps_ = reach > 0.0 ? int(reach / grid_) : -1;
    base_ = wrap(start);
    next_ = draw(wrap(base_ + interval_), 1);
}

double RandomJump::position(double elapsed)
{
    while (elapsed >= double(jumps_ + 1) * interval_) {
        ++jumps_;
        base_ = next_;
        next_ = draw(wrap(base_ + interval_), jumps_ + 1);
    }
    return wrap(base_ + elapsed - double(jumps_) * interval_);
}

double RandomJump::draw(double from, int64_t jump) const
{
    if (steps_ < 0)
        return from;
    const int k = int(keyedUniform(mixKey(key_, uint64_t(slot_ + jump)), uint32_t(2 * steps_ + 1))) - steps_;
    return wrap(from - std::fmod(from, grid_) + k * grid_);
}

double RandomJump::wrap(double beat) const
{
    const double m = std::fmod(beat, double(beats_));
    return m < 0.0 ? m + beats_ : m;
}

} // namespace azl
