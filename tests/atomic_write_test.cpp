#include "document/composition_file.hpp"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string readAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("an atomic write replaces an existing file and leaves no temporary file behind")
{
    const fs::path dir = fs::temp_directory_path() /
        ("nocturne-atomic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const fs::path file = dir / "show.json";

    azl::writeFileAtomically(file, "first\n");
    CHECK(readAll(file) == "first\n");
    azl::writeFileAtomically(file, "second\nline\n", false);
    CHECK(readAll(file) == "second\nline\n");
    CHECK_FALSE(fs::exists(dir / "show.json.tmp"));

    CHECK_THROWS(azl::writeFileAtomically(dir / "missing" / "show.json", "x"));
    fs::remove_all(dir);
}
