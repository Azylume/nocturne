#include <doctest.h>

#include "platform/app_paths.hpp"
#include "scoped_env.hpp"
#include "test_files.hpp"

#include <filesystem>
#include <fstream>

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

TEST_CASE("platform: the user's documents folder is ~/Documents on Apple and the Documents known folder on Windows")
{
#ifdef _WIN32
    CHECK(azl::documentsFolder().is_absolute());
    CHECK(std::filesystem::is_directory(azl::documentsFolder()));
    CHECK(azl::documentsFolder() != std::filesystem::temp_directory_path());
#else
    ScopedEnv base("HOME", "/Users/test");
    CHECK(azl::documentsFolder() == std::filesystem::path("/Users/test/Documents"));
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

TEST_CASE("platform: a tool in the program's folder comes before the one on PATH")
{
    const TempDir folder;
    CHECK(azl::programInFolderOrPath(folder.path, "ffmpeg") == std::filesystem::path("ffmpeg"));
#ifdef _WIN32
    const std::filesystem::path bundled = folder.path / "ffmpeg.exe";
#else
    const std::filesystem::path bundled = folder.path / "ffmpeg";
#endif
    std::ofstream(bundled) << "";
    CHECK(azl::programInFolderOrPath(folder.path, "ffmpeg") == bundled);
    CHECK(azl::programInFolderOrPath(folder.path, "ffprobe") == std::filesystem::path("ffprobe"));
}
