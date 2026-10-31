#pragma once

#include "platform/process.hpp"

#include <doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
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

// Runs nocturne-ctl against the engine on `port` and waits for it. `args` split at spaces, single quotes grouping
// words as in a shell.
[[maybe_unused]] CtlOutput runNocturneCtl(uint16_t port, const std::string& args)
{
    std::vector<std::string> argv{"--port", std::to_string(port)};
    std::istringstream words(args);
    for (std::string word; words >> std::quoted(word, '\'');)
        argv.push_back(word);
    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
    azl::SpawnOptions options{.program = ctlPath(), .args = std::move(argv)};
    options.output = azl::Stdio::to(pipe->write);
    options.error = azl::Stdio::to(pipe->write);
    std::string error;
    std::optional<azl::Process> ctl = azl::spawnProcess(options, &error);
    azl::closeHandle(pipe->write);
    REQUIRE_MESSAGE(ctl, error);
    CtlOutput run;
    char buffer[4096];
    for (long n; (n = azl::readHandle(pipe->read, buffer, sizeof buffer)) > 0;)
        run.output.append(buffer, size_t(n));
    azl::closeHandle(pipe->read);
    run.code = ctl->wait();
    return run;
}

[[maybe_unused]] bool ffmpegMissing()
{
    if (std::system("command -v ffmpeg > /dev/null && command -v ffprobe > /dev/null") == 0)
        return false;
    MESSAGE("skipped: needs ffmpeg and ffprobe on PATH");
    return true;
}

} // namespace
