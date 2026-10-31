#include <doctest.h>

#include "platform/app_paths.hpp"
#include "scoped_env.hpp"

#include <filesystem>

TEST_CASE("platform: Nocturne's folders live in the user's own data folder")
{
#ifdef _WIN32
    ScopedEnv base("LOCALAPPDATA", "C:\\Users\\test\\AppData\\Local");
    CHECK(azl::appDataFolder() == std::filesystem::path("C:\\Users\\test\\AppData\\Local\\Nocturne"));
    CHECK(azl::appLogsFolder() == std::filesystem::path("C:\\Users\\test\\AppData\\Local\\Nocturne\\Logs"));
#else
    ScopedEnv base("HOME", "/Users/test");
    CHECK(azl::appDataFolder() == std::filesystem::path("/Users/test/Library/Application Support/Nocturne"));
    CHECK(azl::appLogsFolder() == std::filesystem::path("/Users/test/Library/Logs/Nocturne"));
#endif
}

TEST_CASE("platform: the user's videos folder is ~/Movies on Apple and the Videos known folder on Windows")
{
#ifdef _WIN32
    CHECK(azl::videosFolder().is_absolute());
    CHECK(std::filesystem::is_directory(azl::videosFolder()));
    CHECK(azl::videosFolder() != std::filesystem::temp_directory_path());
#else
    ScopedEnv base("HOME", "/Users/test");
    CHECK(azl::videosFolder() == std::filesystem::path("/Users/test/Movies"));
#endif
}

TEST_CASE("platform: the running program finds itself and the programs beside it")
{
    const std::filesystem::path self = azl::executablePath();
    CHECK(self.is_absolute());
    CHECK(std::filesystem::is_regular_file(self));
    CHECK(azl::siblingExecutable("nocturne").parent_path() == self.parent_path());
#ifdef _WIN32
    CHECK(azl::siblingExecutable("nocturne").filename() == "nocturne.exe");
#else
    CHECK(azl::siblingExecutable("nocturne").filename() == "nocturne");
#endif
}
