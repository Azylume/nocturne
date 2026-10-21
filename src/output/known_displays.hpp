#pragma once

#include "output/displays.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace azl {

// Every display ever attached on this machine, with what it reported (name, vendor, model, serial, port, position),
// and whether the engine ends macOS mirroring between screens' displays; kept in `file` (none: this run only). A
// composition load parks a screen whose display is known but not attached (a projector switched off) instead of moving
// it to another display. Any thread.
class KnownDisplays {
public:
    explicit KnownDisplays(std::filesystem::path file = {});

    void add(const std::vector<DisplayInfo>& displays);
    std::set<std::string> ids() const;
    // As last seen attached; nullopt for a display never seen.
    std::optional<DisplayInfo> find(const std::string& id) const;
    // On unless turned off.
    bool unmirror() const;
    void setUnmirror(bool on);

private:
    void save() const;

    const std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::map<std::string, DisplayInfo> displays_;
    bool unmirror_ = true;
};

} // namespace azl
