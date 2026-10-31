#include "platform/app_paths.hpp"

#include <mach-o/dyld.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace azl {

namespace {

std::filesystem::path home()
{
    const char* home = std::getenv("HOME");
    return home ? home : "/tmp";
}

} // namespace

std::filesystem::path appDataFolder()
{
    return home() / "Library/Application Support/Nocturne";
}

std::filesystem::path appLogsFolder()
{
    return home() / "Library/Logs/Nocturne";
}

std::filesystem::path videosFolder()
{
    return home() / "Movies";
}

std::filesystem::path executablePath()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(buffer.data(), ec);
    return ec ? std::filesystem::path(buffer.data()) : resolved;
}

std::filesystem::path siblingExecutable(const std::string& name)
{
    return executablePath().parent_path() / name;
}

} // namespace azl
