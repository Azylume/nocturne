#include "platform/app_paths.hpp"

#include <windows.h>

#include <shlobj.h>

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

std::filesystem::path videosFolder()
{
    PWSTR path = nullptr;
    std::filesystem::path folder = std::filesystem::temp_directory_path();
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &path)))
        folder = path;
    CoTaskMemFree(path);
    return folder;
}

std::filesystem::path executablePath()
{
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (length == 0)
            return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::path(path) : resolved;
}

std::filesystem::path siblingExecutable(const std::string& name)
{
    return executablePath().parent_path() / (name + ".exe");
}

} // namespace azl
