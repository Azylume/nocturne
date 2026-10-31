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
