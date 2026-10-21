#pragma once

#include "engine/machine_settings.hpp"

#include <cstddef>
#include <vector>

namespace azl {

// When the frame guard draws one more layer at half resolution: once the recent GPU time is over `raiseAt` of the
// frame budget for `settle` frames since its last step.
struct GuardThresholds {
    double raiseAt = 0.9;
    int settle = 15;
};

// Smooth steps as the GPU time nears the budget, Sharp only once frames overrun it. With protectTransitions, while a
// transition runs the guard steps at once and earlier, since a transition's extra passes arrive within a frame.
GuardThresholds guardThresholds(GuardProfile profile, bool protectTransitions, bool fading);

// The frame budget guard's level: how many of the costliest layers it draws at half resolution. It steps one level
// at a time and waits for the GPU time average to follow before the next step. A level comes off only once the frames
// would fit with what it saved added back, so it does not flicker on and off. Render thread only.
struct FrameGuard {
    size_t level = 0;
    int frames = 0;
    int calm = 0;
    double before = 0.0;
    // What each level saved, measured once it settled; negative until then.
    std::vector<double> saved;

    // One frame of `gpu` seconds against `budget`, with `layers` that can be reduced; true when the level changed.
    bool step(double gpu, double budget, size_t layers, const GuardThresholds& thresholds);
};

} // namespace azl
