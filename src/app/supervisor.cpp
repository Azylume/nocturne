#include "document/composition_file.hpp"
#include "platform/app_paths.hpp"
#include "platform/process.hpp"
#include "supervisor/crash_guard.hpp"
#include "supervisor/restart_request.hpp"
#include "supervisor/rotating_log.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* kUsage = R"(usage: nocturne-supervisor [options] [-- engine options]
Runs the engine, restarts it when it crashes or hangs, and switches to safe mode after too many crashes. Runs the
venue desk beside it and restarts it when it stops; the desk never holds up the engine.
  --engine PATH          engine binary (default: nocturne next to this binary)
  --state DIR            composition, skip list and status (default ~/Library/Application Support/Nocturne/State)
  --logs DIR             supervisor.log and engine.log (default ~/Library/Logs/Nocturne)
  --hang-timeout S       seconds without a finished frame that count as hung (default 2)
  --startup-timeout S    seconds the engine may take to its first frame (default 30)
  --crash-limit N        crashes within the window that start safe mode (default 3)
  --crash-window S       (default 60)
  --fallback-image FILE  the picture safe mode shows instead of black
  --desk PATH|off        the venue desk binary (default: nocturne-desk next to this binary), or none
  --desk-port N          the desk's HTTP port (default 7480; 0 picks a free one)
  --desk-loopback        the desk listens on 127.0.0.1 alone (tests)
  --clear-skips          empty the skip list and quit
)";

struct Options {
    fs::path engine;
    fs::path state;
    fs::path logs;
    double hangTimeout = 2.0;
    double startupTimeout = 30.0;
    int crashLimit = 3;
    double crashWindow = 60.0;
    std::string fallbackImage;
    bool clearSkips = false;
    fs::path desk;
    int deskPort = 7480;
    bool deskLoopback = false;
    std::vector<std::string> engineArgs;
};

[[noreturn]] void usage(int code)
{
    std::fputs(kUsage, code ? stderr : stdout);
    std::exit(code);
}

fs::path ownDirectory()
{
    std::array<char, PATH_MAX> path{};
    uint32_t size = path.size();
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return ".";
    return fs::weakly_canonical(path.data()).parent_path();
}

Options parse(int argc, char** argv)
{
    Options o;
    o.engine = ownDirectory() / "nocturne";
    o.desk = ownDirectory() / "nocturne-desk";
    o.state = azl::appDataFolder() / "State";
    o.logs = azl::appLogsFolder();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&] {
            if (i + 1 >= argc)
                usage(2);
            return argv[++i];
        };
        if (arg == "--") {
            o.engineArgs.assign(argv + i + 1, argv + argc);
            break;
        }
        if (arg == "--engine") o.engine = value();
        else if (arg == "--state") o.state = value();
        else if (arg == "--logs") o.logs = value();
        else if (arg == "--hang-timeout") o.hangTimeout = std::atof(value());
        else if (arg == "--startup-timeout") o.startupTimeout = std::atof(value());
        else if (arg == "--crash-limit") o.crashLimit = std::atoi(value());
        else if (arg == "--crash-window") o.crashWindow = std::atof(value());
        else if (arg == "--fallback-image") o.fallbackImage = value();
        else if (arg == "--clear-skips") o.clearSkips = true;
        else if (arg == "--desk") o.desk = std::string(value()) == "off" ? fs::path() : fs::path(argv[i]);
        else if (arg == "--desk-port") o.deskPort = std::atoi(value());
        else if (arg == "--desk-loopback") o.deskLoopback = true;
        else if (arg == "--help" || arg == "-h") usage(0);
        else usage(2);
    }
    return o;
}

std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_r(&t, &local);
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &local);
    char out[40];
    std::snprintf(out, sizeof out, "%s.%03d", text, ms);
    return out;
}

double seconds(Clock::duration d)
{
    return std::chrono::duration<double>(d).count();
}

std::string tenths(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", value);
    return text;
}

int signalPipe[2] = {-1, -1};

void onSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] ssize_t n = ::write(signalPipe[1], &byte, 1);
}

