#pragma once

#include "document/composition_file.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace azl {

// Saves the composition continuously on its own thread, never on the caller's: a change is written a short debounce
// after it arrives (at most a second after the first unsaved one), and where the show is in time whenever it drifts
// from what was saved (a tempo change, a resync, an Autopilot step, another audio input). Each save is an atomic
// write-rename. A save that fails (a full disk) is logged once and tried again every second until it works, which is
// logged too. A copy of the saved file goes to backups/ next to it once a minute while it changes, and on request
// (before a show file replaces the composition); the last kBackups of each kind are kept.
class StateSaver {
public:
    // Where the show is now, or nothing before the engine has rendered its first frame.
    using Now = std::function<std::optional<Resume>()>;

    static constexpr double kDebounce = 0.25;
    static constexpr double kBackupEvery = 60.0;
    static constexpr size_t kBackups = 20;

    // `last` is what the file holds already, kept until `now` reports something. `source` and `show` are written with
    // every save.
    StateSaver(std::filesystem::path path, Now now, std::optional<Resume> last = std::nullopt,
               std::optional<std::string> source = std::nullopt, std::optional<std::string> show = std::nullopt);
    // Saves what is unsaved before it returns.
    ~StateSaver();

    // Any thread, never blocks for long.
    void changed(std::shared_ptr<const Composition> composition);
    // Any thread, never blocks for long: the show file written with every save from now on.
    void fileChanged(const std::optional<ShowFile>& file);
    uint64_t saves() const;
    // Saves that failed.
    uint64_t failures() const;
    // Any thread: copies what the file holds now to backups/TIME-`kind`.json, if it exists.
    void backup(const std::string& kind);

private:
    void run();
    bool drifted(const Resume& now) const;
    void save(const std::shared_ptr<const Composition>& composition, const std::optional<Resume>& resume,
              const std::optional<ShowFile>& file);

    std::filesystem::path path_;
    Now now_;
    std::optional<std::string> source_;
    std::optional<std::string> show_;
    std::mutex backupMutex_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::shared_ptr<const Composition> composition_;
    std::optional<ShowFile> file_;
    bool dirty_ = false;
    bool running_ = true;
    uint64_t saves_ = 0;
    uint64_t failures_ = 0;
    bool failing_ = false;
    // Saver thread only.
    std::shared_ptr<const Composition> saved_;
    std::optional<ShowFile> savedFile_;
    std::optional<Resume> resume_;
    std::optional<std::chrono::steady_clock::time_point> backedUp_;
    std::thread thread_;
};

} // namespace azl
