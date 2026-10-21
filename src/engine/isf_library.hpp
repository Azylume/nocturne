#pragma once

#include "effects/isf.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace azl {

// A shader file of the user folder as it was last read.
struct UserIsfShader {
    std::string file;
    std::string name;
    // Empty when the file did not parse.
    std::string type;
    IsfKind kind = IsfKind::Effect;
    // Empty when it parsed and translated.
    std::string error;
    std::string text;
};

// The ISF shaders of a folder on the engine machine (the state folder's isf-user), which are the user's own: effects
// and generators join the effect registry in the "User" category, transitions the ISF transitions. A file that does not
// parse or translate is listed with its error; an effect or generator that does not translate still registers, with a
// shader that fails with that error. The files are only ever read.
class UserIsfLibrary {
public:
    static constexpr size_t kMaxFileBytes = 256 * 1024;

    // Reads the folder once; none when empty.
    UserIsfLibrary(std::filesystem::path folder, std::filesystem::path cacheDir);
    // Retires what it registered.
    ~UserIsfLibrary();
    UserIsfLibrary(const UserIsfLibrary&) = delete;
    UserIsfLibrary& operator=(const UserIsfLibrary&) = delete;

    // Reads the folder again: registers new and changed shaders and retires removed ones. Whether anything changed.
    bool scan();
    std::vector<UserIsfShader> shaders() const;
    const std::filesystem::path& folder() const { return folder_; }

private:
    const std::filesystem::path folder_;
    const std::filesystem::path cacheDir_;
    mutable std::mutex mutex_;
    std::vector<UserIsfShader> shaders_;
    // The text each registered effect type was made from.
    std::map<std::string, std::string> effects_;
    std::vector<TransitionMode> transitions_;
};

} // namespace azl
