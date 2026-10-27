#include "ui/client/engine_launcher.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

extern char** environ;

namespace azl::ui {

namespace {

namespace fs = std::filesystem;

constexpr int kHeartbeatFd = 3; // The engine reads NOCTURNE_HEARTBEAT_FD and quits when its peer closes (the UI dies).

// The directory of the running executable (.../Contents/MacOS), so the UI finds the engine beside it in the one
// bundle, not in the build directory.
fs::path executableDir()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::path(buffer.data()), ec);
    if (ec)
        resolved = fs::path(buffer.data());
    return resolved.parent_path();
}

// True when something accepts a TCP connection on 127.0.0.1:port within a short window: an engine is already up.
bool portAnswers(uint16_t port)
{
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return false;
    const int flags = ::fcntl(sock, F_GETFL, 0);
    ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool answers = false;
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        answers = true;
    } else if (errno == EINPROGRESS) {
        pollfd pfd{sock, POLLOUT, 0};
        if (::poll(&pfd, 1, 300) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                answers = true;
        }
    }
    ::close(sock);
    return answers;
}

// The read end of the heartbeat pipe the UI holds for the engine it started; closing it tells the engine to quit. One
// engine runs at a time, so one fd is enough. Shared so the spawn, alive and stop hooks all reach it.
struct HeartbeatPipe {
    int readFd = -1;
    void close()
    {
        if (readFd >= 0)
            ::close(readFd);
        readFd = -1;
    }
};

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

    // The engine we started has died. Restart it, unless it has already died too often in the last minute.
    ownedPid_ = 0;
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

EngineLauncher::Hooks defaultEngineHooks(uint16_t port)
{
    auto pipe = std::make_shared<HeartbeatPipe>();
    EngineLauncher::Hooks hooks;
    hooks.reachable = [port] { return portAnswers(port); };
    hooks.now = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    hooks.spawn = [port, pipe]() -> pid_t {
        const fs::path engine = executableDir() / "nocturne";
        std::error_code ec;
        if (!fs::exists(engine, ec))
            return 0; // Not a single-bundle build (e.g. a dev build with separate bundles); do not spawn.

        int fds[2];
        if (::pipe(fds) != 0)
            return 0;
        // The UI keeps the read end past a fork, the child only gets the write end as fd 3; the engine writes a frame
        // mark to it and quits when the read end closes, which happens when the UI process dies for any reason.
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], kHeartbeatFd);

        const std::string path = engine.string();
        std::string portText = std::to_string(port);
        // The engine listens on 127.0.0.1 only (never the LAN) on the port the UI connects to. It is NOT given
        // NOCTURNE_SUPERVISED, so it never exits to ask a supervisor to restart it; the UI is its only watchdog.
        char loopbackArg[] = "--loopback";
        char portArg[] = "--port";
        char* argv[] = {const_cast<char*>(path.c_str()), loopbackArg, portArg, portText.data(), nullptr};

        std::vector<char*> env;
        const std::string heartbeat = "NOCTURNE_HEARTBEAT_FD=" + std::to_string(kHeartbeatFd);
        for (char** e = environ; *e; ++e)
            if (std::strncmp(*e, "NOCTURNE_HEARTBEAT_FD=", 22) != 0 &&
                std::strncmp(*e, "NOCTURNE_SUPERVISED=", 20) != 0)
                env.push_back(*e);
        env.push_back(const_cast<char*>(heartbeat.c_str()));
        env.push_back(nullptr);

        pid_t pid = 0;
        const int rc = ::posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, env.data());
        posix_spawn_file_actions_destroy(&actions);
        ::close(fds[1]); // The child holds the write end now.
        if (rc != 0) {
            ::close(fds[0]);
            return 0;
        }
        pipe->close(); // Any previous engine's pipe is done with.
        pipe->readFd = fds[0];
        return pid;
    };
    hooks.alive = [pipe](pid_t pid) {
        if (::waitpid(pid, nullptr, WNOHANG) == pid) {
            pipe->close(); // The engine exited; let its heartbeat pipe go.
            return false;
        }
        return true;
    };
    hooks.stop = [pipe](pid_t pid) {
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 30; ++i) {
            if (::waitpid(pid, nullptr, WNOHANG) == pid) {
                pipe->close();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        pipe->close();
    };
    return hooks;
}

}
