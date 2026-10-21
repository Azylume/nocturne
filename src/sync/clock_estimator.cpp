#include "sync/clock_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace azl {

void ClockEstimator::add(const ClockSample& sample)
{
    if (!(sample.roundTrip() >= 0.0) || !std::isfinite(sample.offset()))
        return;
    samples_.push_back(sample);
    if (samples_.size() > kWindow)
        samples_.pop_front();
    fit();
}

std::optional<double> ClockEstimator::offsetAt(double local) const
{
    if (samples_.empty())
        return std::nullopt;
    return offset_ + drift_ * (local - at_);
}

void ClockEstimator::fit()
{
    std::vector<ClockSample> best(samples_.begin(), samples_.end());
    std::sort(best.begin(), best.end(),
              [](const ClockSample& a, const ClockSample& b) { return a.roundTrip() < b.roundTrip(); });
    roundTrip_ = best.front().roundTrip();
    best.resize(std::max<size_t>(std::min<size_t>(best.size(), 4), best.size() / 4));

    double meanT = 0.0;
    double meanO = 0.0;
    for (const ClockSample& s : best) {
        meanT += s.midpoint();
        meanO += s.offset();
    }
    meanT /= double(best.size());
    meanO /= double(best.size());
    double stt = 0.0;
    double sto = 0.0;
    double first = best.front().midpoint();
    double last = first;
    for (const ClockSample& s : best) {
        stt += (s.midpoint() - meanT) * (s.midpoint() - meanT);
        sto += (s.midpoint() - meanT) * (s.offset() - meanO);
        first = std::min(first, s.midpoint());
        last = std::max(last, s.midpoint());
    }
    offset_ = meanO;
    at_ = meanT;
    drift_ = last - first >= kMinDriftSpan && stt > 0.0 ? std::clamp(sto / stt, -kMaxDrift, kMaxDrift) : 0.0;
}

} // namespace azl
