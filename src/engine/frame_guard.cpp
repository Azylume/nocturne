#include "engine/frame_guard.hpp"

#include <algorithm>

namespace azl {

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
    const size_t was = level;
    if (gpu > thresholds.raiseAt * budget && level < layers && frames >= thresholds.settle) {
        level++;
        before = gpu;
        saved.push_back(-1.0);
    } else if (level > 0) {
        double& last = saved.back();
        if (last < 0.0 && frames >= 30)
            last = std::max(0.0, before - gpu);
        calm = last >= 0.0 && gpu + last < 0.75 * budget ? calm + 1 : 0;
        if (calm >= 60) {
            level--;
            saved.pop_back();
        }
    }
    if (level == was)
        return false;
    frames = calm = 0;
    return true;
}

} // namespace azl
