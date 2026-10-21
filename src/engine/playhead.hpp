#pragma once

#include "document/composition.hpp"
#include "engine/transport.hpp"
#include "media/video_decoder.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace azl {

// The frames a clip plays between its in and out points. The first frame and the end are found as the decoders find a
// preload's start at `in` forward and at `out` backward, so a preload parks exactly on the frame the clip starts on.
struct Region {
    int first = 0;
    int frames = 1;
};

Region regionOf(const Transport& transport, int frames);
FramePick inRegion(FramePick pick, const Region& region);

// Where a clip is and which frames it shows and prefetches.
//
// The clip's progress in clip lengths is computed from an anchor: progress = anchor.progress + the beats (BPM sync) or
// seconds (timeline) since the anchor, over the clip's length. The anchor moves only when the clip's timing changes or
// it is scrubbed, so the clip follows tempo changes without a jump and never drifts. Random Jump runs from the anchor
// too, on the layer's own seeded generator.
struct Playhead {
    Transport transport;
    ClipLength length;
    int beats = 0;
    double anchorTime = 0.0;
    double anchorBeat = 0.0;
    double anchorProgress = 0.0;
    // The beat clock's resyncs as of the anchor.
    uint64_t resyncs = 0;
    // Keys Random Jump's draws.
    uint64_t key = 0;
    std::optional<RandomJump> jumps;
    // The progress Random Jump started at.
    double jumpsFrom = 0.0;

    // Re-anchors where the clip is now when its timing changes. A resync (`clockResyncs` moving on) restarts BPM-synced
    // clips on the new downbeat.
    void retime(const Transport& next, ClipLength clip, double seconds, double beat, uint64_t clockResyncs);
    // Moves the clip to `position` (0..1 of the clip); it plays on from there.
    void scrub(double position, double seconds, double beat);
    // The frame to show, and the frames to prefetch into `targets`.
    FramePick pick(double seconds, double beat, std::vector<FrameTarget>& targets);
    double progress(double seconds, double beat) const;
    // Where in the clip (0..1) it is now, and whether it is moving backward.
    std::pair<double, bool> position(double seconds, double beat);
    void anchor(double at, double seconds, double beat);
    void startJumps();
};

} // namespace azl
