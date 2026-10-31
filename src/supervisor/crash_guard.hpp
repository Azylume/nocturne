#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Crash causes. Before a risky operation (opening a clip file, compiling an effect chain) the engine records what it
// is working on, and on which thread, in a marks file and removes it when done. A crash records the crashing thread.
// After a crash the supervisor adds the marks of the crashing thread to a skip list; marks on other threads are
// innocent. The engine never opens or compiles what is on the skip list again.
namespace azl::crash {

struct Mark {
    // "clip" (key: media hash) or "effect" (key: chain key).
    std::string kind;
    std::string key;
    // For people: the file name or the effect chain.
    std::string name;
    // The thread inside the operation: pthread_threadid_np, GetCurrentThreadId on Windows.
    uint64_t thread = 0;
};

struct Crash {
    uint64_t thread = 0;
    // On Windows the signal the structured exception stands for (SIGSEGV for an access violation).
    int signal = 0;
};

struct Skip {
    Mark mark;
    std::string reason;
    double at = 0.0;
};

// Engine side. Without setup, marks and crashes are not recorded and nothing is skipped. Clears both files.
void setup(const std::filesystem::path& marksFile, const std::filesystem::path& skipFile,
           const std::filesystem::path& crashFile = {});
bool skipped(const std::string& kind, const std::string& key);
// Records the calling thread as the crashing one; async-signal-safe, and only the first call counts.
void recordCrash(int signal);
// Installs handlers for the crash signals that record the crash and then let it proceed. On Windows an unhandled
// exception filter and SIGABRT do the same, and no error dialog keeps the crashed process alive.
void installCrashHandlers();

// Records the mark for as long as it lives. Fault injection for tests: with NOCTURNE_CRASH_ON=kind:key (or kind:name)
// in the environment, the process records a crash on the marking thread and kills itself while the mark is recorded;
// with NOCTURNE_CRASH_ELSEWHERE_ON the crash is recorded on another thread.
class Scope {
public:
    Scope(std::string kind, std::string key, std::string name);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Mark mark_;
};

// Supervisor side.
std::vector<Mark> readMarks(const std::filesystem::path& marksFile);
std::optional<Crash> readCrash(const std::filesystem::path& crashFile);
// The marks the crash happened inside: those held by the crashing thread. Nothing without a crash record.
std::vector<Mark> culprits(const std::vector<Mark>& marks, const std::optional<Crash>& crash);
std::vector<Skip> readSkips(const std::filesystem::path& skipFile);
// Adds the marks that are not listed yet; returns the ones it added.
std::vector<Mark> addSkips(const std::filesystem::path& skipFile, const std::vector<Mark>& marks,
                           const std::string& reason);

} // namespace azl::crash
