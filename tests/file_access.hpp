#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

#include <filesystem>
#include <vector>

namespace {

#ifdef _WIN32

// This process's user, as a SID owned by the returned buffer.
[[maybe_unused]] std::vector<uint8_t> currentUser()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> user(size);
    const bool ok = GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    return ok ? user : std::vector<uint8_t>{};
}

// Replaces the access list of `path` with one protected entry for this user, in SDDL rights ("FA", "FRFX").
[[maybe_unused]] bool userOnly(const std::filesystem::path& path, const wchar_t* rights)
{
    const std::vector<uint8_t> user = currentUser();
    LPWSTR sid = nullptr;
    if (user.empty() || !ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &sid))
        return false;
    const std::wstring sddl = L"D:P(A;OICI;" + std::wstring(rights) + L";;;" + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    const bool set =
        SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    return set;
}

// A TOKEN_PRIVILEGES with room for the backup and the restore privilege.
struct BackupPrivileges {
    DWORD count = 0;
    LUID_AND_ATTRIBUTES privileges[2]{};

    TOKEN_PRIVILEGES* native() { return reinterpret_cast<TOKEN_PRIVILEGES*>(this); }
};

// Turns this process's backup and restore privileges off, or back to how they were. An elevated process (a CI runner,
// an SSH session) has them on, and CreateDirectory opens with backup intent, so a folder's access list would not keep
// it out of the folder.
[[maybe_unused]] bool backupPrivileges(bool restore)
{
    static BackupPrivileges before;
    const bool turnedOff = before.count != 0;
    if (restore != turnedOff)
        return true;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    bool ok = false;
    if (restore) {
        ok = AdjustTokenPrivileges(token, FALSE, before.native(), 0, nullptr, nullptr);
        before = {};
    } else {
        BackupPrivileges off{.count = 2};
        DWORD size = sizeof(before);
        ok = LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &off.privileges[0].Luid) &&
             LookupPrivilegeValueW(nullptr, SE_RESTORE_NAME, &off.privileges[1].Luid) &&
             AdjustTokenPrivileges(token, FALSE, off.native(), sizeof(before), before.native(), &size);
    }
    CloseHandle(token);
    return ok;
}

#endif

// Lets this user create files and folders in `folder`, or only read and enter it: mode 0700 or 0500 on POSIX.
[[maybe_unused]] bool folderWritable(const std::filesystem::path& folder, bool writable)
{
#ifdef _WIN32
    return backupPrivileges(writable) && userOnly(folder, writable ? L"FA" : L"FRFX");
#else
    return ::chmod(folder.c_str(), writable ? 0700 : 0500) == 0;
#endif
}

// Only this user may open the file: mode 0600 on POSIX; on Windows a protected access list with one entry, this
// user's.
[[maybe_unused]] bool ownerOnlyFile(const std::filesystem::path& file)
{
#ifdef _WIN32
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(descriptor, &control, &revision);
    bool only = dacl && (control & SE_DACL_PROTECTED) && dacl->AceCount == 1;
    void* ace = nullptr;
    const std::vector<uint8_t> user = currentUser();
    if (only && GetAce(dacl, 0, &ace) && !user.empty()) {
        const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        only = allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
               EqualSid(const_cast<DWORD*>(&allowed->SidStart),
                        reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid);
    } else {
        only = false;
    }
    LocalFree(descriptor);
    return only;
#else
    struct stat info {};
    return ::stat(file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600;
#endif
}

} // namespace
