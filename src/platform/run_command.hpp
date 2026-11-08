#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace azl {

struct CommandResult {
    // False when the program could not be started or did not end in time; `output` then holds the reason.
    bool ended = false;
    int exitCode = -1;
    // Standard output and error together, at most 64 KiB.
    std::string output;
};

// Runs a program with fixed arguments, no shell and no input, and waits up to `seconds` for it to end; it is killed
// when it does not. `searchPath`: `program` is a name looked up on PATH. Any thread but the render thread.
CommandResult runCommand(const std::filesystem::path& program, const std::vector<std::string>& args, bool searchPath,
                         double seconds = 10.0);

} // namespace azl
