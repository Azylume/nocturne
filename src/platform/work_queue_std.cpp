#include "platform/work_queue.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace azl {

using Clock = std::chrono::steady_clock;

struct WorkQueue::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    // Under mutex.
    std::deque<std::function<void()>> ready;
    std::multimap<Clock::time_point, std::function<void()>> timed;
    bool stopping = false;
    std::thread thread;

    void post(Clock::time_point due, std::function<void()> task)
    {
        {
            std::lock_guard lock(mutex);
            timed.emplace(due, std::move(task));
        }
        wake.notify_one();
    }

    // Queue thread only. Tasks that are not due when the queue stops are dropped.
    void run(std::optional<ThreadRole> role)
    {
        if (role)
            setThreadRole(*role);
        std::unique_lock lock(mutex);
        while (true) {
            for (auto due = timed.begin(); due != timed.end() && due->first <= Clock::now(); due = timed.erase(due))
                ready.push_back(std::move(due->second));
            if (!ready.empty()) {
                std::function<void()> task = std::move(ready.front());
                ready.pop_front();
                lock.unlock();
                task();
                lock.lock();
            } else if (stopping) {
                return;
            } else if (timed.empty()) {
                wake.wait(lock);
            } else {
                wake.wait_until(lock, timed.begin()->first);
            }
        }
    }
};

WorkQueue::WorkQueue(const char*, std::optional<ThreadRole> role) : impl_(std::make_unique<Impl>())
{
    impl_->thread = std::thread([impl = impl_.get(), role] { impl->run(role); });
}

WorkQueue::~WorkQueue()
{
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_one();
    impl_->thread.join();
}

void WorkQueue::async(std::function<void()> task)
{
    {
        std::lock_guard lock(impl_->mutex);
        impl_->ready.push_back(std::move(task));
    }
    impl_->wake.notify_one();
}

void WorkQueue::flush()
{
    std::promise<void> done;
    async([&done] { done.set_value(); });
    done.get_future().wait();
}

namespace {

// Shared with the timer's queued runs, which may outlive the timer until they are due.
struct TimerState {
    std::chrono::nanoseconds interval;
    std::function<void()> task;
    std::atomic<bool> cancelled{false};
};

void schedule(WorkQueue::Impl& queue, std::shared_ptr<TimerState> state, Clock::time_point due)
{
    queue.post(due, [&queue, state, due] {
        if (state->cancelled)
            return;
        state->task();
        if (state->interval.count() > 0)
            schedule(queue, state, std::max(due + state->interval, Clock::now()));
    });
}

} // namespace

struct QueueTimer::Impl {
    WorkQueue& queue;
    std::shared_ptr<TimerState> state;
};

QueueTimer::QueueTimer(WorkQueue& queue, std::chrono::nanoseconds delay, std::chrono::nanoseconds interval,
                       std::function<void()> task)
    : impl_(new Impl{queue, std::shared_ptr<TimerState>(new TimerState{interval, std::move(task)})})
{
    schedule(*queue.impl_, impl_->state, Clock::now() + delay);
}

QueueTimer::~QueueTimer()
{
    impl_->state->cancelled = true;
    impl_->queue.flush();
}

namespace detail {

void parallelFor(size_t count, ThreadRole role, void* context, void (*body)(void* context, size_t index))
{
    if (count == 0)
        return;
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; (i = next++) < count;)
            body(context, i);
    };
    const size_t helpers = std::min<size_t>(count, std::max(1u, std::thread::hardware_concurrency())) - 1;
    std::vector<std::thread> threads;
    threads.reserve(helpers);
    for (size_t t = 0; t < helpers; ++t)
        threads.emplace_back([&] {
            setThreadRole(role);
            work();
        });
    work();
    for (std::thread& thread : threads)
        thread.join();
}

} // namespace detail

} // namespace azl
