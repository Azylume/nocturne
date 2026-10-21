#include "audio/audio_device_settings.hpp"

#include <cstdio>
#include <fstream>

namespace azl {

AudioDeviceSettings::AudioDeviceSettings(std::filesystem::path file, bool autoGain, double gainDb)
    : file_(std::move(file)), defaults_{autoGain, gainDb, {}}
{
    if (file_.empty())
        return;
    try {
        const std::optional<nlohmann::json> j = readJsonFile(file_);
        const nlohmann::json devices = j ? j->value("devices", nlohmann::json::object()) : nlohmann::json::object();
        for (const auto& [uid, d] : devices.items()) {
            entries_[uid] = {d.value("autoGain", true), d.value("gainDb", 0.0), d.value("channels", std::vector<int>{})};
            names_[uid] = d.value("name", "");
        }
        if (j && j->contains("input")) {
            const nlohmann::json& input = (*j)["input"];
            const auto choice = [](const nlohmann::json& c) {
                return AudioChoice{c.value("device", ""), c.value("channels", std::vector<int>{})};
            };
            picked_ = ResumeAudio{choice(input), choice(input.value("commandLine", nlohmann::json::object()))};
        }
    } catch (const std::exception& e) {
        std::printf("audio devices: %s; starting a new list\n", e.what());
    }
}

std::optional<AudioDeviceSettings::Entry> AudioDeviceSettings::inputChanged(const AudioInputStatus& status)
{
    std::lock_guard lock(mutex_);
    const bool running = status.state == "running" || status.state == "fallback";
    const std::string uid = running ? status.uid : "";
    if (uid.empty() || uid != uid_) {
        uid_ = uid;
        name_ = status.name;
        if (uid.empty())
            return std::nullopt;
        const auto it = entries_.find(uid);
        current_ = it != entries_.end() ? it->second : Entry{defaults_.autoGain, defaults_.gainDb, status.channels};
        return current_;
    }
    if (status.channels != current_.channels) {
        current_.channels = status.channels;
        remember();
    }
    return std::nullopt;
}

void AudioDeviceSettings::gainChanged(bool autoGain, double gainDb)
{
    std::lock_guard lock(mutex_);
    current_.autoGain = autoGain;
    current_.gainDb = gainDb;
    if (!uid_.empty())
        remember();
}

std::optional<AudioDeviceSettings::Entry> AudioDeviceSettings::find(const std::string& uid) const
{
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(uid);
    if (it == entries_.end())
        return std::nullopt;
    return it->second;
}

std::optional<ResumeAudio> AudioDeviceSettings::picked() const
{
    std::lock_guard lock(mutex_);
    return picked_;
}

void AudioDeviceSettings::pick(const ResumeAudio& picked)
{
    std::lock_guard lock(mutex_);
    picked_ = picked;
    save();
}

void AudioDeviceSettings::remember()
{
    entries_[uid_] = current_;
    names_[uid_] = name_;
    save();
}

void AudioDeviceSettings::save() const
{
    if (file_.empty())
        return;
    nlohmann::json devices = nlohmann::json::object();
    for (const auto& [uid, e] : entries_)
        devices[uid] = {{"name", names_.count(uid) ? names_.at(uid) : std::string()},
                        {"autoGain", e.autoGain},
                        {"gainDb", e.gainDb},
                        {"channels", e.channels}};
    nlohmann::json kept{{"devices", devices}};
    if (picked_)
        kept["input"] = {{"device", picked_->open.device},
                         {"channels", picked_->open.channels},
                         {"commandLine",
                          {{"device", picked_->commandLine.device}, {"channels", picked_->commandLine.channels}}}};
    const std::filesystem::path temporary = file_.string() + ".tmp";
    std::ofstream(temporary) << kept.dump(2);
    std::error_code ec;
    std::filesystem::rename(temporary, file_, ec);
}

bool flagsOverridePick(const AudioChoice& commandLine, const std::optional<ResumeAudio>& picked)
{
    return picked && picked->commandLine != commandLine && picked->open != commandLine;
}

} // namespace azl
