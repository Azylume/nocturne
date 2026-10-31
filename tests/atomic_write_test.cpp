#include "document/composition_file.hpp"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

std::string readAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

fs::path freshFolder()
{
    const fs::path dir = fs::temp_directory_path() /
        ("nocturne-atomic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("an atomic write replaces an existing file and leaves no temporary file behind")
{
    const fs::path dir = freshFolder();
    const fs::path file = dir / "show.json";

    azl::writeFileAtomically(file, "first\n");
    CHECK(readAll(file) == "first\n");
    azl::writeFileAtomically(file, "second\nline\n", false);
    CHECK(readAll(file) == "second\nline\n");
    CHECK_FALSE(fs::exists(dir / "show.json.tmp"));

    CHECK_THROWS(azl::writeFileAtomically(dir / "missing" / "show.json", "x"));
    fs::remove_all(dir);
}

TEST_CASE("an atomic write replaces a file another thread keeps reading, and the reader sees only whole versions")
{
    const fs::path dir = freshFolder();
    const fs::path file = dir / "status.json";
    const auto version = [](int i) { return std::string(4096, char('a' + i % 26)); };
    azl::writeFileAtomically(file, version(0), false);

    // Reads as status.json's readers do, with std::ifstream: on Windows that shares neither the file's deletion nor
    // its replacement.
    std::atomic<bool> writing{true};
    std::atomic<int> reads{0}, torn{0};
    std::thread reader([&] {
        while (writing) {
            const std::string text = readAll(file);
            ++reads;
            if (!text.empty() && (text.size() != 4096 || text.find_first_not_of(text[0]) != std::string::npos))
                ++torn;
        }
    });
    int failed = 0;
    for (int i = 1; i <= 1000; ++i)
        try {
            azl::writeFileAtomically(file, version(i), false);
        } catch (const std::exception&) {
            ++failed;
        }
    writing = false;
    reader.join();
    CHECK(failed == 0);
    CHECK(torn == 0);
    CHECK(reads > 0);
    CHECK(readAll(file) == version(1000));
    fs::remove_all(dir);
}
