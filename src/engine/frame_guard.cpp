#include "engine/frame_guard.hpp"

#include <algorithm>

namespace azl {

namespace {

constexpr double kFitsAt = 0.75;    // of the budget: a frame under it counts as calm
constexpr int kCalm = 60;           // calm frames that take a level off, until a bounce
constexpr int kMaxCalm = 36000;     // ten minutes at 60 fps
constexpr int kBounceWithin = 120;  // frames after a step down within which a step up is a bounce

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
    if (sinceOff >= 0 && ++sinceOff > kBounceWithin) {
        sinceOff = -1;
        calmNeeded = kCalm;
    }
    const size_t was = level;
    hot = gpu > thresholds.raiseAt * budget ? hot + 1 : 0;
    if (hot >= thresholds.settle && level < layers && frames >= thresholds.settle) {
        if (sinceOff >= 0) {
            calmNeeded = std::min(calmNeeded * 2, kMaxCalm);
            sinceOff = -1;
        }
        level++;
    } else if (level > 0) {
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
