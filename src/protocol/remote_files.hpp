#pragma once

#include "protocol/messages.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// Why a client on another machine may not name `path` (absolute on the engine machine; "~/" is the engine user's
// home): it is relative, has a ".." part, or lies outside every one of `roots` once symbolic links are resolved. None
// when it may.
std::optional<std::string> pathRefusal(const std::string& path, const std::vector<std::filesystem::path>& roots);

// Why `message` from a client on another machine is refused: one of the paths it names is refused by pathRefusal.
// The paths are composition save and load "path" and "mediaRoot", composition shows "folder", library add and relink
// "path", record set and folders "path", and preset import "roots".
std::optional<std::string> remotePathRefusal(const Message& message, const std::vector<std::filesystem::path>& roots);

} // namespace azl
