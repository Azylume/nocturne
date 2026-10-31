#include "platform/app_paths.hpp"

#include <cstdlib>

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

} // namespace azl
