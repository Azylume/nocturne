#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <mutex>
#include <string>
#include <vector>

namespace azl::ui {

// The show files last opened or saved through the engine at one host, newest first, kept in the UI's settings.
// Thread-safe: the engine's replies arrive on its own thread.
class RecentShows {
public:
    static constexpr size_t kMax = 10;

    // Reads the list kept for `host`; without settings nothing is kept.
    void use(juce::PropertiesFile* settings, const std::string& host);
    // Puts `path` first, once, and drops the oldest beyond kMax.
    void add(const std::string& path);
    void clear();
    std::vector<std::string> paths() const;

private:
    void save();

    mutable std::mutex mutex_;
    juce::PropertiesFile* settings_ = nullptr;
    juce::String key_;
    std::vector<std::string> paths_;
};

} // namespace azl::ui
