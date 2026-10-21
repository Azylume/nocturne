#pragma once

#include "audio/audio_input.hpp"

#include <mutex>

namespace azl {

// An audio input with a device list the harness sets instead of CoreAudio's. It opens the requested device by UID or
// part of its name, the default device while that is missing, and takes the requested one back when it is attached
// again, as the machine's input does. Changes are reported on the calling thread.
class FakeAudioInput : public AudioSource {
public:
    // Two devices: "fake-mic" (the default, 1 channel) and "fake-interface" (4 channels).
    FakeAudioInput();

    void setDevices(std::vector<AudioDeviceInfo> devices);

    std::vector<AudioDeviceInfo> inputDevices() const override;
    std::vector<AudioAppInfo> inputApps() const override { return {}; }
    void select(std::string device, std::vector<int> channels = {}) override;
    AudioInputStatus status() const override;
    void onChange(Changed changed) override;
    void onDevicesChange(std::function<void()> changed) override;

private:
    void reconcile(const std::string& event);

    mutable std::mutex mutex_;
    std::vector<AudioDeviceInfo> devices_;
    AudioInputStatus status_;
    Changed changed_;
    std::function<void()> devicesChanged_;
};

} // namespace azl
