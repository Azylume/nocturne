#include "app/engine_main.hpp"

#include "app/main_thread.hpp"
#include "app/safe_mode.hpp"
#include "audio/audio_input.hpp"
#include "platform/main_loop.hpp"
#include "platform/process.hpp"
#include "platform/profile.hpp"

#include <windows.h>

#include <psapi.h>
#include <shellapi.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace azl::app {

const char* const kHandlesStat = "handles";

namespace {

std::string utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(size, 1) - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

// The flags as UTF-8, whatever the console's code page.
std::vector<std::string> commandLineArgs()
{
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> args;
    for (int i = 1; i < count; ++i)
        args.push_back(utf8(wide[i]));
    LocalFree(wide);
    return args;
}

double seconds(const FILETIME& t)
{
    return double(uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 1e7;
}

size_t threadCount()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    const DWORD self = GetCurrentProcessId();
    size_t count = 0;
    THREADENTRY32 entry{.dwSize = sizeof(THREADENTRY32)};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        count += entry.th32OwnerProcessID == self;
    CloseHandle(snapshot);
    return count;
}

// Windows has no microphone prompt to answer: the privacy setting decides, and Settings changes it.
int micCheck()
{
    const std::string permission = AudioInput::permission();
    std::printf("microphone permission: %s\n", permission.c_str());
    if (permission != "authorized")
        std::printf("allow it in Settings > Privacy & security > Microphone (desktop apps)\n");
    for (const AudioDeviceInfo& d : AudioInput::devices())
        std::printf("input %s%s: %d channels at %.0f Hz, uid %s\n", d.name.c_str(), d.isDefault ? " (default)" : "",
                    d.channels, d.sampleRate, d.uid.c_str());
    return permission == "authorized" ? 0 : 1;
}

// A show runs unattended: no sleep, no display sleep, and 1 ms timer resolution for the threads that pace frames.
int run(const EngineConfig& opt)
{
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    timeBeginPeriod(1);
    const int code = runEngine(opt);
    timeEndPeriod(1);
    SetThreadExecutionState(ES_CONTINUOUS);
    return code;
}

} // namespace

void runMain()
{
    MainLoop::current()->run();
}

Running beatToSupervisor(LatestFrame latest)
{
    const std::optional<NativeHandle> pipe = inheritedHeartbeat();
    if (!pipe)
        return nullptr;
    auto running = std::make_shared<std::atomic<bool>>(true);
    auto beats = std::make_shared<std::thread>([running, latest = std::move(latest), pipe = *pipe] {
        uint64_t last = UINT64_MAX;
        while (*running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const uint64_t frame = latest();
            if (frame == last)
                continue;
            last = frame;
            if (writeHandle(pipe, "h", 1) < 0) {
                std::printf("supervisor gone, quitting\n");
                std::fflush(stdout);
                onMain(stopMain);
                return;
            }
        }
    });
    return Running(nullptr, [running, beats](void*) {
        *running = false;
        beats->join();
    });
}

Running quitOnStopRequests()
{
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    auto watcher = std::make_shared<std::thread>([stopped] {
        while (!*stopped)
            if (waitForStop(0.1)) {
                stopMain();
                return;
            }
    });
    return Running(nullptr, [stopped, watcher](void*) {
        *stopped = true;
        watcher->join();
    });
}

ProcessUse processUse()
{
    ProcessUse use;
    FILETIME created{}, ended{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
        use.cpuSeconds = seconds(kernel) + seconds(user);
    use.threads = threadCount();
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        use.handles = handles;
    PROCESS_MEMORY_COUNTERS_EX memory{.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX)};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), memory.cb))
        use.heapMB = double(memory.PrivateUsage) / 1048576.0;
    return use;
}

} // namespace azl::app

int main()
{
    AZL_THREAD_NAME("main");
    // Before any thread starts, so a stop asked for at any time is seen.
    azl::watchStopRequests();
    SetConsoleOutputCP(CP_UTF8);
    const azl::app::EngineConfig opt = azl::app::parseCommandLine(azl::app::commandLineArgs());
    if (opt.micCheck)
        return azl::app::micCheck();
    try {
        azl::MainLoop loop;
        if (!opt.safeMode.empty())
            return azl::runSafeMode(opt);
        return azl::app::run(opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nocturne: %s\n", e.what());
        return 1;
    }
}
