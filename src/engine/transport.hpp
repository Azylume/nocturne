#pragma once

#include "document/composition.hpp"

#include <cstdint>

namespace azl {

// The power of two beats that puts a clip of `seconds` between 80 and 160 BPM.
int autoBeats(double seconds);

struct FramePick {
    int index = 0;
    bool forward = true;
    // False once a play-once-and-clear clip has ended: the layer shows nothing.
    bool visible = true;
};

// The frame a play mode shows after `progress` clip lengths (>= 0) of a clip of `frames` frames. Playing forward,
// frame i covers positions [i, i + 1) in frame units; playing backward it covers (i, i + 1], so the clip's end shows
// its last frame, never the one past it. Random Jump positions are picked as Loop.
FramePick pickFrame(PlayMode mode, double progress, int frames);

// Where in the clip (0..1) a play mode is after `progress` clip lengths, and whether it is moving backward.
double clipPhase(PlayMode mode, double progress);
bool playsBackward(PlayMode mode, double progress);

// The progress at which a play mode is at `phase`, moving backward if it can, so the picture stays where it is when
// the timing changes or the clip is scrubbed.
double progressAt(PlayMode mode, double phase, bool backward);

// Random choices are a function of a key only, never of a generator's history, so every engine with the same seed
// makes the same choice for the same moment, also one that joined the show late.
uint64_t mixKey(uint64_t a, uint64_t b);
// Uniform in 0..count-1, by rejection.
uint32_t keyedUniform(uint64_t key, uint32_t count);

// What a layer's draws are for: its Random Jump, its Autopilot, or the column Autopilot's (as layer 0); an effect's
// Autopilot draws as Steps, keyed by the effect.
enum class Choice : uint64_t { Jump = 1, Pilot = 2, Columns = 3, Steps = 4 };
uint64_t layerKey(uint32_t seed, Choice choice, uint64_t layer);

// Random Jump in beats, as Resolume does it. Every `interval` clip beats the clip jumps to the start of the grid step
// it is in plus k steps, k uniform in -n..n, wrapping around the clip. The step is the interval capped at one beat (a
// quarter beat in clips under two beats); n is the distance, capped at the clip's beats less one step, in whole steps.
// Between jumps the clip plays forward. Each target is drawn as the previous jump happens, one interval ahead. Jump n
// after the start draws with `key` and slot + n, where the slot numbers the start on the interval grid.
class RandomJump {
public:
    // Starts at `start` clip beats.
    RandomJump(double start, double interval, double distance, int beats, uint64_t key, int64_t slot);

    // The position in clip beats, within [0, beats), after `elapsed` clip beats since the start. `elapsed` never
    // decreases between calls.
    double position(double elapsed);
    // Where the next jump lands, in clip beats.
    double nextTarget() const { return next_; }

private:
    double draw(double from, int64_t jump) const;
    double wrap(double beat) const;

    double interval_;
    double grid_;
    // -1 when the distance allows no jump.
    int steps_;
    int beats_;
    uint64_t key_;
    int64_t slot_;
    int64_t jumps_ = 0;
    double base_;
    double next_;
};

} // namespace azl
