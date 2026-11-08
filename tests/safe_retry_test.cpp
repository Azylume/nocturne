#include "supervisor/safe_retry.hpp"

#include <doctest.h>

namespace {

using azl::SafeRetry;
constexpr double kBase = 60.0;

// One failed retry: the cool-down runs out, the retry starts and the engine crashes soon after.
double failedRetry(SafeRetry& retry)
{
    const double due = *retry.nextAt();
    CHECK(retry.due(due));
    retry.retryStarted(due);
    retry.enteredSafeMode(due + 5.0);
    return due + 5.0;
}

} // namespace

TEST_CASE("safe retry: off, nothing is ever scheduled")
{
    SafeRetry retry(0.0);
    CHECK_FALSE(retry.enabled());
    CHECK(retry.enteredSafeMode(100.0) == SafeRetry::Scheduled::Off);
    CHECK_FALSE(retry.nextAt());
    CHECK_FALSE(retry.due(1e9));
}

TEST_CASE("safe retry: the cool-down doubles after each failed retry and the third retry is the last")
{
    SafeRetry retry(kBase);
    double now = 1000.0;
    CHECK(retry.enteredSafeMode(now) == SafeRetry::Scheduled::Yes);
    CHECK(*retry.nextAt() == now + kBase);
    CHECK_FALSE(retry.due(now + kBase - 1.0));

    now = failedRetry(retry);
    CHECK(retry.last() == SafeRetry::Last::Failed);
    CHECK(*retry.nextAt() == now + 2 * kBase);

    now = failedRetry(retry);
    CHECK(*retry.nextAt() == now + 4 * kBase);

    const double due = *retry.nextAt();
    retry.retryStarted(due);
    CHECK(retry.enteredSafeMode(due + 5.0) == SafeRetry::Scheduled::Capped);
    CHECK_FALSE(retry.nextAt());
    CHECK(retry.attempts(due + 5.0) == 3);
}

TEST_CASE("safe retry: a crash on probation is a failure, a crash after the probation starts over")
{
    SafeRetry retry(kBase);
    retry.enteredSafeMode(0.0);
    retry.retryStarted(kBase);
    CHECK(retry.onProbation());
    CHECK_FALSE(retry.survived(kBase + SafeRetry::kProbation - 1.0));
    CHECK(retry.onProbation());

    CHECK(retry.survived(kBase + SafeRetry::kProbation));
    CHECK_FALSE(retry.survived(kBase + SafeRetry::kProbation + 1.0));
    CHECK_FALSE(retry.onProbation());
    CHECK(retry.last() == SafeRetry::Last::Survived);

    const double later = kBase + SafeRetry::kProbation + 100.0;
    CHECK(retry.enteredSafeMode(later) == SafeRetry::Scheduled::Yes);
    CHECK(*retry.nextAt() == later + kBase);
    CHECK(retry.attempts(later) == 1);
}

TEST_CASE("safe retry: the cap is 3 in a rolling 24 hours")
{
    SafeRetry retry(kBase);
    retry.enteredSafeMode(0.0);
    for (double start : {100.0, 200.0, 300.0})
        retry.retryStarted(start);
    CHECK(retry.enteredSafeMode(400.0) == SafeRetry::Scheduled::Capped);
    CHECK(retry.enteredSafeMode(100.0 + SafeRetry::kDay - 1.0) == SafeRetry::Scheduled::Capped);
    CHECK(retry.attempts(100.0 + SafeRetry::kDay) == 2);
    CHECK(retry.enteredSafeMode(100.0 + SafeRetry::kDay) == SafeRetry::Scheduled::Yes);
}

TEST_CASE("safe retry: safe mode a client asked for or left is never retried and starts the cool-down over")
{
    SafeRetry retry(kBase);
    retry.enteredSafeMode(0.0);
    retry.clientDecided();
    CHECK_FALSE(retry.nextAt());
    CHECK_FALSE(retry.due(1e9));

    retry.enteredSafeMode(10.0);
    retry.retryStarted(10.0 + kBase);
    retry.enteredSafeMode(10.0 + kBase + 1.0);
    CHECK(*retry.nextAt() == 10.0 + kBase + 1.0 + 2 * kBase);
    retry.retryStarted(*retry.nextAt());
    retry.clientDecided();
    CHECK_FALSE(retry.onProbation());
    CHECK(retry.enteredSafeMode(5000.0) == SafeRetry::Scheduled::Yes);
    CHECK(*retry.nextAt() == 5000.0 + kBase);
}

TEST_CASE("safe retry: the retries of an earlier supervisor count against the cap")
{
    SafeRetry first(kBase);
    first.enteredSafeMode(0.0);
    for (double start : {100.0, 200.0, 300.0})
        first.retryStarted(start);

    SafeRetry restarted(kBase, first.recent(400.0));
    CHECK(restarted.attempts(400.0) == 3);
    CHECK(restarted.enteredSafeMode(400.0) == SafeRetry::Scheduled::Capped);
}
