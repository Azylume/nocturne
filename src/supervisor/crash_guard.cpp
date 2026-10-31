#include "supervisor/crash_guard.hpp"

#include "document/composition_file.hpp"
#include "platform/process.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace azl::crash {

namespace {

struct State {
    std::mutex mutex;
    std::filesystem::path marksFile;
    std::set<std::pair<std::string, std::string>> skips;
    std::vector<Mark> marks;
};

State& state()
{
    static State* s = new State;
    return *s;
}

std::atomic_flag crashRecorded;

#ifdef _WIN32
const HANDLE kNoCrashFile = INVALID_HANDLE_VALUE;
std::atomic<HANDLE> crashFile{kNoCrashFile};

uint64_t currentThread()
{
    return GetCurrentThreadId();
}

HANDLE openCrashFile(const std::filesystem::path& path)
{
    return path.empty() ? kNoCrashFile
                        : CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void closeCrashFile(HANDLE file)
{
    if (file != kNoCrashFile)
        CloseHandle(file);
}

void writeCrashFile(const char* text, size_t size)
{
    OVERLAPPED start{};
    DWORD written = 0;
    WriteFile(crashFile.load(), text, DWORD(size), &written, &start);
}

[[noreturn]] void killSelf()
{
    TerminateProcess(GetCurrentProcess(), 1);
    std::abort();
}
#else
constexpr int kNoCrashFile = -1;
std::atomic<int> crashFile{kNoCrashFile};

uint64_t currentThread()
{
    uint64_t thread = 0;
    pthread_threadid_np(nullptr, &thread);
    return thread;
}

int openCrashFile(const std::filesystem::path& path)
{
    return path.empty() ? kNoCrashFile : ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}

void closeCrashFile(int fd)
{
    if (fd != kNoCrashFile)
        ::close(fd);
}

void writeCrashFile(const char* text, size_t size)
{
    [[maybe_unused]] ssize_t written = ::pwrite(crashFile.load(), text, size, 0);
}

[[noreturn]] void killSelf()
{
    ::kill(getpid(), SIGKILL);
    std::abort();
}
#endif

nlohmann::json markJson(const Mark& m)
{
    return {{"kind", m.kind}, {"key", m.key}, {"name", m.name}, {"thread", m.thread}};
}

Mark markFrom(const nlohmann::json& j)
{
    return {j.value("kind", ""), j.value("key", ""), j.value("name", ""), j.value("thread", uint64_t(0))};
}

void append(char* text, size_t& n, const char* part)
{
    while (*part)
        text[n++] = *part++;
}

void append(char* text, size_t& n, uint64_t value)
{
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = char('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        text[n++] = digits[--count];
}

#ifdef _WIN32
// The C signal a structured exception stands for.
int signalFor(DWORD exception)
{
    switch (exception) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
        return SIGILL;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_STACK_CHECK:
        return SIGFPE;
    default:
        return SIGSEGV;
    }
}

// Runs on the crashing thread; the process then dies of the exception as it would have.
LONG WINAPI onCrash(EXCEPTION_POINTERS* info)
{
    recordCrash(signalFor(info->ExceptionRecord->ExceptionCode));
    return EXCEPTION_CONTINUE_SEARCH;
}

void onAbort(int signal)
{
    recordCrash(signal);
}
#else
void onCrash(int signal, siginfo_t* info, void*)
{
    recordCrash(signal);
    // SA_RESETHAND leaves SIGILL and SIGTRAP installed on macOS.
    ::signal(signal, SIG_DFL);
    // A fault returns to the faulting instruction and crashes under the default action; a sent signal is raised again.
    if (info->si_code == SI_USER || info->si_code == SI_QUEUE)
        ::raise(signal);
}
#endif

nlohmann::json readJson(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        return nullptr;
    return nlohmann::json::parse(in, nullptr, false);
}

// Under the lock.
void writeMarks(State& s)
{
    if (s.marksFile.empty())
        return;
    nlohmann::json marks = nlohmann::json::array();
    for (const Mark& m : s.marks)
        marks.push_back(markJson(m));
    try {
        writeFileAtomically(s.marksFile, nlohmann::json{{"pid", currentProcessId()}, {"marks", marks}}.dump() + "\n",
                            false);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "crash marks: %s\n", e.what());
    }
}

void injectFault(const Mark& mark)
{
    auto asked = [&](const char* variable) {
        const char* on = std::getenv(variable);
        return on && (mark.kind + ":" + mark.key == on || mark.kind + ":" + mark.name == on);
    };
    const bool here = asked("NOCTURNE_CRASH_ON");
    if (!here && !asked("NOCTURNE_CRASH_ELSEWHERE_ON"))
        return;
    std::fprintf(stderr, "NOCTURNE_CRASH_ON: killing the engine at %s %s%s\n", mark.kind.c_str(), mark.name.c_str(),
                 here ? "" : " from another thread");
    std::fflush(stderr);
    if (here)
        recordCrash(SIGSEGV);
    else
        std::thread([] { recordCrash(SIGSEGV); }).join();
    killSelf();
}

} // namespace

