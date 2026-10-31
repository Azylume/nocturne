#include "app/main_thread.hpp"

#include "platform/main_loop.hpp"

#include <atomic>

namespace azl::app {

namespace {

MainLoop& loop()
{
    return *MainLoop::current();
}

} // namespace

void onMain(std::function<void()> work)
{
    loop().post(std::move(work));
}

void onMainAfter(std::chrono::milliseconds delay, std::function<void()> work)
{
    loop().after(delay, std::move(work));
}

bool onMainThread()
{
    return MainLoop::current() && loop().onLoopThread();
}

void stopMain()
{
    loop().quit();
}

// Each run arms the next one while the timer lives.
struct MainTimer::Impl {
    struct State {
        std::chrono::milliseconds interval;
        std::function<void()> work;
        std::atomic<bool> alive{true};
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    static void arm(const std::shared_ptr<State>& state)
    {
        onMainAfter(state->interval, [state] {
            if (!state->alive)
                return;
            state->work();
            arm(state);
        });
    }
};

MainTimer::MainTimer(std::chrono::milliseconds interval, std::function<void()> work) : impl_(std::make_unique<Impl>())
{
    impl_->state->interval = interval;
    impl_->state->work = std::move(work);
    Impl::arm(impl_->state);
}

MainTimer::~MainTimer()
{
    impl_->state->alive = false;
}

} // namespace azl::app
