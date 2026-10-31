#pragma once

#include <filesystem>

namespace azl {

// Nocturne's own files on this machine: ~/Library/Application Support/Nocturne on Apple, %LOCALAPPDATA%\Nocturne on
// Windows. Read from the environment on every call, so a test can point it elsewhere. Not created.
std::filesystem::path appDataFolder();

// The supervisor's logs: ~/Library/Logs/Nocturne on Apple, %LOCALAPPDATA%\Nocturne\Logs on Windows. Not created.
std::filesystem::path appLogsFolder();

} // namespace azl
