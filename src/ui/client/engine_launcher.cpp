#include "ui/client/engine_launcher.hpp"

#include "net/socket_compat.hpp"
#include "platform/app_paths.hpp"
#include "platform/process.hpp"
#include "supervisor/restart_request.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace azl::ui {

namespace {

namespace fs = std::filesystem;

// True when something accepts a TCP connection on 127.0.0.1:port within a short window: an engine is already up.
bool portAnswers(uint16_t port)
{
    startSockets();
    const Socket sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == kNoSocket)
        return false;
    setNonBlocking(sock);
    in_addr loopback{};
    loopback.s_addr = htonl(INADDR_LOOPBACK);
    const sockaddr_in addr = ipv4Endpoint(loopback, port);

    bool answers = false;
    if (::connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
        answers = true;
    } else if (wouldBlock(socketError())) {
        pollfd pfd{sock, POLLOUT, 0};
        if (pollSockets(&pfd, 1, 300) > 0 && (pfd.revents & POLLOUT)) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0 && err == 0)
                answers = true;
        }
    }
    closeSocket(sock);
    return answers;
}

// Passes the engine's stderr through to the UI's own on a thread of its own, keeping the line in which the engine said
// it found no usable GPU.
class ErrorPump {
public:
    explicit ErrorPump(NativeHandle error) : error_(error), thread_([this] { run(); }) {}
    ~ErrorPump()
    {
        finish();
        closeHandle(error_);
    }

    // Waits for the engine's stderr to end, then returns that line without its newline; empty when there was none.
    std::string noGpuLine()
    {
        finish();
        return noGpu_;
    }

private:
    void finish()
    {
        if (thread_.joinable())
            thread_.join();
    }

    void run()
    {
        std::string line;
        char buffer[1024];
        long n;
        while ((n = readHandle(error_, buffer, sizeof buffer)) > 0) {
            std::fwrite(buffer, 1, size_t(n), stderr);
            for (long i = 0; i < n; ++i) {
                if (buffer[i] != '\n' && buffer[i] != '\r') {
                    line += buffer[i];
                    continue;
                }
                if (line.rfind(restart::kNoGpuLine, 0) == 0)
                    noGpu_ = line;
                line.clear();
            }
        }
        if (line.rfind(restart::kNoGpuLine, 0) == 0)
            noGpu_ = line;
    }

    NativeHandle error_;
    std::string noGpu_;
    std::thread thread_;
};

// The engine the UI started, the read end of its heartbeat pipe (closing that tells the engine to quit) and its
// stderr. One engine runs at a time. Shared so the spawn, alive, stop and refusal hooks all reach it.
struct StartedEngine {
    std::optional<Process> process;
    std::optional<NativeHandle> heartbeat;
    std::unique_ptr<ErrorPump> errors;
    // The pid of the last engine that exited for want of a GPU, and its line.
    int refusedPid = 0;
    std::string refusal;

    void forget()
    {
        if (heartbeat)
            closeHandle(*heartbeat);
        heartbeat.reset();
        process.reset();
        errors.reset();
    }

    // Called once the engine has exited.
    void noteRefusal()
    {
        if (process->poll() != restart::kNoGpu)
            return;
        refusedPid = process->pid();
        refusal = errors ? errors->noGpuLine() : "";
        if (refusal.empty())
            refusal = restart::kNoGpuLine + std::string("the engine did not say what is missing");
    }
};

// Reads away the beats an engine wrote, so its pipe never fills. Never blocks.
void drain(std::optional<NativeHandle> heartbeat)
{
    char beats[256];
    while (heartbeat && readAvailable(*heartbeat, beats, sizeof beats) > 0) {
    }
}

}

EngineLauncher::EngineLauncher(Hooks hooks) : hooks_(std::move(hooks)) {}

void EngineLauncher::ensureRunning()
{
    if (ownedPid_ != 0 || !failure_.empty())
        return;
    if (hooks_.reachable && hooks_.reachable())
        return; // An engine is already up; reuse it and never spawn a second.
    if (hooks_.spawn) {
        ownedPid_ = hooks_.spawn();
        if (ownedPid_ == 0)
            failure_ = "The engine could not be started.";
    }
}

