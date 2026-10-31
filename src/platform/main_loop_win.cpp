#include "platform/main_loop.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>

namespace azl {

namespace {

constexpr UINT kRunWork = WM_APP + 1;
std::atomic<MainLoop*> loop{nullptr};

} // namespace

// Work arrives as posted messages to a message-only window, so it also runs inside the modal loops Windows runs while a
// window is moved or resized.
struct MainLoop::Impl {
    DWORD thread = GetCurrentThreadId();
    HWND window = nullptr;
    std::mutex mutex;
    std::deque<std::function<void()>> work;
    // Loop thread only.
    std::map<UINT_PTR, std::function<void()>> timers;
    UINT_PTR nextTimer = 1;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (impl && message == kRunWork) {
            impl->runOne();
            return 0;
        }
        if (impl && message == WM_TIMER) {
            impl->fire(wParam);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void runOne()
    {
        std::function<void()> next;
        {
            std::lock_guard lock(mutex);
            if (work.empty())
                return;
            next = std::move(work.front());
            work.pop_front();
        }
        next();
    }

    void fire(UINT_PTR id)
    {
        KillTimer(window, id);
        auto it = timers.find(id);
        if (it == timers.end())
            return;
        std::function<void()> due = std::move(it->second);
        timers.erase(it);
        due();
    }
};

MainLoop::MainLoop() : impl_(std::make_unique<Impl>())
{
    static const wchar_t* cls = [] {
        WNDCLASSEXW c{.cbSize = sizeof(WNDCLASSEXW)};
        c.lpfnWndProc = &Impl::procedure;
        c.hInstance = GetModuleHandleW(nullptr);
        c.lpszClassName = L"NocturneMainLoop";
        RegisterClassExW(&c);
        return c.lpszClassName;
    }();
    impl_->window =
        CreateWindowExW(0, cls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!impl_->window)
        throw std::runtime_error("main loop: cannot make its message window");
    SetWindowLongPtrW(impl_->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_.get()));
    MainLoop* expected = nullptr;
    if (!loop.compare_exchange_strong(expected, this)) {
        DestroyWindow(impl_->window);
        throw std::logic_error("main loop: one per process");
    }
}

MainLoop::~MainLoop()
{
    loop = nullptr;
    DestroyWindow(impl_->window);
}

MainLoop* MainLoop::current()
{
    return loop.load();
}

void MainLoop::post(std::function<void()> work)
{
    {
        std::lock_guard lock(impl_->mutex);
        impl_->work.push_back(std::move(work));
    }
    PostMessageW(impl_->window, kRunWork, 0, 0);
}

void MainLoop::after(std::chrono::milliseconds delay, std::function<void()> work)
{
    post([impl = impl_.get(), delay, work = std::move(work)]() mutable {
        const UINT_PTR id = impl->nextTimer++;
        impl->timers.emplace(id, std::move(work));
        SetTimer(impl->window, id, UINT(std::max<long long>(delay.count(), USER_TIMER_MINIMUM)), nullptr);
    });
}

void MainLoop::run()
{
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void MainLoop::quit()
{
    post([] { PostQuitMessage(0); });
}

bool MainLoop::onLoopThread() const
{
    return GetCurrentThreadId() == impl_->thread;
}

} // namespace azl
