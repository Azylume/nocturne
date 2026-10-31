#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// A file descriptor on POSIX, a HANDLE on Windows.
#ifdef _WIN32
using NativeHandle = void*;
#else
using NativeHandle = int;
#endif

struct PipeEnds {
    NativeHandle read;
    NativeHandle write;
};

// A pipe whose ends no child inherits unless it is handed to one (SpawnOptions). Nothing when the system refuses.
std::optional<PipeEnds> makePipe();
// Blocking. The bytes read, 0 at the end, -1 on an error.
long readHandle(NativeHandle handle, void* buffer, size_t size);
long writeHandle(NativeHandle handle, const void* data, size_t size);
void closeHandle(NativeHandle handle);

// Where one of the child's standard streams goes.
struct Stdio {
    enum class Kind { Inherit, Null, Handle };
    Kind kind = Kind::Inherit;
    NativeHandle handle{};

    static Stdio null() { return {Kind::Null, {}}; }
    static Stdio to(NativeHandle handle) { return {Kind::Handle, handle}; }
};

struct SpawnOptions {
    // An absolute path, or with searchPath a name looked up on PATH.
    std::filesystem::path program;
    std::vector<std::string> args;
    bool searchPath = false;
    Stdio input, output, error;
    // The write end of a heartbeat pipe the child writes to. POSIX: its fd 3, with NOCTURNE_HEARTBEAT_FD=3. Windows: an
    // inherited handle, with NOCTURNE_HEARTBEAT_HANDLE set to its value. inheritedHeartbeat() finds it in the child.
    std::optional<NativeHandle> heartbeat;
    // Variables set over this process's environment; a missing value removes the variable.
    std::map<std::string, std::optional<std::string>> environment;
    // The child outlives this process: a session of its own on POSIX; on Windows it stays out of the job that ends
    // every other child with this process.
    bool detached = false;
};

// A started child. The child keeps running when this is destroyed; on POSIX nobody reaps it then.
class Process {
public:
    Process() = default;
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    ~Process();

    int pid() const { return pid_; }
    // Never blocks. The exit code once the child has ended (-1 when a signal ended it), nothing while it runs.
    std::optional<int> poll();
    // Blocks until the child ends; its exit code, -1 when a signal ended it.
    int wait();
    // Asks the child to quit (SIGTERM). On Windows there is no such request yet: it ends the child like kill().
    void terminate();
    void kill();

private:
    friend std::optional<Process> spawnProcess(const SpawnOptions& options, std::string* error);
    int pid_ = 0;
    std::optional<int> exit_;
#ifdef _WIN32
    void* handle_ = nullptr;
#endif
};

// Starts the program with only the handles the options name (every other descriptor closes in the child). Nothing and
// the reason in `error` when it cannot start. Any thread but the render thread.
std::optional<Process> spawnProcess(const SpawnOptions& options, std::string* error = nullptr);

// In a child started with a heartbeat: the handle to write beats to.
std::optional<NativeHandle> inheritedHeartbeat();

} // namespace azl
