#pragma once

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace azl {

// When the Supervisor leaves safe mode on its own (ADR 0019): after a cool-down of `base`, 2x and 4x seconds, at most
// kMaxPerDay retries in any rolling 24 h, and a probation of kProbation seconds after each retry. Times are seconds
// on one timeline the caller owns and passes in, so nothing here reads a clock. Used by the Supervisor thread alone.
class SafeRetry {
public:
    static constexpr int kMaxPerDay = 3;
    static constexpr double kDay = 24.0 * 3600.0;
    static constexpr double kProbation = 600.0;

    enum class Last { None, Survived, Failed };
    enum class Scheduled { Off, Yes, Capped };

    // `base` is the first cool-down; 0 or less turns retrying off. `retries` are the start times of earlier retries.
    explicit SafeRetry(double base, std::vector<double> retries = {}) : base_(base), retries_(std::move(retries)) {}

    bool enabled() const { return base_ > 0.0; }
    bool onProbation() const { return probation_; }
    Last last() const { return last_; }
    std::optional<double> nextAt() const { return next_; }

    int attempts(double now) const { return int(recent(now).size()); }
    std::vector<double> recent(double now) const
    {
        std::vector<double> kept;
        std::copy_if(retries_.begin(), retries_.end(), std::back_inserter(kept),
                     [&](double t) { return now - t < kDay; });
        return kept;
    }

    // Safe mode that crashes caused. A retry on probation that gets here has failed, and the next cool-down doubles.
    Scheduled enteredSafeMode(double now)
    {
        next_.reset();
        if (!enabled())
            return Scheduled::Off;
        if (probation_) {
            probation_ = false;
            last_ = Last::Failed;
            ++level_;
        }
        if (attempts(now) >= kMaxPerDay)
            return Scheduled::Capped;
        next_ = now + base_ * std::pow(2.0, level_);
        return Scheduled::Yes;
    }

    // Safe mode a client asked for, or left: no timer, and the cool-down starts over.
    void clientDecided()
    {
        next_.reset();
        probation_ = false;
        level_ = 0;
    }

    bool due(double now) const { return next_ && now >= *next_; }

    void retryStarted(double now)
    {
        retries_ = recent(now);
        retries_.push_back(now);
        next_.reset();
        probation_ = true;
        retryStart_ = now;
    }

    // True once, when the retried engine has run for the whole probation: the cool-down starts over.
    bool survived(double now)
    {
        if (!probation_ || now - retryStart_ < kProbation)
            return false;
        probation_ = false;
        level_ = 0;
        last_ = Last::Survived;
        return true;
    }

private:
    double base_;
    std::vector<double> retries_;
    std::optional<double> next_;
    bool probation_ = false;
    double retryStart_ = 0.0;
    int level_ = 0;
    Last last_ = Last::None;
};

} // namespace azl
