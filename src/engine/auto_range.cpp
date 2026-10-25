#include "engine/auto_range.hpp"

#include <algorithm>
#include <cmath>

namespace azl {

namespace {

// The bin of a linear 0..1 value: 1 dB bins from kFloorDb to 0 dB, everything below the floor in the first.
int binOf(double value)
{
    if (value <= 0.0)
        return 0;
    const double db = 20.0 * std::log10(value);
    return std::clamp(int((db - AutoRange::kFloorDb) / -AutoRange::kFloorDb * AutoRange::kBins), 0, AutoRange::kBins - 1);
}

// The linear value at a fractional bin position.
double valueAt(double bin)
{
    return std::pow(10.0, (AutoRange::kFloorDb + bin / AutoRange::kBins * -AutoRange::kFloorDb) / 20.0);
}

constexpr double kRenormaliseAt = 1e12;

} // namespace

void AutoRange::learn(double value, double dt, bool music)
{
    if (!music || !(dt > 0.0))
        return;
    dt = std::min(dt, 0.5);
    weight_ *= std::exp(dt / kSeconds);
    const double w = dt * weight_;
    bins_[size_t(binOf(value))] += w;
    total_ += w;
    if (weight_ > kRenormaliseAt) {
        for (double& b : bins_)
            b /= weight_;
        total_ /= weight_;
        weight_ = 1.0;
    }
    sinceUpdate_ += dt;
    // The first update is at once, so the warm-up starts from the first frame of music.
    if (sinceUpdate_ >= kUpdateSeconds || total_ == w) {
        sinceUpdate_ = 0.0;
        update();
    }
}

void AutoRange::update()
{
    // The percentiles, interpolated within their bin.
    auto percentile = [&](double fraction) {
        const double target = fraction * total_;
        double below = 0.0;
        for (int i = 0; i < kBins; ++i) {
            const double here = bins_[size_t(i)];
            if (here > 0.0 && below + here >= target)
                return valueAt(i + (target - below) / here);
            below += here;
        }
        return 1.0;
    };
    double low = percentile(kLow), high = percentile(kHigh);
    high = std::max(high, low + kMinSpan * std::max(high, 1e-3));
    const double w = std::min(heard() / kWarmUp, 1.0);
    range_ = {w * low, w * high + (1.0 - w)};
    scale_ = 1.0 / (range_.high - range_.low);
}

} // namespace azl