void EngineLauncher::poll()
{
    if (ownedPid_ == 0 || !failure_.empty())
        return;
    if (!hooks_.alive || hooks_.alive(ownedPid_))
        return; // Still running.

    // The engine we started has died. Restart it, unless it refused to run or has already died too often in the last
    // minute.
    const int died = ownedPid_;
    ownedPid_ = 0;
    if (hooks_.refusal) {
        failure_ = hooks_.refusal(died);
        if (!failure_.empty())
            return;
    }
    const double t = hooks_.now ? hooks_.now() : 0.0;
    while (!restarts_.empty() && t - restarts_.front() > kWindowSeconds)
        restarts_.pop_front();
    if (int(restarts_.size()) >= kMaxRestarts) {
        failure_ = "The engine stopped and could not be kept running (it was restarted twice in a minute).";
        return;
    }
    restarts_.push_back(t);
    ownedPid_ = hooks_.spawn ? hooks_.spawn() : 0;
    if (ownedPid_ == 0)
        failure_ = "The engine could not be restarted.";
}

void EngineLauncher::shutdown()
{
    if (ownedPid_ != 0 && hooks_.stop)
        hooks_.stop(ownedPid_);
    ownedPid_ = 0;
}

std::vector<std::string> engineArgs(uint16_t port, const std::string& uiDisplay)
{
    std::vector<std::string> args{"--loopback", "--port", std::to_string(port)};
    if (!uiDisplay.empty())
        args.insert(args.end(), {"--ui-display", uiDisplay});
    return args;
}

EngineLauncher::Hooks defaultEngineHooks(uint16_t port, fs::path engine, std::function<std::string()> uiDisplay)
{
    if (engine.empty())
        engine = siblingExecutable("nocturne");
    auto started = std::make_shared<StartedEngine>();
    EngineLauncher::Hooks hooks;
    hooks.reachable = [port] { return portAnswers(port); };
    hooks.now = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    hooks.spawn = [port, started, engine, uiDisplay]() -> int {
        std::error_code ec;
        if (!fs::exists(engine, ec))
            return 0; // Not a single-bundle build (e.g. a dev build with separate bundles); do not spawn.
        const std::optional<PipeEnds> pipe = makePipe();
        if (!pipe)
            return 0;
        const std::optional<PipeEnds> errors = makePipe();
        if (!errors) {
            closeHandle(pipe->read);
            closeHandle(pipe->write);
            return 0;
        }
        // The engine listens on 127.0.0.1 only (never the LAN) on the port the UI connects to. It is NOT given
        // NOCTURNE_SUPERVISED, so it never exits to ask a supervisor to restart it; the UI is its only watchdog. It
        // writes a frame mark to the heartbeat and quits when the UI's read end closes, however the UI dies.
        std::optional<Process> process =
            spawnProcess({.program = engine,
                          .args = engineArgs(port, uiDisplay ? uiDisplay() : std::string()),
                          .error = Stdio::to(errors->write),
                          .heartbeat = pipe->write,
                          .environment = {{"NOCTURNE_SUPERVISED", std::nullopt}}});
        closeHandle(pipe->write);
        closeHandle(errors->write);
        if (!process) {
            closeHandle(pipe->read);
            closeHandle(errors->read);
            return 0;
        }
        started->forget(); // Any previous engine's pipes are done with.
        started->process = std::move(process);
        started->heartbeat = pipe->read;
        started->errors = std::make_unique<ErrorPump>(errors->read);
        return started->process->pid();
    };
    hooks.alive = [started](int pid) {
        drain(started->heartbeat);
        if (!started->process || started->process->pid() != pid || !started->process->poll())
            return true;
        started->noteRefusal();
        started->forget(); // The engine exited; let its pipes go.
        return false;
    };
    hooks.refusal = [started](int pid) { return pid == started->refusedPid ? started->refusal : std::string(); };
    hooks.stop = [started](int pid) {
        if (!started->process || started->process->pid() != pid)
            return;
        started->process->terminate();
        for (int i = 0; i < 30 && !started->process->poll(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        started->process->kill();
        started->process->wait();
        started->forget();
    };
    return hooks;
}

}
