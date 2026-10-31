#pragma once

#include "supervisor/crash_guard.hpp"
#include "temp_folder.hpp"

#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

// Holds a mark on its own thread until it goes, as the decoders' or the compiler's thread does inside an open or
// compile.
class HeldMark {
public:
    HeldMark(std::string kind, std::string key, std::string name)
    {
        std::future<void> held = held_.get_future();
        thread_ = std::thread([this, kind, key, name, release = release_.get_future()] {
            azl::crash::Scope scope(kind, key, name);
            held_.set_value();
            release.wait();
        });
        held.wait();
    }

    ~HeldMark()
    {
        release_.set_value();
        thread_.join();
    }

private:
    std::promise<void> held_;
    std::promise<void> release_;
    std::thread thread_;
};

struct CrashFiles {
    CrashFiles() { azl::crash::setup(marks, skips, crash); }
    ~CrashFiles() { azl::crash::setup({}, {}); }

    std::vector<azl::crash::Mark> culprits() const
    {
        return azl::crash::culprits(azl::crash::readMarks(marks), azl::crash::readCrash(crash));
    }

    TempFolder dir;
    std::filesystem::path marks = dir.path / "inflight.json";
    std::filesystem::path skips = dir.path / "skip.json";
    std::filesystem::path crash = dir.path / "crash.json";
};

[[maybe_unused]] const std::string kChain = "edgeDetection,classicStripper,classicColorize";

} // namespace
