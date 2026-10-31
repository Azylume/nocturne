#pragma once

#include "platform/process.hpp"

#include <doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Where the harness tests find the binaries and source files they use: the build on the laptop, or what
// tools/deploy-mini.sh ships on the Mini, named by the environment.
namespace {

[[maybe_unused]] std::filesystem::path sourceDir()
{
    const char* dir = std::getenv("NOCTURNE_SOURCE_DIR");
    return dir ? dir : NOCTURNE_SOURCE_DIR;
}

[[maybe_unused]] std::filesystem::path bundleBinary(const std::string& name)
{
    const char* path = std::getenv("NOCTURNE_SUPERVISOR");
    const std::filesystem::path binary = std::filesystem::path(path ? path : NOCTURNE_SUPERVISOR).parent_path() / name;
#ifdef _WIN32
    return binary.string() + ".exe";
#else
    return binary;
#endif
}

[[maybe_unused]] std::filesystem::path ctlPath()
{
    return bundleBinary("nocturne-ctl");
}

struct CtlOutput {
    int code = -1;
    // Its standard output and error, as they came.
    std::string output;
};

// `args` split at spaces, single quotes grouping words as in a shell. A backslash is an ordinary character, as in a
// Windows path.
[[maybe_unused]] std::vector<std::string> shellWords(const std::string& args)
{
    std::vector<std::string> words;
    std::istringstream in(args);
    for (std::string word; in >> std::quoted(word, '\'', '\0');)
        words.push_back(word);
    return words;
}

// Runs a program with its standard output and error in one pipe and waits for it.
[[maybe_unused]] CtlOutput runProgram(azl::SpawnOptions options)
{
    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
    options.output = azl::Stdio::to(pipe->write);
    options.error = azl::Stdio::to(pipe->write);
    std::string error;
    std::optional<azl::Process> child = azl::spawnProcess(options, &error);
    azl::closeHandle(pipe->write);
    REQUIRE_MESSAGE(child, error);
    CtlOutput run;
    char buffer[4096];
    for (long n; (n = azl::readHandle(pipe->read, buffer, sizeof buffer)) > 0;)
        run.output.append(buffer, size_t(n));
    azl::closeHandle(pipe->read);
    run.code = child->wait();
#ifdef _WIN32
    // Console programs end their lines with CRLF on Windows; the cases compare lines.
    std::erase(run.output, '\r');
#endif
    return run;
}

// Runs nocturne-ctl with `args` (shellWords) and the environment changes, and waits for it.
[[maybe_unused]] CtlOutput runCtl(const std::string& args,
                                  std::map<std::string, std::optional<std::string>> environment = {})
{
    return runProgram({.program = ctlPath(), .args = shellWords(args), .environment = std::move(environment)});
}

// Runs nocturne-ctl against the engine on `port` and waits for it.
[[maybe_unused]] CtlOutput runNocturneCtl(uint16_t port, const std::string& args)
{
    return runCtl("--port " + std::to_string(port) + " " + args);
}

[[maybe_unused]] bool ffmpegMissing()
{
#ifdef _WIN32
    if (std::system("where ffmpeg > nul 2>&1 && where ffprobe > nul 2>&1") == 0)
#else
    if (std::system("command -v ffmpeg > /dev/null && command -v ffprobe > /dev/null") == 0)
#endif
        return false;
    MESSAGE("skipped: needs ffmpeg and ffprobe on PATH");
    return true;
}

} // namespace
