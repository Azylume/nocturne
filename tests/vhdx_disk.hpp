#pragma once

#include "test_files.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

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
    ~VhdxDisk() { detach(); }

    bool detach() { return diskpart(select() + "detach vdisk\n"); }
    // The volume comes back on the same folder.
    bool attach() { return diskpart(select() + "attach vdisk\n"); }

    bool diskpart(const std::string& script)
    {
        const fs::path file = dir.path / "diskpart.txt";
        std::ofstream(file) << script;
        return std::system(("diskpart /s \"" + file.string() + "\" > \"" + (dir.path / "diskpart.log").string() +
                            "\"").c_str()) == 0;
    }

    TempDir dir;
    fs::path image;
    fs::path mount;

private:
    std::string select() const { return "select vdisk file=\"" + image.string() + "\"\n"; }
};
