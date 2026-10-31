#include "platform/app_paths.hpp"

#include <windows.h>

#include <string>

namespace azl {

namespace {

std::filesystem::path localAppData()
{
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0)
        return std::filesystem::temp_directory_path();
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size));
    return value;
}

} // namespace

std::filesystem::path appDataFolder()
{
    return localAppData() / "Nocturne";
}

std::filesystem::path appLogsFolder()
{
    return appDataFolder() / "Logs";
}

} // namespace azl
