#pragma once

#include <array>

namespace azl {

// The running range of an audio feature, for audio modulators with Auto range: the 10th and 90th percentile of the
// feature over about the last 30 s of music, which a modulator maps onto its target range instead of raw 0..1, so a
// show looks the same at any input level. Features are linear (a band level at -20 dB is a tenth of one at 0 dB), so
// the history is a histogram of 1 dB bins over 60 dB, each frame weighing its duration and the past fading with a
// 30 s time constant. Frames without music (no live input, or the full-band level under -60 dB) teach nothing, so
// silence never narrows the range. Until it has heard a few seconds the range blends from 0..1, today's raw mapping,
// so the first seconds never jump.
//
// Render thread, and cheap: a frame adds to one bin (the fading is a weight that grows instead of every bin shrinking,
// renormalised now and then), the percentiles are found again four times a second, and mapping a value is a
// subtraction and a multiplication. Fixed size, no allocation.
class AutoRange {
public:
    static constexpr int kBins = 60;
    static constexpr double kFloorDb = -60.0;
    static constexpr double kSeconds = 30.0;
    // Seconds of music until the learned range takes over from 0..1.
    static constexpr double kWarmUp = 4.0;
    // The narrowest range, as a fraction of its top, so noise on a steady feature is not blown up to a full swing.
    static constexpr double kMinSpan = 0.25;
    static constexpr double kLow = 0.10;
    static constexpr double kHigh = 0.90;
    // How often the percentiles are found again.
    static constexpr double kUpdateSeconds = 0.25;

    struct Range {
        double low = 0.0;
        double high = 1.0;
    };

    // One frame of `value` (0..1) lasting `dt` seconds; `music` false teaches nothing.
    void learn(double value, double dt, bool music);
    // The range to map, blended from 0..1 during the warm-up, as of the last update.
    Range range() const { return range_; }
    // `value` mapped from the range onto 0..1, clamped.
    double map(double value) const
    {
        const double v = (value - range_.low) * scale_;
        return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
    }
    // Seconds of music heard, as the fading history weighs them.
    double heard() const { return total_ / weight_; }

private:
    void update();

    std::array<double, kBins> bins_{};
    // Every bin and the total are in units of `weight_`, which grows by e every kSeconds of music.
    double weight_ = 1.0;
    double total_ = 0.0;
    double sinceUpdate_ = 0.0;
    Range range_;
    double scale_ = 1.0;
};

} // namespace azl
