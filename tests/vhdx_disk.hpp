#pragma once

#include "test_files.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <virtdisk.h>

#pragma comment(lib, "virtdisk.lib")

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

inline bool elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool known = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size);
    CloseHandle(token);
    return known && elevation.TokenIsElevated;
}

// Why this process makes no disk image, empty when it may: only an elevated process on a CI runner
// (GITHUB_ACTIONS=true) makes one, and never with NOCTURNE_NO_DISK_IMAGES=1.
inline std::string diskImagesRefused()
{
    if (const char* no = std::getenv("NOCTURNE_NO_DISK_IMAGES"); no && std::string(no) == "1")
        return "NOCTURNE_NO_DISK_IMAGES=1 is set";
    if (const char* ci = std::getenv("GITHUB_ACTIONS"); !ci || std::string(ci) != "true")
        return "GITHUB_ACTIONS=true is not set";
    return elevated() ? "" : "the process is not elevated";
}

// Ends the case with a message, before any virtual disk or diskpart call, when no disk image may be made.
#define RETURN_WITHOUT_DISK_IMAGES()                                                                                 \
    if (const std::string refused = diskImagesRefused(); !refused.empty()) {                                         \
        MESSAGE("makes a disk image only on an elevated CI runner: " << refused);                                    \
        return;                                                                                                      \
    }

// Windows: an NTFS volume in a VHDX mounted on a folder, which can be detached and attached again. The object holds
// the disk's handle, so the disk stays attached until detach() or the object's end, whatever other process exits.
// Needs an elevated process.
struct VhdxDisk {
    explicit VhdxDisk(int megabytes)
    {
        image = dir.path / "disk.vhdx";
        mount = dir.path / "mount";
        fs::create_directories(mount);
        REQUIRE(create(megabytes));
        REQUIRE(attachDisk());
        REQUIRE(diskpart("create partition primary\nformat fs=ntfs quick label=azl\nassign mount=\"" +
                         mount.string() + "\"\n"));
        REQUIRE_MESSAGE(mounted(), "the mount folder " << mount.string() << " is not on the disk's volume");
    }
    ~VhdxDisk()
    {
        if (disk_ == INVALID_HANDLE_VALUE)
            return;
        DetachVirtualDisk(disk_, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(disk_);
    }

    bool detach()
    {
        return succeeded("DetachVirtualDisk", DetachVirtualDisk(disk_, DETACH_VIRTUAL_DISK_FLAG_NONE, 0));
    }
    // The volume comes back on the same folder; true once the folder is on it again.
    bool attach()
    {
        if (!attachDisk())
            return false;
        for (int i = 0; i < 100; ++i) {
            if (mounted())
                return true;
            Sleep(100);
        }
        FAIL_CHECK("the volume is attached but not back on " << mount.string() << " after 10 s");
        return false;
    }

    TempDir dir;
    fs::path image;
    fs::path mount;

private:
    HANDLE disk_ = INVALID_HANDLE_VALUE;

    bool create(int megabytes)
    {
        // VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT, spelled out so no library has to define it.
        VIRTUAL_STORAGE_TYPE type{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
                                  {0xec984aec, 0xa0f9, 0x47e9, {0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b}}};
        CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
        parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
        parameters.Version2.MaximumSize = ULONGLONG(megabytes) << 20;
        return succeeded("CreateVirtualDisk",
                         CreateVirtualDisk(&type, image.wstring().c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr,
                                           CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &parameters, nullptr, &disk_));
    }

    bool attachDisk()
    {
        ATTACH_VIRTUAL_DISK_PARAMETERS parameters{};
        parameters.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
        const DWORD error =
            AttachVirtualDisk(disk_, nullptr, ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER, 0, &parameters, nullptr);
        return succeeded("AttachVirtualDisk", error);
    }

    // The N of \\.\PhysicalDriveN, which diskpart selects while this process keeps the disk attached.
    int diskNumber()
    {
        wchar_t path[MAX_PATH] = {};
        ULONG size = sizeof path;
        REQUIRE(succeeded("GetVirtualDiskPhysicalPath", GetVirtualDiskPhysicalPath(disk_, &size, path)));
        const std::wstring physical(path);
        const std::wstring prefix = L"\\\\.\\PhysicalDrive";
        REQUIRE(physical.rfind(prefix, 0) == 0);
        return std::stoi(physical.substr(prefix.size()));
    }

    // True when the mount folder is the root of the disk's volume, not a folder on the temp dir's volume.
    bool mounted()
    {
        wchar_t root[MAX_PATH] = {}, outer[MAX_PATH] = {}, label[MAX_PATH] = {};
        const std::wstring folder = mount.wstring() + L"\\";
        if (!GetVolumePathNameW(folder.c_str(), root, MAX_PATH) ||
            !GetVolumePathNameW(dir.path.c_str(), outer, MAX_PATH))
            return false;
        return std::wstring(root) != outer &&
               GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) &&
               std::wstring(label) == L"azl";
    }

    bool succeeded(const char* call, DWORD error)
    {
        if (error == ERROR_SUCCESS)
            return true;
        FAIL_CHECK(call << " on " << image.string() << " failed with Windows error " << error);
        return false;
    }

    // Runs the script on the attached disk. When it fails, the case fails with diskpart's output and the disk's state.
    bool diskpart(const std::string& script)
    {
        const std::string select = "select disk " + std::to_string(diskNumber()) + "\n";
        if (run(select + script))
            return true;
        const std::string output = log();
        run(select + "detail disk\n");
        FAIL_CHECK("diskpart failed on:\n" << select << script << "with:\n" << output << "the disk now:\n" << log());
        return false;
    }

    bool run(const std::string& script)
    {
        const fs::path file = dir.path / "diskpart.txt";
        std::ofstream(file) << script;
        return std::system(("diskpart /s \"" + file.string() + "\" > \"" + logFile().string() + "\"").c_str()) == 0;
    }

    fs::path logFile() const { return dir.path / "diskpart.log"; }
    std::string log() const
    {
        std::ifstream in(logFile());
        return {std::istreambuf_iterator<char>(in), {}};
    }
};
