#pragma once

#include "platform/thread_priority.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace azl {

// Runs tasks one at a time, in the order they were queued, off the calling thread (a GCD serial queue on Apple, a
// thread of its own elsewhere). Any thread may queue.
class WorkQueue {
public:
    // Without a role the queue runs at the default priority.
    explicit WorkQueue(const char* label, std::optional<ThreadRole> role = std::nullopt);
    // Runs every task queued so far, then returns. Never on the queue itself.
    ~WorkQueue();
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;

    // Any thread, returns at once.
    void async(std::function<void()> task);
    // Blocks until every task queued before this call has run. Never on the queue itself.
    void flush();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    friend class QueueTimer;
};

// Runs a task on a queue after a delay, then again every interval (once when the interval is zero), until the timer is
// destroyed.
class QueueTimer {
public:
    QueueTimer(WorkQueue& queue, std::chrono::nanoseconds delay, std::chrono::nanoseconds interval,
               std::function<void()> task);
    // The task never runs after this returns. Never on the queue itself.
    ~QueueTimer();
    QueueTimer(const QueueTimer&) = delete;
    QueueTimer& operator=(const QueueTimer&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

namespace detail {
void parallelFor(size_t count, ThreadRole role, void* context, void (*body)(void* context, size_t index));
}

// Calls body(i) for every i below count, spread over the CPU cores at the role's priority, and returns when every call
// has returned. Allocates nothing on Apple.
template <class Body>
void parallelFor(size_t count, ThreadRole role, Body&& body)
{
    using Stored = std::remove_reference_t<Body>;
    detail::parallelFor(count, role, const_cast<void*>(static_cast<const void*>(&body)),
                        [](void* context, size_t index) { (*static_cast<Stored*>(context))(index); });
}

} // namespace azl
