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

// The system's folder for `id`, or the temporary folder when it has none.
std::filesystem::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    std::filesystem::path folder = std::filesystem::temp_directory_path();
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)))
        folder = path;
    CoTaskMemFree(path);
    return folder;
}

} // namespace

std::filesystem::path homeFolder()
{
    for (const wchar_t* name : {L"HOME", L"USERPROFILE"}) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size == 0)
            continue;
        std::wstring value(size, L'\0');
        value.resize(GetEnvironmentVariableW(name, value.data(), size));
        return value;
    }
    return {};
}

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
    return knownFolder(FOLDERID_Videos);
}

std::filesystem::path documentsFolder()
{
    return knownFolder(FOLDERID_Documents);
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