void setup(const std::filesystem::path& marksFile, const std::filesystem::path& skipFile,
           const std::filesystem::path& crashFileName)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    closeCrashFile(crashFile.exchange(openCrashFile(crashFileName)));
    crashRecorded.clear();
    s.marksFile = marksFile;
    s.skips.clear();
    for (const Skip& skip : readSkips(skipFile))
        s.skips.insert({skip.mark.kind, skip.mark.key});
    writeMarks(s);
}

bool skipped(const std::string& kind, const std::string& key)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.skips.contains({kind, key});
}

void recordCrash(int signal)
{
    if (crashFile.load() == kNoCrashFile || crashRecorded.test_and_set())
        return;
    char text[96];
    size_t n = 0;
    append(text, n, "{\"thread\":");
    append(text, n, currentThread());
    append(text, n, ",\"signal\":");
    append(text, n, uint64_t(signal));
    append(text, n, "}\n");
    writeCrashFile(text, n);
}

void installCrashHandlers()
{
#ifdef _WIN32
    // No error dialog holds a crashed engine open: it dies at once, and the supervisor restarts it.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(onCrash);
    std::signal(SIGABRT, onAbort);
#else
    struct sigaction action{};
    action.sa_sigaction = onCrash;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP})
        sigaction(signal, &action, nullptr);
#endif
}

Scope::Scope(std::string kind, std::string key, std::string name)
    : mark_{std::move(kind), std::move(key), std::move(name), currentThread()}
{
    State& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.marks.push_back(mark_);
        writeMarks(s);
    }
    injectFault(mark_);
}

Scope::~Scope()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    auto it = std::find_if(s.marks.begin(), s.marks.end(),
                           [&](const Mark& m) { return m.kind == mark_.kind && m.key == mark_.key && m.thread == mark_.thread; });
    if (it != s.marks.end())
        s.marks.erase(it);
    writeMarks(s);
}

std::vector<Mark> readMarks(const std::filesystem::path& marksFile)
{
    std::vector<Mark> marks;
    const nlohmann::json j = readJson(marksFile);
    if (!j.is_object())
        return marks;
    for (const nlohmann::json& m : j.value("marks", nlohmann::json::array()))
        if (m.is_object())
            marks.push_back(markFrom(m));
    return marks;
}

std::optional<Crash> readCrash(const std::filesystem::path& crashFile)
{
    const nlohmann::json j = readJson(crashFile);
    if (!j.is_object() || j.value("thread", uint64_t(0)) == 0)
        return std::nullopt;
    return Crash{j.value("thread", uint64_t(0)), j.value("signal", 0)};
}

std::vector<Mark> culprits(const std::vector<Mark>& marks, const std::optional<Crash>& crash)
{
    std::vector<Mark> held;
    if (!crash)
        return held;
    std::copy_if(marks.begin(), marks.end(), std::back_inserter(held),
                 [&](const Mark& m) { return m.thread == crash->thread; });
    return held;
}

std::vector<Skip> readSkips(const std::filesystem::path& skipFile)
{
    std::vector<Skip> skips;
    const nlohmann::json j = readJson(skipFile);
    if (!j.is_object())
        return skips;
    for (const nlohmann::json& s : j.value("skip", nlohmann::json::array()))
        if (s.is_object())
            skips.push_back({markFrom(s), s.value("reason", ""), s.value("at", 0.0)});
    return skips;
}

std::vector<Mark> addSkips(const std::filesystem::path& skipFile, const std::vector<Mark>& marks,
                           const std::string& reason)
{
    std::vector<Skip> skips = readSkips(skipFile);
    std::vector<Mark> added;
    for (const Mark& mark : marks) {
        const bool listed = std::any_of(skips.begin(), skips.end(), [&](const Skip& s) {
            return s.mark.kind == mark.kind && s.mark.key == mark.key;
        });
        if (listed || mark.key.empty())
            continue;
        skips.push_back({mark, reason, unixTime()});
        added.push_back(mark);
    }
    if (added.empty())
        return added;
    nlohmann::json list = nlohmann::json::array();
    for (const Skip& s : skips) {
        nlohmann::json entry = markJson(s.mark);
        entry["reason"] = s.reason;
        entry["at"] = s.at;
        list.push_back(entry);
    }
    writeFileAtomically(skipFile, nlohmann::json{{"skip", list}}.dump(2) + "\n");
    return added;
}

} // namespace azl::crash
