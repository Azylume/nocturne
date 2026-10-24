#pragma once

#include "engine/machine_settings.hpp"

#include <cstddef>

namespace azl {

// When the frame guard draws one more layer at half resolution: once the recent GPU time has been over `raiseAt` of
// the frame budget for `settle` frames in a row, and `settle` frames since its last step.
struct GuardThresholds {
    double raiseAt = 0.9;
    int settle = 15;
};

// Smooth steps as the GPU time nears the budget, Sharp only once frames overrun it. With protectTransitions, while a
// transition runs the guard steps at once and earlier, since a transition's extra passes arrive within a frame.
GuardThresholds guardThresholds(GuardProfile profile, bool protectTransitions, bool fading);

// The frame budget guard's level: how many of the costliest layers it draws at half resolution. It steps up one level
// at a time, on a sustained overrun only (one long frame, as a chain compile makes, is not a step), and waits for the
// GPU time average to follow before the next step. A level comes off once the frames have fitted well under the
// budget for `calmNeeded` frames (a second at the engine's rate, which the budget is a frame of), counting a frame
// over it against two under, so a spike delays it a little and never holds it up. A level that comes off and is
// needed back within two seconds was holding the frames in the budget: each such bounce doubles the calm the next
// step down needs (up to ten minutes), and a step down that holds resets it to a second. Render thread only.
struct FrameGuard {
    size_t level = 0;
    int frames = 0;        // since the level last changed
    int hot = 0;           // frames in a row over raiseAt
    int calm = 0;          // fitting frames, less two per frame over, since the level last changed
    int calmTimes = 1;     // seconds of calm that take a level off, doubled by each bounce
    int calmNeeded = 0;    // that as frames of the budget, as of the last step
    int sinceOff = -1;     // frames since a level came off, while a step up would be a bounce; -1 otherwise

    // One frame of `gpu` seconds against `budget`, with `layers` that can be reduced; true when the level changed.
    bool step(double gpu, double budget, size_t layers, const GuardThresholds& thresholds);
};

} // namespace azl
