#pragma once

#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

// Test clips and scratch folders, on every platform.
namespace {

namespace fs = std::filesystem;

[[maybe_unused]] fs::path mediaDir()
{
    const char* dir = std::getenv("NOCTURNE_MEDIA_DIR");
    return dir ? dir : NOCTURNE_MEDIA_DIR;
}

// A new empty folder, removed with everything in it at the end of the test.
class TempDir {
public:
    TempDir()
    {
        std::random_device random;
        for (int attempt = 0; path.empty() && attempt < 100; ++attempt) {
            const fs::path candidate = fs::temp_directory_path() / ("nocturne-test-" + std::to_string(random()));
            if (fs::create_directory(candidate))
                path = candidate;
        }
        REQUIRE_FALSE(path.empty());
    }
    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    fs::path path;
};

} // namespace
