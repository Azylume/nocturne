#pragma once

#include "document/screen_roles.hpp"

#include <filesystem>
#include <mutex>

namespace azl {

// This machine's display for each screen role, kept in `file` (none: this run only) across restarts and deploys. It
// follows the document: every composition the engine publishes records where its screens are. Any thread.
class ScreenDisplays {
public:
    explicit ScreenDisplays(std::filesystem::path file = {});

    RoleDisplays roles() const;
    void keep(const Composition& composition);

private:
    const std::filesystem::path file_;
    mutable std::mutex mutex_;
    RoleDisplays roles_;
};

} // namespace azl
