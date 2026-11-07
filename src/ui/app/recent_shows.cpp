#include "ui/app/recent_shows.hpp"

#include <algorithm>

namespace azl::ui {

void RecentShows::use(juce::PropertiesFile* settings, const std::string& host)
{
    std::lock_guard lock(mutex_);
    settings_ = settings;
    key_ = "recentShows." + juce::String(host).toLowerCase();
    paths_.clear();
    if (!settings_)
        return;
    for (const juce::String& line : juce::StringArray::fromLines(settings_->getValue(key_)))
        if (line.isNotEmpty() && paths_.size() < kMax)
            paths_.push_back(line.toStdString());
}

void RecentShows::add(const std::string& path)
{
    if (path.empty())
        return;
    std::lock_guard lock(mutex_);
    std::erase(paths_, path);
    paths_.insert(paths_.begin(), path);
    paths_.resize(std::min(paths_.size(), kMax));
    save();
}

void RecentShows::clear()
{
    std::lock_guard lock(mutex_);
    paths_.clear();
    save();
}

std::vector<std::string> RecentShows::paths() const
{
    std::lock_guard lock(mutex_);
    return paths_;
}

void RecentShows::save()
{
    if (!settings_)
        return;
    juce::StringArray lines;
    for (const std::string& path : paths_)
        lines.add(juce::String(path));
    settings_->setValue(key_, lines.joinIntoString("\n"));
    settings_->saveIfNeeded();
}

} // namespace azl::ui
