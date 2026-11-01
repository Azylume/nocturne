#pragma once

#include "test_files.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

// Why this process makes no disk image, empty when it may: NOCTURNE_NO_DISK_IMAGES=1, or a process that is not
// elevated, where diskpart would ask for elevation on the desktop.
inline std::string diskImagesRefused()
{
    if (const char* no = std::getenv("NOCTURNE_NO_DISK_IMAGES"); no && std::string(no) == "1")
        return "NOCTURNE_NO_DISK_IMAGES=1 is set";
    return elevated() ? "" : "the process is not elevated";
}

// Ends the case with a message, before any diskpart, when no disk image may be made. CI's runner is elevated.
#define RETURN_WITHOUT_DISK_IMAGES()                                                                                 \
    if (const std::string refused = diskImagesRefused(); !refused.empty()) {                                         \
        MESSAGE("needs an elevated process that may make disk images (diskpart): " << refused);                     \
        return;                                                                                                      \
    }

// Windows: an NTFS volume in a VHDX mounted on a folder with diskpart, which can be detached and attached again.
// Needs an elevated process.
struct VhdxDisk {
    explicit VhdxDisk(int megabytes)
    {
        image = dir.path / "disk.vhdx";
        mount = dir.path / "mount";
        fs::create_directories(mount);
        REQUIRE(diskpart("create vdisk file=\"" + image.string() + "\" maximum=" + std::to_string(megabytes) +
                         " type=expandable\n" + select() + "attach vdisk\ncreate partition primary\n"
                         "format fs=ntfs quick label=azl\nassign mount=\"" + mount.string() + "\"\n"));
    }
    ~VhdxDisk() { run(select() + "detach vdisk\n"); }

    bool detach() { return diskpart(select() + "detach vdisk\n"); }
    // The volume comes back on the same folder.
    bool attach() { return diskpart(select() + "attach vdisk\n"); }

    TempDir dir;
    fs::path image;
    fs::path mount;

private:
    std::string select() const { return "select vdisk file=\"" + image.string() + "\"\n"; }

    // When the script fails, the case fails with diskpart's output and the disk's state.
    bool diskpart(const std::string& script)
    {
        if (run(script))
            return true;
        const std::string output = log();
        run(select() + "detail vdisk\n");
        FAIL_CHECK("diskpart failed on:\n" << script << "with:\n" << output << "the disk now:\n" << log());
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
