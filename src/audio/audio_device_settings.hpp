#pragma once

#include "audio/audio_input.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// The gain and channels of each input device, by device UID, kept in `file` on the engine machine (none: this run
// only), and the input a client picked last. The service reports every input change and gain change; when another
// device starts running, its own setting comes back. A device never set gets the default gain and keeps the channels
// it was opened with. Any thread.
class AudioDeviceSettings {
public:
    struct Entry {
        bool autoGain = true;
        double gainDb = 0.0;
        // Empty mixes all.
        std::vector<int> channels;
        bool operator==(const Entry&) const = default;
    };

    AudioDeviceSettings(std::filesystem::path file, bool autoGain, double gainDb);

    // The input's status changed. Returns what to apply when a different device now runs; records a channel change
    // on the same device.
    std::optional<Entry> inputChanged(const AudioInputStatus& status);
    // The gain was set; it is recorded for the running device.
    void gainChanged(bool autoGain, double gainDb);
    std::optional<Entry> find(const std::string& uid) const;
    // The input picked last and the command line's input then, for startingInput.
    std::optional<ResumeAudio> picked() const;
    void pick(const ResumeAudio& picked);

private:
    void remember();
    void save() const;

    const std::filesystem::path file_;
    const Entry defaults_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::string> names_;
    std::string uid_;
    std::string name_;
    Entry current_;
    std::optional<ResumeAudio> picked_;
};

// The command line's input runs instead of the kept pick: the flags changed since it was picked.
bool flagsOverridePick(const AudioChoice& commandLine, const std::optional<ResumeAudio>& picked);

} // namespace azl
