#include "harness/fake_audio_input.hpp"

#include <algorithm>
#include <cctype>

namespace azl {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

} // namespace

FakeAudioInput::FakeAudioInput()
    : devices_{{"fake-mic", "Fake Mic", 1, 48000.0, true}, {"fake-interface", "Fake Interface", 4, 48000.0, false}}
{
    status_.permission = "authorized";
}

void FakeAudioInput::setDevices(std::vector<AudioDeviceInfo> devices)
{
    std::function<void()> changed;
    {
        std::lock_guard lock(mutex_);
        devices_ = std::move(devices);
        changed = devicesChanged_;
    }
    if (changed)
        changed();
    reconcile("devices changed");
}

std::vector<AudioDeviceInfo> FakeAudioInput::inputDevices() const
{
    std::lock_guard lock(mutex_);
    return devices_;
}

void FakeAudioInput::select(std::string device, std::vector<int> channels)
{
    {
        std::lock_guard lock(mutex_);
        status_.requested = std::move(device);
        status_.channels = std::move(channels);
    }
    reconcile("select");
}

AudioInputStatus FakeAudioInput::status() const
{
    std::lock_guard lock(mutex_);
    return status_;
}

void FakeAudioInput::onChange(Changed changed)
{
    std::lock_guard lock(mutex_);
    changed_ = std::move(changed);
}

void FakeAudioInput::onDevicesChange(std::function<void()> changed)
{
    std::lock_guard lock(mutex_);
    devicesChanged_ = std::move(changed);
}

void FakeAudioInput::reconcile(const std::string& event)
{
    AudioInputStatus status;
    Changed changed;
    {
        std::lock_guard lock(mutex_);
        AudioInputStatus& s = status_;
        const auto requested = std::find_if(devices_.begin(), devices_.end(), [&](const AudioDeviceInfo& d) {
            return !s.requested.empty() &&
                   (d.uid == s.requested || lower(d.name).find(lower(s.requested)) != std::string::npos);
        });
        const auto fallback =
            std::find_if(devices_.begin(), devices_.end(), [](const AudioDeviceInfo& d) { return d.isDefault; });
        const AudioDeviceInfo* open = requested != devices_.end() ? &*requested
                                      : fallback != devices_.end() ? &*fallback
                                                                   : nullptr;
        if (s.requested == "off")
            open = nullptr;
        s.uid = open ? open->uid : "";
        s.name = open ? open->name : "";
        s.sampleRate = open ? open->sampleRate : 0.0;
        s.state = s.requested == "off"                                       ? "off"
                  : !open                                                    ? "none"
                  : requested == devices_.end() && !s.requested.empty() ? "fallback"
                                                                             : "running";
        status = s;
        changed = changed_;
    }
    if (changed)
        changed(status, event);
}

} // namespace azl
