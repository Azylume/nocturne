#include "platform/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

extern char** environ;

namespace azl {

namespace {

constexpr int kHeartbeatFd = 3;

int stopPipe[2] = {-1, -1};
std::atomic<bool> stopAsked = false;

void onStopSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] ssize_t n = ::write(stopPipe[1], &byte, 1);
}

void redirect(posix_spawn_file_actions_t& actions, const Stdio& stdio, int fd)
{
    switch (stdio.kind) {
    case Stdio::Kind::Inherit: posix_spawn_file_actions_addinherit_np(&actions, fd); break;
    case Stdio::Kind::Null: posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0); break;
    case Stdio::Kind::Handle: posix_spawn_file_actions_adddup2(&actions, stdio.handle, fd); break;
    }
}

std::vector<std::string> environmentFor(const SpawnOptions& options)
{
    std::map<std::string, std::optional<std::string>> overrides = options.environment;
    if (options.heartbeat)
        overrides["NOCTURNE_HEARTBEAT_FD"] = std::to_string(kHeartbeatFd);
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        const std::string_view entry(*e);
        if (!overrides.contains(std::string(entry.substr(0, entry.find('=')))))
            env.emplace_back(entry);
    }
    for (const auto& [name, value] : overrides)
        if (value)
            env.push_back(name + "=" + *value);
    return env;
}

std::vector<char*> pointers(std::vector<std::string>& strings)
{
    std::vector<char*> out;
    for (std::string& s : strings)
        out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

} // namespace

std::optional<PipeEnds> makePipe()
{
    int fds[2];
    if (::pipe(fds) != 0)
        return std::nullopt;
    for (int fd : fds)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return PipeEnds{fds[0], fds[1]};
}

long readHandle(NativeHandle handle, void* buffer, size_t size)
{
    ssize_t n;
    while ((n = ::read(handle, buffer, size)) < 0 && errno == EINTR) {
    }
    return long(n);
}

long writeHandle(NativeHandle handle, const void* data, size_t size)
{
    ssize_t n;
    while ((n = ::write(handle, data, size)) < 0 && errno == EINTR) {
    }
    return long(n);
}

long readAvailable(NativeHandle handle, void* buffer, size_t size)
{
    pollfd fd{handle, POLLIN, 0};
    if (::poll(&fd, 1, 0) <= 0)
        return 0;
    const long n = readHandle(handle, buffer, size);
    return n > 0 ? n : -1;
}

void closeHandle(NativeHandle handle)
{
    ::close(handle);
}

std::optional<Process> spawnProcess(const SpawnOptions& options, std::string* error)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    redirect(actions, options.input, 0);
    redirect(actions, options.output, 1);
    redirect(actions, options.error, 2);
    if (options.heartbeat)
        posix_spawn_file_actions_adddup2(&actions, *options.heartbeat, kHeartbeatFd);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF |
                                              (options.detached ? POSIX_SPAWN_SETSID : 0));
    sigset_t handled;
    sigemptyset(&handled);
    for (int sig : {SIGPIPE, SIGTERM, SIGINT, SIGHUP})
        sigaddset(&handled, sig);
    posix_spawnattr_setsigdefault(&attributes, &handled);

    std::vector<std::string> args{options.program.string()};
    args.insert(args.end(), options.args.begin(), options.args.end());
    std::vector<std::string> env = environmentFor(options);
    std::vector<char*> argv = pointers(args), envp = pointers(env);

    pid_t pid = 0;
    const int failed = options.searchPath
                           ? posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), envp.data())
                           : posix_spawn(&pid, argv[0], &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (failed) {
        if (error)
            *error = std::strerror(failed);
        return std::nullopt;
    }
    Process process;
    process.pid_ = pid;
    return process;
}

Process::Process(Process&& other) noexcept
    : pid_(std::exchange(other.pid_, 0)), exit_(other.exit_), signal_(other.signal_)
{
}

Process& Process::operator=(Process&& other) noexcept
{
    pid_ = std::exchange(other.pid_, 0);
    exit_ = other.exit_;
    signal_ = other.signal_;
    return *this;
}

Process::~Process() = default;

void Process::ended(int status)
{
    exit_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    signal_ = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}

std::optional<int> Process::poll()
{
    int status = 0;
    if (!exit_ && pid_ > 0 && ::waitpid(pid_, &status, WNOHANG) == pid_)
        ended(status);
    return exit_;
}

int Process::wait()
{
    int status = 0;
    while (!exit_ && pid_ > 0) {
        if (::waitpid(pid_, &status, 0) == pid_)
            ended(status);
        else if (errno != EINTR)
            exit_ = -1;
    }
    return exit_.value_or(-1);
}

std::string Process::describeExit() const
{
    if (!exit_)
        return {};
    if (signal_ <= 0 || signal_ >= NSIG)
        return "exited with status " + std::to_string(*exit_);
    std::string name = sys_signame[signal_];
    for (char& c : name)
        c = char(std::toupper(c));
    return "killed by SIG" + name;
}

void Process::terminate()
{
    if (!exit_ && pid_ > 0)
        ::kill(pid_, SIGTERM);
}

void Process::kill()
{
    if (!exit_ && pid_ > 0)
        ::kill(pid_, SIGKILL);
}

std::optional<NativeHandle> inheritedHeartbeat()
{
    const char* fd = std::getenv("NOCTURNE_HEARTBEAT_FD");
    if (!fd)
        return std::nullopt;
    return std::atoi(fd);
}

int currentProcessId()
{
    return int(::getpid());
}

bool watchStopRequests()
{
    if (::pipe(stopPipe) != 0)
        return false;
    for (int fd : stopPipe) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    std::signal(SIGPIPE, SIG_IGN);
    struct sigaction action{};
    action.sa_handler = onStopSignal;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGTERM, SIGINT, SIGHUP})
        sigaction(sig, &action, nullptr);
    return true;
}

bool waitForStop(double seconds, std::optional<NativeHandle> readable)
{
    pollfd fds[2] = {{stopPipe[0], POLLIN, 0}, {readable.value_or(-1), POLLIN, 0}};
    ::poll(fds, readable ? 2 : 1, int(seconds * 1000.0));
    char buffer[64];
    while (stopPipe[0] >= 0 && ::read(stopPipe[0], buffer, sizeof buffer) > 0)
        stopAsked = true;
    return stopAsked;
}

} // namespace azl
