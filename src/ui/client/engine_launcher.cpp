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
#include <filesystem>
#include <thread>
#include <vector>

extern char** environ;

namespace azl::ui {

namespace {

namespace fs = std::filesystem;

// The directory of the running executable (.../Contents/MacOS), so the UI finds the supervisor beside it in the one
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

}

EngineLauncher::EngineLauncher(Hooks hooks) : hooks_(std::move(hooks)) {}

void EngineLauncher::ensureRunning()
{
    if (ensured_)
        return;
    ensured_ = true;
    if (hooks_.reachable && hooks_.reachable())
        return; // An engine is already up; reuse it and never spawn a second.
    if (hooks_.spawn)
        ownedPid_ = hooks_.spawn();
}

void EngineLauncher::shutdown()
{
    if (ownedPid_ != 0 && hooks_.stop)
        hooks_.stop(ownedPid_);
    ownedPid_ = 0;
}

EngineLauncher::Hooks defaultEngineHooks(uint16_t port)
{
    EngineLauncher::Hooks hooks;
    hooks.reachable = [port] { return portAnswers(port); };
    hooks.spawn = [] () -> pid_t {
        const fs::path supervisor = executableDir() / "nocturne-supervisor";
        std::error_code ec;
        if (!fs::exists(supervisor, ec))
            return 0; // Not a single-bundle build (e.g. a dev build with separate bundles); do not spawn.
        const std::string path = supervisor.string();
        char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
        pid_t pid = 0;
        if (::posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, environ) != 0)
            return 0;
        return pid;
    };
    hooks.stop = [] (pid_t pid) {
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 30; ++i) {
            if (::waitpid(pid, nullptr, WNOHANG) == pid)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
    };
    return hooks;
}

}
