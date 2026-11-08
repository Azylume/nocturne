// Stands in for the engine under nocturne-supervisor and the UI's engine launcher, on every platform: it beats on the
// heartbeat it was given, records crash marks in its state folder as the engine does, and runs one command at a time
// from STATE/command (removed once read), or the one --do names:
//   exit N            exits with status N
//   hang              stops beating and ignores stop requests until it is killed
//   fault             faults while it holds a "clip" mark for poison.mp4
//   fault-elsewhere   faults on another thread while this one holds that mark
//   fault-compiling   faults while it holds an "effect" mark for the chain "chain"
//   crash-first N     faults without a mark on each of its first N runs, counted in STATE/crash-count, then runs on
//   no-gpu            says on stderr that it found no usable GPU and exits as the engine does then
// NOCTURNE_FAKE_ENGINE_DO names the command when there is no --do; started with --safe-mode it runs none from there
// and says its --safe-retry-at. It quits on a stop request, and when nobody reads its heartbeat any more.
#include "platform/process.hpp"
#include "supervisor/crash_guard.hpp"
#include "supervisor/restart_request.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

void say(const std::string& line)
{
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

std::string takeCommand(const fs::path& state)
{
    if (state.empty())
        return {};
    const fs::path file = state / "command";
    std::ifstream in(file);
    std::string command;
    if (!std::getline(in, command))
        return {};
    in.close();
    std::error_code ec;
    fs::remove(file, ec);
    return command;
}

void fault()
{
    int* volatile nowhere = nullptr;
    *nowhere = 1;
}

[[noreturn]] void hang()
{
    say("fake engine hanging");
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

// Faults on each of the first `limit` runs.
void crashFirst(const fs::path& state, int limit)
{
    const fs::path file = state / "crash-count";
    int runs = 0;
    std::ifstream(file) >> runs;
    if (runs >= limit)
        return;
    std::ofstream(file) << runs + 1 << "\n";
    fault();
}

// Runs one command; the exit status when it ends the process.
std::optional<int> run(const std::string& command, const fs::path& state)
{
    say("fake engine: " + command);
    if (command.rfind("exit ", 0) == 0)
        return std::atoi(command.c_str() + 5);
    if (command == "hang")
        hang();
    if (command.rfind("crash-first ", 0) == 0)
        crashFirst(state, std::atoi(command.c_str() + 12));
    if (command == "no-gpu") {
        std::fprintf(stderr, "%sfake has no Vulkan 1.3 device.\n", azl::restart::kNoGpuLine);
        return azl::restart::kNoGpu;
    }
    if (command == "fault") {
        azl::crash::Scope opening("clip", "poison", "poison.mp4");
        fault();
    }
    if (command == "fault-compiling") {
        azl::crash::Scope compiling("effect", "chain", "chain");
        fault();
    }
    if (command == "fault-elsewhere") {
        azl::crash::Scope opening("clip", "poison", "poison.mp4");
        std::thread(fault).join();
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv)
{
    std::string command, safeMode, safeRetryAt;
    fs::path state;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--do")
            command = argv[++i];
        else if (arg == "--safe-mode")
            safeMode = argv[++i];
        else if (arg == "--safe-retry-at")
            safeRetryAt = argv[++i];
        else if (arg == "--state")
            state = argv[++i];
    }
    if (const char* fromEnvironment = std::getenv("NOCTURNE_FAKE_ENGINE_DO"); command.empty() && fromEnvironment)
        command = safeMode.empty() ? fromEnvironment : "";
    if (!azl::watchStopRequests())
        return 1;
    if (!state.empty()) {
        azl::crash::setup(state / "inflight.json", state / "skip.json", state / "crash.json");
        azl::crash::installCrashHandlers();
    }
    say(safeMode.empty() ? "fake engine started" : "safe mode: " + safeMode);
    if (!safeRetryAt.empty())
        say("safe retry at " + safeRetryAt);
    std::string args;
    for (int i = 1; i < argc; ++i)
        args += std::string(" ") + argv[i];
    say("fake engine args:" + args);
    const std::optional<azl::NativeHandle> heartbeat = azl::inheritedHeartbeat();
    while (true) {
        if (command.empty())
            command = takeCommand(state);
        if (!command.empty())
            if (const std::optional<int> status = run(command, state))
                return *status;
        command.clear();
        if (heartbeat && azl::writeHandle(*heartbeat, "h", 1) < 0) {
            say("supervisor gone, quitting");
            return 0;
        }
        if (azl::waitForStop(0.05)) {
            say("fake engine stopping");
            return 0;
        }
    }
}
