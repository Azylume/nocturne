// Stands in for the engine under nocturne-supervisor and the UI's engine launcher, on every platform: it beats on the
// heartbeat it was given, records crash marks in its state folder as the engine does, and runs one command at a time
// from STATE/command (removed once read), or the one --do names:
//   exit N            exits with status N
//   hang              stops beating and ignores stop requests until it is killed
//   fault             faults while it holds a "clip" mark for poison.mp4
//   fault-elsewhere   faults on another thread while this one holds that mark
// It quits on a stop request, and when nobody reads its heartbeat any more.
#include "platform/process.hpp"
#include "supervisor/crash_guard.hpp"

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

// Runs one command; the exit status when it ends the process.
std::optional<int> run(const std::string& command)
{
    say("fake engine: " + command);
    if (command.rfind("exit ", 0) == 0)
        return std::atoi(command.c_str() + 5);
    if (command == "hang")
        hang();
    if (command == "fault") {
        azl::crash::Scope opening("clip", "poison", "poison.mp4");
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
    std::string command, safeMode;
    fs::path state;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--do")
            command = argv[++i];
        else if (arg == "--safe-mode")
            safeMode = argv[++i];
        else if (arg == "--state")
            state = argv[++i];
    }
    if (!azl::watchStopRequests())
        return 1;
    if (!state.empty()) {
        azl::crash::setup(state / "inflight.json", state / "skip.json", state / "crash.json");
        azl::crash::installCrashHandlers();
    }
    say(safeMode.empty() ? "fake engine started" : "safe mode: " + safeMode);
    const std::optional<azl::NativeHandle> heartbeat = azl::inheritedHeartbeat();
    while (true) {
        if (command.empty())
            command = takeCommand(state);
        if (!command.empty())
            if (const std::optional<int> status = run(command))
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
