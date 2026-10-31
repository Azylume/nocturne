#pragma once

#include <filesystem>
#include <string>

namespace azl {

// Nocturne's own files on this machine: ~/Library/Application Support/Nocturne on Apple, %LOCALAPPDATA%\Nocturne on
// Windows. Read from the environment on every call, so a test can point it elsewhere. Not created.
std::filesystem::path appDataFolder();

// The supervisor's logs: ~/Library/Logs/Nocturne on Apple, %LOCALAPPDATA%\Nocturne\Logs on Windows. Not created.
std::filesystem::path appLogsFolder();

// The running program's own file, links resolved. Empty when the system does not say.
std::filesystem::path executablePath();

// The program `name` installed beside the running one, with the platform's suffix (".exe" on Windows).
std::filesystem::path siblingExecutable(const std::string& name);

} // namespace azl
