#include "engine/playhead.hpp"

#include "platform/profile.hpp"

#include <algorithm>
#include <cmath>

namespace azl {

Region regionOf(const Transport& transport, int frames)
{
    if (frames <= 0)
        return {0, 1};
    const int first = std::clamp(int(transport.in * frames), 0, frames - 1);
    const int end = std::clamp(int(std::ceil(transport.out * frames)), first + 1, frames);
    return {first, end - first};
}

FramePick inRegion(FramePick pick, const Region& region)
{
    pick.index += region.first;
    return pick;
}

void Playhead::retime(const Transport& next, ClipLength clip, double seconds, double beat, uint64_t clockResyncs)
{
    const int clipBeats = next.beats > 0 ? next.beats : autoBeats(regionOf(next, clip.frames).frames / clip.fps);
    const bool known = length.frames > 0;
    Transport faster = transport;
    faster.speed = next.speed;
    if (known && jumps && faster == next && next != transport && clipBeats == beats && clip == length) {
        // Random Jump carries on with its sequence at the new speed.
        anchorProgress = progress(seconds, beat);
        anchorTime = seconds;
        anchorBeat = beat;
    } else if (known && (next != transport || clipBeats != beats || clip != length)) {
        const auto [phase, backward] = position(seconds, beat);
        anchor(progressAt(next.playMode, phase, backward), seconds, beat);
    }
    transport = next;
    length = clip;
    beats = clipBeats;
    if (resyncs != clockResyncs) {
        resyncs = clockResyncs;
        if (next.sync == SyncMode::Bpm)
            anchor(0.0, seconds, 0.0);
    }
    startJumps();
}

std::pair<double, bool> Playhead::position(double seconds, double beat)
{
    const double p = progress(seconds, beat);
    if (jumps)
        return {jumps->position((p - jumpsFrom) * beats) / beats, false};
    return {clipPhase(transport.playMode, p), playsBackward(transport.playMode, p)};
}

void Playhead::anchor(double at, double seconds, double beat)
{
    anchorProgress = at;
    anchorTime = seconds;
    anchorBeat = beat;
    jumps.reset();
}

void Playhead::scrub(double position, double seconds, double beat)
{
    const bool backward = this->position(seconds, beat).second;
    anchor(progressAt(transport.playMode, position, backward), seconds, beat);
    startJumps();
}

void Playhead::startJumps()
{
    if (transport.playMode != PlayMode::RandomJump) {
        jumps.reset();
        return;
    }
    if (jumps)
        return;
    jumpsFrom = anchorProgress;
    jumps.emplace(clipPhase(PlayMode::Loop, anchorProgress) * beats, transport.jumpInterval, transport.jumpDistance,
                  beats, key, std::llround(anchorBeat / transport.jumpInterval));
}

// The next Random Jump target, where a bounce turns and, for BPM-synced clips, the first frame, where a resync or a
// retrigger restarts them, are prefetched.
FramePick Playhead::pick(double seconds, double beat, std::vector<FrameTarget>& targets)
{
    AZL_ZONE("pick frame");
    const PlayMode mode = transport.playMode;
    const Region region = regionOf(transport, length.frames);
    const int frames = region.frames;
    FramePick shown = pickFrame(mode, progress(seconds, beat), frames);
    if (jumps) {
        shown = pickFrame(mode, position(seconds, beat).first, frames);
        targets.push_back({pickFrame(mode, jumps->nextTarget() / beats, frames).index + region.first, true});
    }
    shown = inRegion(shown, region);
    if (mode == PlayMode::Bounce)
        targets.push_back(shown.forward ? FrameTarget{region.first + frames - 1, false} : FrameTarget{region.first, true});
    if (transport.sync == SyncMode::Bpm) {
        const FramePick first = inRegion(pickFrame(mode, 0.0, frames), region);
        targets.push_back({first.index, first.forward});
    } else if (frames < length.frames && (mode == PlayMode::Loop || mode == PlayMode::Reverse)) {
        // The decoders wrap at the clip's ends, so a loop within in and out prefetches where it wraps.
        targets.push_back(mode == PlayMode::Loop ? FrameTarget{region.first, true}
                                                 : FrameTarget{region.first + frames - 1, false});
    }
    return shown;
}

double Playhead::progress(double seconds, double beat) const
{
    const double speed = transport.speed;
    const double elapsed = transport.sync == SyncMode::Bpm
                               ? (beat - anchorBeat) * speed / beats
                               : (seconds - anchorTime) * length.fps * speed / regionOf(transport, length.frames).frames;
    return anchorProgress + std::max(0.0, elapsed);
}

} // namespace azl
