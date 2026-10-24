#include "engine/frame_guard.hpp"

#include <algorithm>

namespace azl {

namespace {

constexpr double kFitsAt = 0.75;       // of the budget: a frame under it counts as calm
constexpr double kCalmSeconds = 1.0;   // of calm that takes a level off, until a bounce
constexpr double kMaxCalmSeconds = 600.0;
constexpr double kBounceSeconds = 2.0; // after a step down, within which a step up is a bounce

// Seconds as frames of the budget, which is the frame at the engine's rate.
int framesOf(double seconds, double budget)
{
    return std::max(1, int(seconds / budget + 0.5));
}

} // namespace

GuardThresholds guardThresholds(GuardProfile profile, bool protectTransitions, bool fading)
{
    GuardThresholds t = profile == GuardProfile::Sharp ? GuardThresholds{1.0, 30} : GuardThresholds{0.9, 15};
    if (protectTransitions && fading)
        t = {std::min(t.raiseAt, 0.8), 3};
    return t;
}

bool FrameGuard::step(double gpu, double budget, size_t layers, const GuardThresholds& thresholds)
{
    frames++;
    if (sinceOff >= 0 && ++sinceOff > framesOf(kBounceSeconds, budget)) {
        sinceOff = -1;
        calmTimes = 1;
    }
    const size_t was = level;
    hot = gpu > thresholds.raiseAt * budget ? hot + 1 : 0;
    if (hot >= thresholds.settle && level < layers && frames >= thresholds.settle) {
        if (sinceOff >= 0) {
            calmTimes = std::min(calmTimes * 2, int(kMaxCalmSeconds / kCalmSeconds));
            sinceOff = -1;
        }
        level++;
    }
    calmNeeded = std::min(framesOf(kCalmSeconds * calmTimes, budget), framesOf(kMaxCalmSeconds, budget));
    if (level == was && level > 0) {
        calm = gpu < kFitsAt * budget ? calm + 1 : std::max(0, calm - 2);
        if (calm >= calmNeeded) {
            level--;
            sinceOff = 0;
        }
    }
    if (level == was)
        return false;
    frames = hot = calm = 0;
    return true;
}

} // namespace azl
