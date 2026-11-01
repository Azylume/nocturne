#include "app/engine_main.hpp"

#include "app/main_thread.hpp"
#include "app/safe_mode.hpp"
#include "audio/audio_input.hpp"
#include "platform/mac_displays.hpp"
#include "platform/profile.hpp"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <malloc/malloc.h>
#include <memory>
#include <thread>
#include <unistd.h>

#include <sys/resource.h>

namespace azl::app {

const char* const kHandlesStat = "machPorts";

namespace {

double cpuSeconds()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    auto seconds = [](const timeval& t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

// What malloc holds for the process, beside RSS: a memory step that is not in malloc is in pages the frameworks map.
double mallocInUseMB()
{
    malloc_statistics_t stats{};
    malloc_zone_statistics(nullptr, &stats);
    return double(stats.size_in_use) / 1048576.0;
}

size_t threadCount()
{
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return 0;
    for (mach_msg_type_number_t i = 0; i < count; ++i)
        mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), vm_address_t(threads), count * sizeof(thread_act_t));
    return count;
}

size_t portCount()
{
    mach_port_name_array_t names = nullptr;
    mach_port_type_array_t types = nullptr;
    mach_msg_type_number_t nameCount = 0, typeCount = 0;
    if (mach_port_names(mach_task_self(), &names, &nameCount, &types, &typeCount) != KERN_SUCCESS)
        return 0;
    vm_deallocate(mach_task_self(), vm_address_t(names), nameCount * sizeof(mach_port_name_t));
    vm_deallocate(mach_task_self(), vm_address_t(types), typeCount * sizeof(mach_port_type_t));
    return nameCount;
}

dispatch_source_t quitOnSignal(int sig)
{
    std::signal(sig, SIG_IGN);
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, uintptr_t(sig), 0,
                                                      dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{ stopMain(); });
    dispatch_resume(source);
    return source;
}

// Tells the supervisor that frames are being finished: a byte down its pipe whenever a new frame was published since
// the last look. When the supervisor is gone the engine quits, so it never lingers beside the next one.
class Heartbeat {
public:
    Heartbeat(LatestFrame latest, int fd) : thread_([this, latest = std::move(latest), fd] { run(latest, fd); }) {}
    ~Heartbeat()
    {
        running_ = false;
        thread_.join();
    }

private:
    void run(const LatestFrame& latest, int fd)
    {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        uint64_t last = UINT64_MAX;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const uint64_t frame = latest();
            if (frame == last)
                continue;
            last = frame;
            if (::write(fd, "h", 1) < 0 && errno == EPIPE) {
                std::printf("supervisor gone, quitting\n");
                std::fflush(stdout);
                onMain(stopMain);
                return;
            }
        }
    }

    std::atomic<bool> running_{true};
    std::thread thread_;
};

int micCheck()
{
    std::string permission = azl::AudioInput::permission();
    std::printf("microphone permission: %s\n", permission.c_str());
    std::fflush(stdout);
    if (permission == "notDetermined" && azl::AudioInput::disabled())
        std::printf("not asking: NOCTURNE_NO_AUDIO_INPUT is set\n");
    else if (permission == "notDetermined") {
        std::printf("asking for microphone access; answer the prompt on this Mac's screen\n");
        std::fflush(stdout);
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                 completionHandler:^(BOOL) { dispatch_semaphore_signal(answered); }];
        if (dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, int64_t(300) * NSEC_PER_SEC)))
            std::printf("no answer within 5 minutes\n");
        permission = azl::AudioInput::permission();
        std::printf("microphone permission: %s\n", permission.c_str());
    }
    for (const azl::AudioDeviceInfo& d : azl::AudioInput::devices())
        std::printf("input %s%s: %d channels at %.0f Hz, uid %s\n", d.name.c_str(), d.isDefault ? " (default)" : "",
                    d.channels, d.sampleRate, d.uid.c_str());
    return permission == "authorized" ? 0 : 1;
}

int run(const EngineConfig& opt)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:opt.offscreen ? NSApplicationActivationPolicyProhibited
                                             : NSApplicationActivationPolicyAccessory];

    // A show runs unattended: no App Nap, no display sleep.
    id activity = [NSProcessInfo.processInfo
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical | NSActivityIdleDisplaySleepDisabled
                          reason:@"live show"];
    const int code = runEngine(opt);
    [NSProcessInfo.processInfo endActivity:activity];
    return code;
}

} // namespace

std::unique_ptr<Displays> windowDisplays(bool windowed, int virtualDisplays)
{
    return std::make_unique<MacDisplays>(windowed, virtualDisplays);
}

void runMain()
{
    [NSApp run];
}

Running quitOnStopRequests()
{
    dispatch_source_t onInt = quitOnSignal(SIGINT);
    dispatch_source_t onTerm = quitOnSignal(SIGTERM);
    return Running(nullptr, [onInt, onTerm](void*) {
        dispatch_source_cancel(onInt);
        dispatch_source_cancel(onTerm);
    });
}

Running beatToSupervisor(LatestFrame latest)
{
    const char* fd = std::getenv("NOCTURNE_HEARTBEAT_FD");
    if (!fd)
        return nullptr;
    return std::make_shared<Heartbeat>(std::move(latest), std::atoi(fd));
}

ProcessUse processUse()
{
    return {.cpuSeconds = cpuSeconds(), .threads = threadCount(), .handles = portCount(), .heapMB = mallocInUseMB()};
}

} // namespace azl::app

int main(int argc, char** argv)
{
    AZL_THREAD_NAME("main");
    azl::becomeResponsibleProcess(argv);
    std::signal(SIGPIPE, SIG_IGN);
    @autoreleasepool {
        const azl::app::EngineConfig opt = azl::app::parseCommandLine({argv + 1, argv + argc});
        if (opt.micCheck)
            return azl::app::micCheck();
        if (!opt.safeMode.empty())
            return azl::runSafeMode(opt);
        try {
            return azl::app::run(opt);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nocturne: %s\n", e.what());
            return 1;
        }
    }
}
