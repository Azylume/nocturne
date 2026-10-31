#include "platform/work_queue.hpp"

#include "platform/qos_apple.hpp"

#include <dispatch/dispatch.h>

namespace azl {

struct WorkQueue::Impl {
    dispatch_queue_t queue;
};

struct QueueTimer::Impl {
    WorkQueue& queue;
    dispatch_source_t source;
    std::function<void()> task;
};

WorkQueue::WorkQueue(const char* label, std::optional<ThreadRole> role)
    : impl_(new Impl{dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL)})
{
    if (role)
        dispatch_set_target_queue(impl_->queue, dispatch_get_global_queue(qosClass(*role), 0));
}

WorkQueue::~WorkQueue()
{
    flush();
    dispatch_release(impl_->queue);
}

void WorkQueue::async(std::function<void()> task)
{
    dispatch_async_f(impl_->queue, new std::function<void()>(std::move(task)), [](void* context) {
        std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(context));
        (*task)();
    });
}

void WorkQueue::flush()
{
    dispatch_sync_f(impl_->queue, nullptr, [](void*) {});
}

QueueTimer::QueueTimer(WorkQueue& queue, std::chrono::nanoseconds delay, std::chrono::nanoseconds interval,
                       std::function<void()> task)
    : impl_(new Impl{queue, dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue.impl_->queue),
                     std::move(task)})
{
    const uint64_t repeat = interval.count() > 0 ? uint64_t(interval.count()) : DISPATCH_TIME_FOREVER;
    dispatch_source_set_timer(impl_->source, dispatch_time(DISPATCH_TIME_NOW, delay.count()), repeat, NSEC_PER_MSEC);
    dispatch_set_context(impl_->source, &impl_->task);
    dispatch_source_set_event_handler_f(impl_->source,
                                        [](void* context) { (*static_cast<std::function<void()>*>(context))(); });
    dispatch_resume(impl_->source);
}

QueueTimer::~QueueTimer()
{
    dispatch_source_cancel(impl_->source);
    impl_->queue.flush();
    dispatch_release(impl_->source);
}

namespace detail {

void parallelFor(size_t count, ThreadRole role, void* context, void (*body)(void* context, size_t index))
{
    dispatch_apply_f(count, dispatch_get_global_queue(qosClass(role), 0), context, body);
}

} // namespace detail

} // namespace azl
