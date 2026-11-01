#pragma once

#include "platform/process.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

// A new folder of its own under the system's temporary folder, removed with all it holds at the end of the scope.
class TempFolder {
public:
    TempFolder()
    {
        static std::atomic<int> made = 0;
        path = std::filesystem::temp_directory_path() /
               ("nocturne-test-" + std::to_string(azl::currentProcessId()) + "-" + std::to_string(++made) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    std::filesystem::path path;
};