void setCloexec(int fd)
{
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// Copies the engine's output into engine.log line by line, each line stamped with the time it arrived.
class OutputPump {
public:
    OutputPump(int fd, azl::RotatingLog& log) : fd_(fd), log_(log), thread_([this] { run(); }) {}
    ~OutputPump()
    {
        thread_.join();
        ::close(fd_);
    }

private:
    void run()
    {
        std::string line;
        char buffer[4096];
        while (true) {
            const ssize_t n = ::read(fd_, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n; ++i) {
                line += buffer[i];
                if (buffer[i] == '\n') {
                    log_.write(timestamp() + " " + line);
                    line.clear();
                }
            }
        }
        if (!line.empty())
            log_.write(timestamp() + " " + line + "\n");
    }

    int fd_;
    azl::RotatingLog& log_;
    std::thread thread_;
};

std::string describe(int status)
{
    if (WIFEXITED(status))
        return "exited with status " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        const char* name = sig > 0 && sig < NSIG ? sys_signame[sig] : "?";
        std::string upper = name;
        for (char& c : upper)
            c = char(std::toupper(c));
        return "killed by SIG" + upper;
    }
    return "stopped with status " + std::to_string(status);
}

bool exitedWith(int status, int code)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == code;
}

std::string describe(const std::vector<azl::crash::Mark>& marks)
{
    std::string text;
    for (const azl::crash::Mark& m : marks)
        text += std::string(text.empty() ? "" : ", ") + (m.kind == "clip" ? "opening clip " : "compiling effect chain ") +
                m.name;
    return text;
}

class Supervisor {
public:
    explicit Supervisor(Options options)
        : opt_(std::move(options)), log_(opt_.logs / "supervisor.log", uint64_t(16) << 20),
          engineLog_(opt_.logs / "engine.log", uint64_t(32) << 20)
    {
    }

    int run()
    {
        say("supervisor started (pid " + std::to_string(getpid()) + "), engine " + opt_.engine.string() + ", state " +
            opt_.state.string());
        killStale("engine.pid", opt_.engine, "engine");
        if (!opt_.desk.empty()) {
            killStale("desk.pid", opt_.desk, "desk");
            desk_ = std::thread([this] { runDesk(); });
        }
        while (!stopping_) {
            if (safeReason_.empty())
                runEngine();
            else
                runSafeMode();
        }
        if (desk_.joinable())
            desk_.join();
        say("supervisor stopped");
        writeStatus(0);
        return 0;
    }

private:
    void say(const std::string& line)
    {
        const std::string text = timestamp() + " " + line + "\n";
        log_.write(text);
        std::fputs(text.c_str(), stderr);
    }

    fs::path statePath(const char* name) const { return opt_.state / name; }

    void writeStatus(pid_t pid)
    {
        nlohmann::json crashes = nlohmann::json::array();
        for (const std::string& c : crashLog_)
            crashes.push_back(c);
        const nlohmann::json status{{"mode", safeReason_.empty() ? "normal" : "safe"},
                                    {"reason", safeReason_},
                                    {"engine", pid},
                                    {"starts", starts_},
                                    {"crashes", crashes},
                                    {"supervisor", getpid()}};
        try {
            azl::writeFileAtomically(statePath("status.json"), status.dump(2) + "\n", false);
        } catch (const std::exception& e) {
            say(std::string("status: ") + e.what());
        }
    }

