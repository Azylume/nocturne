#include "platform/run_command.hpp"

#include "platform/process.hpp"

#include <chrono>
#include <thread>

namespace azl {

namespace {

constexpr size_t kMaxOutput = size_t(64) << 10;

CommandResult failed(const std::string& reason)
{
    return {false, -1, reason};
}

} // namespace

CommandResult runCommand(const std::filesystem::path& program, const std::vector<std::string>& args, bool searchPath,
                         double seconds)
{
    const std::optional<PipeEnds> out = makePipe();
    if (!out)
        return failed("cannot create a pipe");
    std::string error;
    std::optional<Process> process = spawnProcess({.program = program,
                                                   .args = args,
                                                   .searchPath = searchPath,
                                                   .input = Stdio::null(),
                                                   .output = Stdio::to(out->write),
                                                   .error = Stdio::to(out->write)},
                                                  &error);
    closeHandle(out->write);
    if (!process) {
        closeHandle(out->read);
        return failed("cannot start " + program.string() + ": " + error);
    }
    CommandResult result;
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    char buffer[4096];
    while (true) {
        const long n = readAvailable(out->read, buffer, sizeof buffer);
        if (n > 0 && result.output.size() < kMaxOutput)
            result.output.append(buffer, size_t(n));
        if (n < 0)
            break;
        if (n == 0 && std::chrono::steady_clock::now() > until) {
            process->kill();
            process->wait();
            closeHandle(out->read);
            return failed(program.string() + " did not end within " + std::to_string(int(seconds)) + " s");
        }
        if (n == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    closeHandle(out->read);
    result.ended = true;
    result.exitCode = process->wait();
    return result;
}

} // namespace azl
