#include "platform/win_displays.hpp"

#include "gpu/gpu_vulkan.hpp"
#include "platform/thread_priority.hpp"
#include "platform/win_monitors.hpp"

#include <windows.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace azl {

namespace {

constexpr int kWindowedWidth = 960;
constexpr int kWindowedHeight = 540;
constexpr int kGap = 8;
constexpr const char* kVirtualPrefix = "window-";
constexpr uint32_t kImages = 3;

double steadySeconds(std::chrono::steady_clock::time_point at)
{
    return std::chrono::duration<double>(at.time_since_epoch()).count();
}

// 1-based index of a virtual display id, or 0.
int virtualIndex(const std::string& id, int count)
{
    if (id.rfind(kVirtualPrefix, 0) != 0)
        return 0;
    const int index = std::atoi(id.c_str() + std::strlen(kVirtualPrefix));
    return index >= 1 && index <= count ? index : 0;
}

RECT workArea(void* monitor)
{
    MONITORINFO info{.cbSize = sizeof(MONITORINFO)};
    GetMonitorInfoW(static_cast<HMONITOR>(monitor), &info);
    return info.rcWork;
}

win::Area centred(void* monitor)
{
    const RECT area = workArea(monitor);
    return {(area.left + area.right - kWindowedWidth) / 2, (area.top + area.bottom - kWindowedHeight) / 2,
            kWindowedWidth, kWindowedHeight};
}

// Virtual display `index` of `count`, side by side along the top of the primary monitor.
win::Area tile(int index, int count)
{
    const RECT area = workArea(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    const int width = std::min(kWindowedWidth, int(area.right - area.left - kGap * (count + 1)) / count);
    const int titleBar = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME);
    return {area.left + kGap + (index - 1) * (width + kGap), area.top + kGap + titleBar, width, width * 9 / 16};
}

// A window's swapchain, presented in the order the present thread draws, one image per refresh of its monitor. The
// present thread draws into nextTarget, the caller commits, and the image goes to the screen right after the refresh's
// callback returns, so the present always follows its submission. Shown times come from VK_KHR_present_wait on a
// thread of the surface's own; without it, from the refresh the present was queued for.
class VulkanSurface : public Surface {
public:
    VulkanSurface(std::unique_ptr<win::OutputWindow> window, const gpu::Device& device, double refreshHz)
        : window_(std::move(window)), device_(device), queue_(device.makeQueue()), refreshHz_(refreshHz),
          vk_(gpu::vulkan::logicalDevice(device))
    {
        surface_ = gpu::vulkan::makeSurface(device, GetModuleHandleW(nullptr), window_->handle());
        if (!surface_)
            throw std::runtime_error("vulkan: cannot make a surface for the output window");
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu::vulkan::physicalDevice(device), gpu::vulkan::queueFamily(device),
                                             surface_, &supported);
        if (!supported)
            throw std::runtime_error("vulkan: the GPU's queue cannot present to this window");
        if (gpu::vulkan::presentWait(device))
            waitForPresent_ =
                reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(vk_, "vkWaitForPresentKHR"));
        for (auto& slot : *slots_)
            slot = true;
        acquired_.resize(kImages + 1);
        for (VkSemaphore& s : acquired_)
            s = makeSemaphore();
        build();
    }

    ~VulkanSurface() override
    {
        tearDown();
        for (VkSemaphore s : acquired_)
            vkDestroySemaphore(vk_, s, nullptr);
        vkDestroySurfaceKHR(gpu::vulkan::instance(device_), surface_, nullptr);
    }

    double refreshHz() const override { return refreshHz_; }

    void runRefreshes(const std::atomic<bool>& running, const Refresh& onRefresh) override
    {
        win::VblankWaiter vblank(window_->monitor(), refreshHz_);
        period_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / refreshHz_));
        while (running) {
            vsync_ = vblank.wait();
            onRefresh(vsync_);
            presentPending();
        }
    }

    const gpu::Texture* nextTarget() override
    {
        if (stale_ && !rebuild())
            return nullptr;
        if (!(*slots_)[slot_])
            return nullptr;
        uint32_t image = 0;
        const VkResult result = vkAcquireNextImageKHR(vk_, swapchain_, 0, acquired_[slot_], VK_NULL_HANDLE, &image);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            stale_ = true;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return nullptr;
        image_ = image;
        return &targets_[image];
    }

    void present(gpu::CommandBuffer& cmd, Shown shown) override
    {
        gpu::vulkan::waitSemaphore(cmd, acquired_[slot_], VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        gpu::vulkan::signalSemaphore(cmd, rendered_[image_]);
        (*slots_)[slot_] = false;
        cmd.onCompleted([slots = slots_, slot = slot_](const gpu::Completion&) { (*slots)[slot] = true; });
        slot_ = (slot_ + 1) % acquired_.size();
        pending_ = {image_, std::move(shown)};
    }

private:
    struct Pending {
        uint32_t image = UINT32_MAX;
        Shown shown;
    };
    struct Waiting {
        uint64_t id = 0;
        Shown shown;
    };

    VkSemaphore makeSemaphore() const
    {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = VK_NULL_HANDLE;
        vkCreateSemaphore(vk_, &info, nullptr, &semaphore);
        return semaphore;
    }

    // Present thread, after the refresh's callback committed the command buffer that draws the image.
    void presentPending()
    {
        if (pending_.image == UINT32_MAX)
            return;
        Pending sent = std::exchange(pending_, Pending{});
        const uint64_t id = ++presents_;
        VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &id;
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.pNext = waitForPresent_ ? &presentId : nullptr;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &rendered_[sent.image];
        info.swapchainCount = 1;
        info.pSwapchains = &swapchain_;
        info.pImageIndices = &sent.image;
        const VkResult result = gpu::vulkan::present(queue_, info);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            stale_ = true;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            sent.shown(0.0);
            return;
        }
        if (!waitForPresent_) {
            sent.shown(steadySeconds(vsync_ + period_));
            return;
        }
        std::lock_guard lock(waitMutex_);
        waiting_.push_back({id, std::move(sent.shown)});
        waitWake_.notify_one();
    }

    // Reports each present when it reached the screen. Its own thread, as urgent as the present thread: a wait blocks
    // until the present is shown, and the time it reports is when it woke.
    void waitForPresents()
    {
        setThreadRole(ThreadRole::Frames);
        std::unique_lock lock(waitMutex_);
        for (;;) {
            waitWake_.wait(lock, [this] { return stopWaiting_ || !waiting_.empty(); });
            if (waiting_.empty())
                return;
            Waiting next = std::move(waiting_.front());
            waiting_.pop_front();
            lock.unlock();
            VkResult result = VK_TIMEOUT;
            while (result == VK_TIMEOUT && !stopWaiting_)
                result = waitForPresent_(vk_, swapchain_, next.id, 50'000'000);
            next.shown(result == VK_SUCCESS ? steadySeconds(std::chrono::steady_clock::now()) : 0.0);
            lock.lock();
        }
    }

    void build(VkSwapchainKHR old = VK_NULL_HANDLE)
    {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu::vulkan::physicalDevice(device_), surface_, &caps);
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            const win::Area area = window_->clientArea();
            extent = {uint32_t(area.width), uint32_t(area.height)};
        }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = std::clamp(kImages, caps.minImageCount, caps.maxImageCount ? caps.maxImageCount : kImages);
        info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        const VkCompositeAlphaFlagsKHR alphas = caps.supportedCompositeAlpha;
        info.compositeAlpha = (alphas & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                  : VkCompositeAlphaFlagBitsKHR(alphas & (~alphas + 1));
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = old;
        if (vkCreateSwapchainKHR(vk_, &info, nullptr, &swapchain_) != VK_SUCCESS)
            throw std::runtime_error("vulkan: cannot make the output's swapchain");
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(vk_, swapchain_, &count, nullptr);
        std::vector<VkImage> images(count);
        vkGetSwapchainImagesKHR(vk_, swapchain_, &count, images.data());
        for (VkImage image : images) {
            targets_.push_back(
                gpu::vulkan::wrapSwapchainImage(device_, image, info.imageFormat, extent.width, extent.height));
            rendered_.push_back(makeSemaphore());
        }
        stopWaiting_ = false;
        if (waitForPresent_)
            waiter_ = std::thread([this] { waitForPresents(); });
    }

    // Waits until the GPU is done with every image this surface handed out, so they can go.
    bool idle() const
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < until) {
            if (std::all_of(slots_->begin(), slots_->end(), [](const auto& free) { return free.load(); }))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    void stopWaiter()
    {
        {
            std::lock_guard lock(waitMutex_);
            stopWaiting_ = true;
            for (Waiting& w : waiting_)
                w.shown(0.0);
            waiting_.clear();
        }
        waitWake_.notify_all();
        if (waiter_.joinable())
            waiter_.join();
    }

    // The last present has reached the screen, so the presentation engine holds none of the swapchain's semaphores.
    void drainPresents()
    {
        if (waitForPresent_ && presents_)
            waitForPresent_(vk_, swapchain_, presents_, 200'000'000);
        else
            std::this_thread::sleep_for(period_ * 3);
    }

    void tearDown()
    {
        stopWaiter();
        idle();
        drainPresents();
        targets_.clear();
        for (VkSemaphore s : rendered_)
            vkDestroySemaphore(vk_, s, nullptr);
        rendered_.clear();
        vkDestroySwapchainKHR(vk_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    // Present thread: a new swapchain for a window whose size or monitor changed.
    bool rebuild()
    {
        if (!idle())
            return false;
        stopWaiter();
        drainPresents();
        targets_.clear();
        for (VkSemaphore s : rendered_)
            vkDestroySemaphore(vk_, s, nullptr);
        rendered_.clear();
        VkSwapchainKHR old = std::exchange(swapchain_, VK_NULL_HANDLE);
        try {
            build(old);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
        }
        vkDestroySwapchainKHR(vk_, old, nullptr);
        stale_ = false;
        return swapchain_ != VK_NULL_HANDLE;
    }

    std::unique_ptr<win::OutputWindow> window_;
    gpu::Device device_;
    gpu::Queue queue_;
    double refreshHz_;
    VkDevice vk_;
    PFN_vkWaitForPresentKHR waitForPresent_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<gpu::Texture> targets_;
    // Per image: signalled when its drawing is done, waited for by its present.
    std::vector<VkSemaphore> rendered_;
    // Per acquire slot, used in turn: signalled when the image is free to draw into.
    std::vector<VkSemaphore> acquired_;
    // Whether the command buffer that last waited on each acquire slot has completed. Shared with completion handlers.
    std::shared_ptr<std::array<std::atomic<bool>, kImages + 1>> slots_ =
        std::make_shared<std::array<std::atomic<bool>, kImages + 1>>();
    // Present thread only.
    size_t slot_ = 0;
    uint32_t image_ = 0;
    Pending pending_;
    uint64_t presents_ = 0;
    bool stale_ = false;
    std::chrono::steady_clock::time_point vsync_{};
    std::chrono::steady_clock::duration period_{};

    std::mutex waitMutex_;
    std::condition_variable waitWake_;
    std::deque<Waiting> waiting_;
    std::atomic<bool> stopWaiting_{false};
    std::thread waiter_;
};

} // namespace

std::optional<Displays::OutputKey> outputKeyFor(const win::WindowKey& key)
{
    const bool none = !key.alt && !key.control && !key.shift && !key.windows;
    const bool altOnly = key.alt && !key.control && !key.shift && !key.windows;
    if (key.virtualKey == VK_ESCAPE && none)
        return Displays::OutputKey::release;
    if (key.virtualKey == VK_F4 && altOnly)
        return Displays::OutputKey::quit;
    return std::nullopt;
}

struct WinDisplays::Impl {
    WinDisplays* owner = nullptr;
    bool windowed = false;
    int virtualDisplays = 0;
    std::unique_ptr<win::DisplayWatcher> watcher;

    std::unique_ptr<win::OutputWindow> window(win::OutputWindow::Style style, win::Area area, const std::string& id,
                                              bool hideCursor)
    {
        return std::make_unique<win::OutputWindow>(style, area, "Nocturne " + id, hideCursor,
                                                   [this, id](const win::WindowKey& key) {
                                                       if (std::optional<OutputKey> k = outputKeyFor(key))
                                                           owner->outputKey(*k, id);
                                                   });
    }
};

WinDisplays::WinDisplays(bool windowed, int virtualDisplays) : impl_(std::make_unique<Impl>())
{
    win::usePhysicalPixels();
    impl_->owner = this;
    impl_->windowed = windowed;
    impl_->virtualDisplays = virtualDisplays;
    impl_->watcher = std::make_unique<win::DisplayWatcher>([this] { changed(); });
}

WinDisplays::~WinDisplays() = default;

std::vector<DisplayInfo> WinDisplays::attached() const
{
    const std::vector<win::Monitor> monitors = win::activeMonitors();
    std::vector<DisplayInfo> displays;
    if (impl_->virtualDisplays == 0) {
        for (const win::Monitor& m : monitors)
            displays.push_back(m.info);
        return displays;
    }
    const double hz = monitors.empty() ? 60.0 : monitors.front().info.refreshHz;
    for (int i = 1; i <= impl_->virtualDisplays; ++i)
        displays.push_back({kVirtualPrefix + std::to_string(i), "Window " + std::to_string(i), 1920, 1080, hz});
    return displays;
}

std::unique_ptr<Surface> WinDisplays::open(const std::string& id, const gpu::Device& device)
{
    using Style = win::OutputWindow::Style;
    const std::vector<win::Monitor> monitors = win::activeMonitors();
    if (impl_->virtualDisplays > 0) {
        const int index = virtualIndex(id, impl_->virtualDisplays);
        if (!index || monitors.empty())
            return nullptr;
        auto window = impl_->window(Style::Titled, tile(index, impl_->virtualDisplays), id, false);
        return std::make_unique<VulkanSurface>(std::move(window), device, monitors.front().info.refreshHz);
    }
    const auto monitor = std::find_if(monitors.begin(), monitors.end(), [&](const win::Monitor& m) {
        return m.info.id == id && m.info.mirrorOf.empty();
    });
    if (monitor == monitors.end())
        return nullptr;
    const DisplayInfo& d = monitor->info;
    const win::Area full{d.x, d.y, int(d.width), int(d.height)};
    std::unique_ptr<win::OutputWindow> window =
        impl_->windowed ? impl_->window(Style::Titled, centred(win::monitorHandle(monitor->gdiName)), id, false)
                        : impl_->window(Style::Borderless, full, id, true);
    return std::make_unique<VulkanSurface>(std::move(window), device, d.refreshHz);
}

bool WinDisplays::unmirror(const std::vector<std::string>& ids)
{
    return impl_->virtualDisplays == 0 && !ids.empty() && win::extendDesktop();
}

} // namespace azl
