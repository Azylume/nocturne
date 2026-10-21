#pragma once

#include "sync/clock_estimator.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace azl {

using Clock = std::function<double()>;

// The main engine's clock as seen from a node, for one connection.
class ClockLink {
public:
    virtual ~ClockLink() = default;
    // Main engine time minus this engine's time at local time `local`, once known.
    virtual std::optional<double> offsetAt(double local) const = 0;
    virtual double roundTrip() const { return 0.0; }
};

// Answers clock pings over UDP with the time they arrived and the time they were answered (ADR 0005).
class ClockResponder {
public:
    // Port 0 picks a free port. Throws when it cannot listen.
    ClockResponder(uint16_t port, Clock now);
    ~ClockResponder();

    uint16_t port() const { return port_; }

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
    uint16_t port_ = 0;
};

// Pings a ClockResponder ten times a second and estimates its clock against `now`.
class ClockPinger : public ClockLink {
public:
    ClockPinger(const std::string& host, uint16_t port, Clock now);
    ~ClockPinger() override;

    std::optional<double> offsetAt(double local) const override;
    double roundTrip() const override;
    double drift() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace azl
