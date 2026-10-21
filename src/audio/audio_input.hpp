#pragma once

#include "audio/audio_analysis.hpp"
#include "document/composition_file.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace azl {

struct AudioDeviceInfo {
    std::string uid;
    std::string name;
    int channels = 0;
    double sampleRate = 0.0;
    bool isDefault = false;
};

// A process that has audio objects: what a per-app tap can capture.
struct AudioAppInfo {
    int pid = 0;
    std::string bundle;
    std::string name;
    // It is playing audio right now.
    bool playing = false;
};

struct AudioInputStatus {
    // What was asked for: a device UID or name (a case-insensitive part of it), empty for the default input, "system"
    // for all system audio, or "app:" and part of an app's bundle ID or name (or its pid) for that app's audio.
    std::string requested;
    // 1-based input channels mixed to mono; empty mixes all.
    std::vector<int> channels;
    // The device in use, if any.
    std::string uid;
    std::string name;
    double sampleRate = 0.0;
    // "running", "fallback" (the requested device is missing, the default input runs instead), "none" or "off".
    std::string state = "none";
    // "authorized", "denied", "restricted" or "notDetermined".
    std::string permission;
    uint64_t restarts = 0;
};

// The input the audio service drives: the machine's, or the harness's fake one.
class AudioSource {
public:
    // Called whenever the status changes; `event` says what happened, for the log.
    using Changed = std::function<void(const AudioInputStatus& status, const std::string& event)>;

    virtual ~AudioSource() = default;
    // Devices with at least one input channel.
    virtual std::vector<AudioDeviceInfo> inputDevices() const = 0;
    // Processes with audio, for per-app taps.
    virtual std::vector<AudioAppInfo> inputApps() const = 0;
    // Any thread. "off" stops capturing and never asks for the microphone permission.
    virtual void select(std::string device, std::vector<int> channels = {}) = 0;
    virtual AudioInputStatus status() const = 0;
    // Any thread. Once it returns, the previous callback is no longer running.
    virtual void onChange(Changed changed) = 0;
    // As onChange; `changed` is called when a device is added or removed.
    virtual void onDevicesChange(std::function<void()> changed) = 0;
};

// Captures one CoreAudio input device, or a process tap of the system's or one app's audio (macOS 14.2+), into an
// AudioAnalysis. The device is chosen by UID or name; when it (or the app) is missing or disappears the default input
// runs instead, and the choice is taken back when it returns. A watchdog
// restarts a device whose callbacks stop or whose sample rate changes. The audio callback only mixes the chosen
// channels to mono and pushes them into the analysis; device changes run on a private serial queue.
class AudioInput : public AudioSource {
public:
    explicit AudioInput(AudioAnalysis& analysis);
    ~AudioInput() override;
    AudioInput(const AudioInput&) = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    // Devices with at least one input channel.
    static std::vector<AudioDeviceInfo> devices();
    // Processes with audio, for per-app taps.
    static std::vector<AudioAppInfo> apps();
    // The microphone permission of this process. An AudioInput asks for it when it was never decided and an input
    // other than "off" is wanted; the answer is reported through Changed.
    static std::string permission();
    // NOCTURNE_NO_AUDIO_INPUT=1: every input is "off" and the microphone permission is never asked for.
    static bool disabled();

    std::vector<AudioDeviceInfo> inputDevices() const override { return devices(); }
    std::vector<AudioAppInfo> inputApps() const override { return apps(); }
    // Applied asynchronously; changes are reported on the input's queue.
    void select(std::string device, std::vector<int> channels = {}) override;
    AudioInputStatus status() const override;
    void onChange(Changed changed) override;
    void onDevicesChange(std::function<void()> changed) override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// The input to open on start: the one open in `saved` when it was saved under the same command line input and does
// not name a device missing from `devices`, else `commandLine`. `log` says why when that differs from the command
// line's input.
AudioChoice startingInput(const AudioChoice& commandLine, const std::optional<ResumeAudio>& saved,
                          const std::vector<AudioDeviceInfo>& devices, std::string& log);

// The process becomes responsible for itself, so macOS asks about and remembers microphone access for this binary
// rather than for the terminal, SSH session or launchd job that started it. Re-executes the binary in place once;
// returns only when that is not possible or already done.
void becomeResponsibleProcess(char** argv);

} // namespace azl
