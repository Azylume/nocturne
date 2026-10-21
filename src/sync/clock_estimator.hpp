#pragma once

#include <cstddef>
#include <deque>
#include <optional>

namespace azl {

// One NTP-style ping-pong: sent and received on this clock, received and answered on the remote one.
struct ClockSample {
    double sent = 0.0;
    double remoteReceived = 0.0;
    double remoteSent = 0.0;
    double received = 0.0;

    double offset() const { return ((remoteReceived - sent) + (remoteSent - received)) / 2.0; }
    double roundTrip() const { return (received - sent) - (remoteSent - remoteReceived); }
    double midpoint() const { return (sent + received) / 2.0; }
};

// Estimates the remote clock as offset + drift against this one from recent ping-pongs. Only the samples with the
// shortest round trips count, since a sample delayed one way is off by half its extra delay; a line through them
// gives the drift once they span long enough.
class ClockEstimator {
public:
    static constexpr size_t kWindow = 256;
    static constexpr double kMinDriftSpan = 5.0;
    static constexpr double kMaxDrift = 500e-6;

    void add(const ClockSample& sample);
    // Remote time minus local time at local time `local`; nothing before the first sample.
    std::optional<double> offsetAt(double local) const;
    double drift() const { return drift_; }
    // The shortest round trip in the window.
    double roundTrip() const { return roundTrip_; }
    size_t samples() const { return samples_.size(); }

private:
    void fit();

    std::deque<ClockSample> samples_;
    double offset_ = 0.0;
    double drift_ = 0.0;
    double at_ = 0.0;
    double roundTrip_ = 0.0;
};

} // namespace azl