    // An engine or desk left behind by a supervisor that died is killed, so it never holds the displays and the port.
    void killStale(const char* pidFile, const fs::path& binary, const char* what)
    {
        std::ifstream in(statePath(pidFile));
        pid_t pid = 0;
        if (!(in >> pid) || pid <= 1)
            return;
        std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
        if (proc_pidpath(pid, path.data(), uint32_t(path.size())) <= 0)
            return;
        if (fs::path(path.data()).filename() != binary.filename())
            return;
        say(std::string("killing the ") + what + " a previous supervisor left running (pid " + std::to_string(pid) + ")");
        ::kill(pid, SIGKILL);
        for (int i = 0; i < 50 && ::kill(pid, 0) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Spawns the engine with its output on a pipe, and the heartbeat pipe's write end as fd 3 when `heartbeat` is set.
    std::optional<pid_t> spawn(const std::vector<std::string>& args, int heartbeat, int& output)
    {
        return spawn(opt_.engine, args, heartbeat, output, "engine.pid");
    }

    std::optional<pid_t> spawn(const fs::path& binary, const std::vector<std::string>& args, int heartbeat, int& output,
                               const char* pidFile)
    {
        const std::optional<azl::PipeEnds> out = azl::makePipe();
        if (!out)
            return std::nullopt;
        azl::SpawnOptions options{.program = binary,
                                  .args = args,
                                  .input = azl::Stdio::null(),
                                  .output = azl::Stdio::to(out->write),
                                  .error = azl::Stdio::to(out->write),
                                  .environment = {{"NOCTURNE_RESPONSIBLE", std::nullopt},
                                                  {"NOCTURNE_HEARTBEAT_FD", std::nullopt},
                                                  {azl::restart::kSupervisedVariable, "1"}}};
        if (heartbeat >= 0)
            options.heartbeat = heartbeat;
        std::string error;
        const std::optional<azl::Process> process = azl::spawnProcess(options, &error);
        azl::closeHandle(out->write);
        if (!process) {
            azl::closeHandle(out->read);
            say("cannot start " + binary.string() + ": " + error);
            return std::nullopt;
        }
        const pid_t pid = process->pid();
        output = out->read;
        std::ofstream(statePath(pidFile)) << pid << "\n";
        return pid;
    }

    // Asks the engine to quit and kills it when it does not within 3 s.
    int stop(pid_t pid)
    {
        ::kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 150; ++i) {
            if (::waitpid(pid, &status, WNOHANG) == pid)
                return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        say("engine did not quit within 3 s, killing it");
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        return status;
    }

    bool drainSignals(int fd)
    {
        char buffer[64];
        bool any = false;
        while (::read(fd, buffer, sizeof buffer) > 0)
            any = true;
        return any;
    }

    void runEngine()
    {
        int heartbeat[2];
        if (::pipe(heartbeat) != 0) {
            say("cannot create the heartbeat pipe");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            return;
        }
        setCloexec(heartbeat[0]);
        ::fcntl(heartbeat[0], F_SETFL, O_NONBLOCK);
        std::vector<std::string> args = opt_.engineArgs;
        args.insert(args.end(), {"--state", opt_.state.string()});
        int output = -1;
        const auto started = Clock::now();
        const std::optional<pid_t> pid = spawn(args, heartbeat[1], output);
        ::close(heartbeat[1]);
        if (!pid) {
            ::close(heartbeat[0]);
            crashed("could not be started", {}, std::nullopt);
            return;
        }
        ++starts_;
        say("engine started (pid " + std::to_string(*pid) + ")");
        writeStatus(*pid);
        OutputPump pump(output, engineLog_);

        std::optional<Clock::time_point> lastBeat;
        std::string hang;
        int status = 0;
        while (true) {
            pollfd fds[2] = {{heartbeat[0], POLLIN, 0}, {signalPipe[0], POLLIN, 0}};
            ::poll(fds, 2, 100);
            if (drainSignals(signalPipe[0])) {
                stopping_ = true;
                say("stopping the engine");
                status = stop(*pid);
                break;
            }
            char beats[256];
            const ssize_t n = ::read(heartbeat[0], beats, sizeof beats);
            const auto now = Clock::now();
            if (n > 0) {
                if (!lastBeat)
                    say("engine running, first frame " + tenths(seconds(now - started)) + " s after the start");
                lastBeat = now;
            }
            if (::waitpid(*pid, &status, WNOHANG) == *pid)
                break;
            if (lastBeat && seconds(now - *lastBeat) > opt_.hangTimeout)
                hang = "hung: no frame for " + tenths(seconds(now - *lastBeat)) + " s";
            else if (!lastBeat && seconds(now - started) > opt_.startupTimeout)
                hang = "hung: no frame " + std::to_string(int(opt_.startupTimeout)) + " s after the start";
            if (!hang.empty()) {
                ::kill(*pid, SIGKILL);
                ::waitpid(*pid, &status, 0);
                break;
            }
        }
        ::close(heartbeat[0]);
        std::error_code ec;
        fs::remove(statePath("engine.pid"), ec);
        const std::optional<azl::crash::Crash> crash = azl::crash::readCrash(statePath("crash.json"));
        fs::remove(statePath("crash.json"), ec);

        if (stopping_) {
            say("engine " + describe(status));
            return;
        }
        if (hang.empty() && exitedWith(status, azl::restart::kStop)) {
            stopping_ = true;
            say("stop asked for from a client; stopping");
            return;
        }
        if (hang.empty() && exitedWith(status, azl::restart::kNormal)) {
            say("engine restart asked for from a client");
            return;
        }
        if (hang.empty() && exitedWith(status, azl::restart::kSafeMode)) {
            safeReason_ = "asked for from a client";
            say("safe mode: " + safeReason_);
            writeStatus(0);
            return;
        }
        if (hang.empty() && exitedWith(status, 0)) {
            say("engine quit (" + describe(status) + "), restarting");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            return;
        }
        crashed(hang.empty() ? describe(status) : hang, azl::crash::readMarks(statePath("inflight.json")), crash);
    }

    // Only what the crashing thread was opening or compiling is skipped; a hang or a crash elsewhere skips nothing.
    void crashed(const std::string& how, const std::vector<azl::crash::Mark>& marks,
                 const std::optional<azl::crash::Crash>& crash)
    {
        const std::vector<azl::crash::Mark> culprits = azl::crash::culprits(marks, crash);
        const std::string cause = culprits.empty() ? "" : " while " + describe(culprits);
        say("engine " + how + cause);
        if (marks.size() > culprits.size()) {
            std::vector<azl::crash::Mark> elsewhere;
            std::copy_if(marks.begin(), marks.end(), std::back_inserter(elsewhere),
                         [&](const azl::crash::Mark& m) { return !crash || m.thread != crash->thread; });
            const std::string where = crash ? "on thread " + std::to_string(crash->thread) : "with no crash record";
            say("skip list: not adding " + describe(elsewhere) + ", the crash (" + where +
                ") was not inside it");
        }
        if (!culprits.empty()) {
            try {
                for (const azl::crash::Mark& m : azl::crash::addSkips(statePath("skip.json"), culprits, how + cause))
                    say("skip list: added " + m.kind + " " + m.name + " (" + m.key + "); it is skipped from now on");
            } catch (const std::exception& e) {
                say(std::string("skip list: ") + e.what());
            }
        }
        const auto now = Clock::now();
        crashes_.push_back(now);
        crashLog_.push_back(timestamp() + " " + how + cause);
        while (crashLog_.size() > 20)
            crashLog_.pop_front();
        while (!crashes_.empty() && seconds(now - crashes_.front()) > opt_.crashWindow)
            crashes_.pop_front();
        if (int(crashes_.size()) >= opt_.crashLimit) {
            safeReason_ = std::to_string(crashes_.size()) + " crashes within " + std::to_string(int(opt_.crashWindow)) +
                          " s; last: " + how + cause;
            say("safe mode: " + safeReason_);
        } else {
            say("restarting the engine");
        }
        writeStatus(0);
    }

    // Safe mode lasts until the supervisor restarts. It is restarted itself if it quits.
    void runSafeMode()
    {
        std::vector<std::string> args = opt_.engineArgs;
        args.insert(args.end(), {"--safe-mode", safeReason_, "--state", opt_.state.string()});
        if (!opt_.fallbackImage.empty())
            args.insert(args.end(), {"--fallback-image", opt_.fallbackImage});
        int output = -1;
        const std::optional<pid_t> pid = spawn(args, -1, output);
        if (!pid) {
            pollfd fd{signalPipe[0], POLLIN, 0};
            ::poll(&fd, 1, 2000);
            stopping_ = drainSignals(signalPipe[0]);
            return;
        }
        say("safe mode started (pid " + std::to_string(*pid) + ")");
        writeStatus(*pid);
        OutputPump pump(output, engineLog_);
        int status = 0;
        while (true) {
            pollfd fd{signalPipe[0], POLLIN, 0};
            ::poll(&fd, 1, 200);
            if (drainSignals(signalPipe[0])) {
                stopping_ = true;
                status = stop(*pid);
                break;
            }
            if (::waitpid(*pid, &status, WNOHANG) == *pid)
                break;
        }
        std::error_code ec;
        fs::remove(statePath("engine.pid"), ec);
        say("safe mode " + describe(status));
        if (!stopping_ && exitedWith(status, azl::restart::kNormal)) {
            say("leaving safe mode, asked for from a client");
            safeReason_.clear();
            crashes_.clear();
            writeStatus(0);
            return;
        }
        if (!stopping_)
            std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // The engine's protocol port, from its command line.
    std::string enginePort() const
    {
        for (size_t i = 0; i + 1 < opt_.engineArgs.size(); ++i)
            if (opt_.engineArgs[i] == "--port")
                return opt_.engineArgs[i + 1];
        return "7400";
    }

    // Keeps the desk running until the supervisor stops; it waits longer after each quick exit, up to 10 s.
    void runDesk()
    {
        std::vector<std::string> args = {"--port", std::to_string(opt_.deskPort), "--engine-port", enginePort()};
        if (opt_.deskLoopback)
            args.push_back("--loopback-only");
        double wait = 1.0;
        while (!stopping_) {
            int output = -1;
            const auto started = Clock::now();
            const std::optional<pid_t> pid = spawn(opt_.desk, args, -1, output, "desk.pid");
            int status = 0;
            if (pid) {
                say("desk started (pid " + std::to_string(*pid) + ")");
                OutputPump pump(output, deskLog_);
                bool reaped = false;
                while (!stopping_ && !(reaped = ::waitpid(*pid, &status, WNOHANG) == *pid))
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!reaped) {
                    ::kill(*pid, SIGTERM);
                    for (int i = 0; i < 50 && !(reaped = ::waitpid(*pid, &status, WNOHANG) == *pid); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (!reaped) {
                    ::kill(*pid, SIGKILL);
                    ::waitpid(*pid, &status, 0);
                }
                std::error_code ec;
                fs::remove(statePath("desk.pid"), ec);
            }
            if (stopping_)
                break;
            say("desk " + (pid ? describe(status) : std::string("could not be started")) + ", restarting");
            wait = seconds(Clock::now() - started) > 30.0 ? 1.0 : std::min(wait * 2.0, 10.0);
            const auto until = Clock::now() + std::chrono::duration<double>(wait);
            while (!stopping_ && Clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    Options opt_;
    azl::RotatingLog log_;
    azl::RotatingLog engineLog_;
    azl::RotatingLog deskLog_{opt_.logs / "desk.log", uint64_t(4) << 20};
    std::atomic<bool> stopping_ = false;
    std::thread desk_;
    std::deque<Clock::time_point> crashes_;
    std::deque<std::string> crashLog_;
    std::string safeReason_;
    int starts_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    Options opt = parse(argc, argv);
    std::error_code ec;
    fs::create_directories(opt.state, ec);
    if (opt.clearSkips) {
        fs::remove(opt.state / "skip.json", ec);
        std::printf("skip list cleared\n");
        return 0;
    }
    if (::pipe(signalPipe) != 0)
        return 1;
    setCloexec(signalPipe[0]);
    setCloexec(signalPipe[1]);
    ::fcntl(signalPipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(signalPipe[1], F_SETFL, O_NONBLOCK);
    std::signal(SIGPIPE, SIG_IGN);
    struct sigaction action{};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGTERM, SIGINT, SIGHUP})
        sigaction(sig, &action, nullptr);
    try {
        return Supervisor(std::move(opt)).run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nocturne-supervisor: %s\n", e.what());
        return 1;
    }
}
