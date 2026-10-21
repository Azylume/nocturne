#pragma once

#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

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
    return std::filesystem::path(path ? path : NOCTURNE_SUPERVISOR).parent_path() / name;
}

[[maybe_unused]] std::filesystem::path ctlPath()
{
    return bundleBinary("nocturne-ctl");
}

[[maybe_unused]] bool ffmpegMissing()
{
    if (std::system("command -v ffmpeg > /dev/null && command -v ffprobe > /dev/null") == 0)
        return false;
    MESSAGE("skipped: needs ffmpeg and ffprobe on PATH");
    return true;
}

} // namespace
