#pragma once

#include "platform/app_paths.hpp"

#include <filesystem>

namespace azl {

// This machine's Macro Sets, unless --macro-sets names another file.
inline std::filesystem::path defaultMacroSetsFile()
{
    return appDataFolder() / "MacroSets.json";
}

} // namespace azl
