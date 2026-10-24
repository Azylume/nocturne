#pragma once

#include "document/composition.hpp"
#include "protocol/document_mirror.hpp"
#include "protocol/messages.hpp"
#include "ui/client/return_video_client.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace azl {
class PreparedCopy;
}

namespace azl::ui {

// One preset of the engine's list. Group is "builtin" (our looks), "imported" (read from another VJ app's preset
// folders) or "mine" (saved by the user); the same name may sit in more than one group.
struct PresetEntry {
    std::string effectType;
    std::string name;
    std::string group;
    std::map<std::string, double> params;
};

// The beat clock and what every layer shows, as of the engine's last answer to "clock state".
struct ClockState {
    struct Clip {
        uint64_t column = 0;
        int frame = 0;
        int beats = 0;
        uint64_t next = 0;
        // The bars left of a trigger's hold on Autopilot, 0 when not held.
        int held = 0;
    };
    double beat = 0.0;
    double tempo = 120.0;
    // The layers the engine's frame budget guard draws at half resolution.
    uint64_t budgetLevel = 0;
    std::map<uint64_t, Clip> clips;
    // The column Autopilot: the column it triggered last, the one it plays next and the bars left of a trigger's hold;
    // all 0 when it is off.
    struct Columns {
        uint64_t column = 0;
        uint64_t next = 0;
        int held = 0;
    };
    Columns columns;
    // Each effect with steps: the step it is on and its Autopilot's next (none when off or before the first), and the
    // bars left of a trigger's hold.
    struct EffectSteps {
        std::optional<size_t> step;
        std::optional<size_t> next;
        int held = 0;

        bool operator==(const EffectSteps&) const = default;
    };
    std::map<uint64_t, EffectSteps> effects;
    // The palette the show is on, as an effect's steps, and each palette slot's colour as the engine draws it; empty
    // without palettes.
    std::optional<EffectSteps> palette;
    std::vector<std::array<double, 4>> paletteColours;
    std::chrono::steady_clock::time_point received{};
};

// The engine's undo history: the steps on each stack and the op of the step undo or redo would take, and every step's
// op on each stack, oldest first (the snapshot lists them, each change moves them on).
struct History {
    size_t undo = 0;
    size_t redo = 0;
    std::string undoOp;
    std::string redoOp;
    std::vector<std::string> past{};
    std::vector<std::string> future{};
};

// Moves `history`'s step lists on by one change's counts and ops.
void followHistory(History& history, size_t undo, size_t redo, const std::string& undoOp, const std::string& redoOp);

// Live values as of the engine's last answer to "modulation state": each macro after its modulators, each modulator's
// source, and each driven parameter.
struct ModulationState {
    std::map<uint64_t, double> macros;
    std::map<uint64_t, double> modulators;
    std::map<std::tuple<uint64_t, uint64_t, std::string>, double> params;
    // Modulators whose target parameter does not exist.
    std::vector<uint64_t> dangling;
};

// The audio analysis the engine reported last, by feature name; empty while the engine serves no audio.
struct AudioLevels {
    std::map<std::string, double> values;
    // Kick, snare and hat onsets in the last analysis.
    std::map<std::string, bool> onsets;
    // 24 bands, 0..1, low to high.
    std::vector<double> spectrum;
    // The gain applied now, also while auto gain runs.
    double gainDb = 0.0;
    bool live = false;
    // Each input channel of the device before mixing; noise is about 1 for hiss, near 0 for music.
    struct Channel {
        double levelDb = -120.0;
        double noise = 0.0;
    };
    std::vector<Channel> channels;
};

// The engine's input gain setting.
struct AudioGain {
    bool autoGain = true;
    double gainDb = 0.0;
};

// The engine's input as it reported it last.
struct AudioInputState {
    std::string uid;
    std::string name;
    // 1-based channels mixed to mono; empty mixes all.
    std::vector<int> channels;
    // What was picked: a device uid, "" for the default input, or "off".
    std::string requested;
    // "running", "fallback", "none" or "off", and the microphone permission.
    std::string state;
    std::string permission;
    double sampleRate = 0.0;
};

// The engine machine's performance and cache settings (settings_service.hpp); no sections from an engine without them.
struct EngineSettings {
    std::string profile = "smooth";
    bool protectTransitions = false;
    bool bakeStatic = false;
    uint64_t renderScale = 100;
    uint64_t frameCacheMB = 0;
    uint64_t preloadMB = 0;
    uint64_t clipPreviews = 0;
    struct Use {
        uint64_t frameCacheMB = 0;
        uint64_t preloadMB = 0;
        uint64_t clipPreviews = 0;
        uint64_t clipPreviewKB = 0;
    };
    Use use;
    // Field -> the engine flag that set it on this start.
    std::map<std::string, std::string> overridden;
    std::vector<std::string> sections;
};

// An attached display of the engine's machine, as of its last outputs list.
struct OutputDisplay {
    std::string id;
    std::string name;
    int width = 0;
    int height = 0;
    // The display it mirrors; empty when it shows its own picture.
    std::string mirrorOf;
};

// An audio input device of the engine's machine.
struct AudioDevice {
    std::string uid;
    std::string name;
    int channels = 0;
};

// What loading a Macro Set would bring, as the engine previewed it (modulation_service.hpp).
struct MacroSetItem {
    std::string key, kind, label, status, parent;
};
struct MacroSetPreview {
    std::string name;
    std::vector<MacroSetItem> items;
    std::vector<std::string> notes;
    size_t macros = 0, modulators = 0;
};

// The engine's recorder as of its last "record" message.
struct RecordState {
    bool recording = false;
    std::string codec;
    std::string target;
    std::string file;
    uint64_t frames = 0;
    uint64_t dropped = 0;
    // "low": less than twice the reserve free; "full": the recording stopped below it.
    std::string space;
    uint64_t freeMB = 0;
    uint64_t stopsAtMB = 0;
    // "on", "off" or "unavailable": the recording's audio track, or the next one's.
    std::string audio;
    std::string audioReason;
    std::string audioSource;
    // The settings the next recording starts with; the folder it goes to, and why it cannot ("" when it can).
    std::string path;
    std::string folder;
    std::string folderProblem;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t reserveMB = 0;
    double reservePercent = 0.0;
    uint64_t maxMB = 0;
    double maxSeconds = 0.0;
    bool precise = false;
    double maxTotalSeconds = 0.0;
    double gigGraceSeconds = 0.0;
    // When the running recording finishes by itself, on the engine machine's clock ("04:15"; empty: never), and why.
    std::string stopsAtClock;
    std::string stopsFor;
};

// Folders on the engine machine, as its recorder listed them for a folder picker.
struct EngineFolder {
    std::string name;
    std::string path;
    // Volumes only.
    uint64_t freeMB = 0;
    uint64_t totalMB = 0;
};
struct FolderList {
    std::string path;
    // Empty at the root.
    std::string parent;
    uint64_t freeMB = 0;
    uint64_t totalMB = 0;
    bool writable = false;
    std::vector<EngineFolder> folders;
    std::vector<EngineFolder> volumes;
};

// One output of the engine's machine as its last machine status reported it; a figure it lacked is empty.
struct MachineOutput {
    uint64_t screen = 0;
    std::string screenName;
    std::string display;
    std::string displayName;
    std::optional<double> hz;
    std::optional<double> presents;
    std::optional<double> hitches;
    // "ok", "low rate", "missing" or "mirrored".
    std::string state;
};

// The engine machine's health as of its last machine status (machine_service.hpp); a figure it lacked is empty.
struct MachineStatus {
    // "normal" or "safe", with the reason it is in safe mode.
    std::string mode;
    std::string safeReason;
    bool showLock = false;
    bool testCard = false;
    std::string version;
    std::optional<double> fps;
    std::optional<double> gpuMs;
    uint64_t guard = 0;
    std::optional<double> cpuSpeedLimit;
    std::optional<double> temperatureC;
    std::string thermalState;
    struct Cue {
        uint64_t cue = 0;
        std::string name;
        int64_t bar = 0;
        double seconds = 0.0;
    };
    std::optional<Cue> nextCue;
    std::vector<MachineOutput> outputs;
    struct Audio {
        std::string device;
        std::string state;
        std::vector<int> channels;
        double gainDb = 0.0;
        double level = 0.0;
        bool live = false;
    };
    std::optional<Audio> audio;
    double bpm = 0.0;
    int64_t bar = 0;
    int beatInBar = 0;
    // Empty while the engine serves no DJ link.
    std::optional<bool> djlinkEnabled;
    bool djlinkFollowing = false;
    // Each player heard on the DJ link; a figure it did not send is empty.
    struct DjPlayer {
        int number = 0;
        std::optional<bool> onAir;
        std::optional<double> bpm;
        std::optional<double> pitch;
        std::string play;
        std::optional<int> beatInBar;
    };
    std::vector<DjPlayer> djPlayers;
    // The venue's media readiness; empty while the engine syncs no media.
    struct Media {
        bool ready = false;
        std::optional<int> missing;
        // The decks missing something, with how many files each.
        std::vector<std::pair<std::string, int>> decks;
        std::string mode;
        std::string state;
        // Of the download under way, 0..1.
        std::optional<double> progress;
        std::string error;
        std::string alert;
    };
    std::optional<Media> media;
    // Why Update is refused now; empty while it is allowed.
    std::string updateRefused;
    // The installer's last result: "updated", "rolled back", "installed" or "failed", with the reason.
    struct Update {
        std::string result;
        std::string build;
        std::string previous;
        std::string reason;
        double at = 0.0;

        bool operator==(const Update&) const = default;
    };
    std::optional<Update> lastUpdate;
};

// This UI's Update of the engine machine (engine/bundle_update.hpp): "packing", "sending", "installing" (the engine
// restarts on it now), or "failed" with the error; empty before the first.
struct UpdateProgress {
    std::string state;
    uint64_t sent = 0;
    uint64_t size = 0;
    std::string build;
    std::string error;
};

// The composition files the engine listed on its machine (composition_service.hpp), newest first.
struct ShowInfo {
    std::string path;
    std::string name;
    double modified = 0.0;
    uint64_t size = 0;
    bool mediaPresent = false;
};

struct ShowList {
    std::string folder;
    std::string showsFolder;
    std::vector<std::string> recent;
    std::vector<ShowInfo> shows;
};

struct LibraryItem {
    std::string hash;
    std::string name;
    std::string path;
    std::string status;
};

// A preview picture: premultiplied BGRA pixels read from a shared surface, or a JPEG from a remote engine.
struct PreviewPicture {
    uint64_t frame = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;
    std::vector<uint8_t> jpeg;
};

// What the UI knows of one engine, kept current from the protocol: the composition (snapshot plus changes), the beat
// clock, the Media Library, clip stills and previews. It does not own the transport: whoever connects hands it a send
// function and feeds it every message. Thread-safe; listener calls come on the thread that delivered the message.
class EngineClient {
public:
    struct Listener {
        virtual ~Listener() = default;
        virtual void documentChanged() {}
        virtual void clockChanged() {}
        virtual void libraryChanged() {}
        virtual void stillArrived(const std::string& hash) {}
        virtual void previewArrived(const std::string& source) {}
        virtual void connectionChanged() {}
        virtual void engineError(const std::string& message) {}
        virtual void modulationChanged() {}
        virtual void audioChanged() {}
        virtual void macroSetsChanged() {}
        virtual void presetsChanged() {}
        virtual void showsChanged() {}
        virtual void outputsChanged() {}
        // The engine turned live clip previews on or off.
        virtual void clipPreviewsChanged() {}
        virtual void recordChanged() {}
        virtual void settingsChanged() {}
        // A recording ended by itself (a full disk); the text is for the user.
        virtual void recordNotice(const std::string& notice) {}
        // The engine kept a screen on its display (ended mirroring, followed a display's new id); the text is for the
        // user.
        virtual void outputsNotice(const std::string& notice) {}
        virtual void machineChanged() {}
        // An output's return video has a new picture.
        virtual void videoArrived() {}
        // A prepared change went out or was scheduled, with what it left out; the text is for the user.
        virtual void prepareNotice(const std::string& notice) {}
        // A loaded show dropped what it held of removed effects, or a change took the macro targets, modulators or Cue
        // steps of a removed layer or effect with it; the text is for the user.
        virtual void compositionNotice(const std::string& notice) {}
        // The night's deck switches changed.
        virtual void switchesChanged() {}
    };
    using Send = std::function<void(const Message&)>;

    explicit EngineClient(Listener& listener);
    ~EngineClient();

    // A transport opened; `local` means the engine runs on this machine, so previews come through shared surfaces.
    void attached(Send send, bool local, const std::string& token = {});
    void receive(const Message& message);
    void detached();
    // Drops everything known of the engine, for a switch to another one; call it while detached.
    void forget();

    bool connected() const;
    bool synced() const;
    // Empty while the engine runs this UI's effect registry; otherwise the banner telling which side to update.
    std::string versionSkew() const;
    // The engine's build id from its last snapshot; empty before one.
    std::string engineBuild() const;
    bool local() const;
    std::shared_ptr<const Composition> composition() const;
    uint64_t change() const;
    ClockState clock() const;
    // The beat now, run on from the last clock state at its tempo.
    double beatNow() const;
    std::vector<LibraryItem> library() const;
    // The JPEG still of a clip, asked for once on first use; empty until it arrives or when there is none.
    std::vector<uint8_t> still(const std::string& hash);
    std::optional<PreviewPicture> preview(const std::string& source) const;
    // Clips the engine reported it cannot play, by hash.
    std::set<std::string> failedClips() const;

    // The layer shows the clip in that column now.
    bool playing(uint64_t layer, uint64_t column) const;
    // The layer was triggered to that column and has not got there yet.
    bool queued(uint64_t layer, uint64_t column) const;

    History history() const;
    // Edits made since the show file was saved or loaded, as far as the engine's snapshot and this client's history
    // tell.
    bool unsaved() const;
    // The show file on the engine machine the document belongs to: the one saved or loaded last, before a restart too;
    // empty for a new document.
    std::string savedPath() const;
    ModulationState modulation() const;
    AudioLevels audio() const;
    // Effect type -> preset name -> parameter values, as of the last preset list.
    Message presets() const;
    // Reads the engine machine's user ISF folder again.
    void rescanIsf();
    // Every preset of every group, by group (builtin, imported, mine), then type and name. An engine that sends no
    // groups has its presets all as "mine".
    std::vector<PresetEntry> presetEntries() const;
    // Effect type -> the preset new effects of it start from.
    std::map<std::string, std::string> presetDefaults() const;
    // Chain preset name -> its effects as JSON.
    Message chainPresets() const;
    // This machine's own presets, defaults and chain presets in the Presets.json format, for export.
    Message presetFile() const;
    std::vector<std::string> macroSets() const;
    // The last preview the engine sent, until the next one.
    std::optional<MacroSetPreview> macroSetPreview() const;
    std::vector<OutputDisplay> displays() const;
    RecordState record() const;
    // Empty until the first machine status since watchMachine(true).
    std::optional<MachineStatus> machine() const;
    // The engine's switch log of the night now: its date and lines (engine/switch_log.hpp).
    struct Switches {
        std::string date;
        std::vector<Message> lines;
    };
    Switches switches() const;

    // Undoable edits. Between beginGesture and endGesture every edit carries the same gesture, so the engine makes
    // them one undo step: a drag, or a compound action such as moving a clip.
    void edit(const Edit& edit);
    void beginGesture();
    void endGesture();
    bool gesturing() const;
    void undo();
    void redo();

    // Prepare (protocol/prepare.hpp): while on, edit, undo and redo change a local copy of the engine's composition,
    // which composition() and history() show, kept on top of the engine's own changes. Live actions still go out.
    void prepare();
    bool preparing() const;
    // Sends the copy's edits to apply as one undo step, now or at the next deck switch, and leaves Prepare.
    void sendPrepared(bool atSwitch);
    // Leaves Prepare; the engine never saw the copy.
    void discardPrepared();

    void requestModulation();
    void savePreset(uint64_t effect, const std::string& name);
    // From `group` when given, else the preset that wins by name (mine over imported over builtin).
    void applyPreset(uint64_t effect, const std::string& name, const std::string& group = {});
    // Saved presets only; the preset list follows.
    void renamePreset(const std::string& effectType, const std::string& name, const std::string& to);
    void deletePreset(const std::string& effectType, const std::string& name);
    // The engine reads the presets of the app the Classic pack mirrors on its machine; the preset list follows.
    void importPresets();
    // An empty name clears the type's default. The preset list follows each of these.
    void setDefaultPreset(const std::string& effectType, const std::string& name);
    void saveChainPreset(const std::string& name, const std::vector<Effect>& effects);
    void renameChainPreset(const std::string& name, const std::string& to);
    void deleteChainPreset(const std::string& name);
    // A file in the Presets.json format joins the engine's saved presets.
    void mergePresets(const Message& file);
    void saveMacroSet(const std::string& name);
    void previewMacroSet(const std::string& name);
    // Loads the set, leaving out the preview's items in `drop`.
    void loadMacroSet(const std::string& name, const std::vector<std::string>& drop);
    // Live actions, outside the undo history.
    void triggerClip(uint64_t layer, uint64_t column);
    void triggerColumn(uint64_t column);
    // Moves the effect to its step `step` (0-based), on the composition's quantisation.
    void triggerStep(uint64_t effect, size_t step);
    void selectDeck(uint64_t deck);
    void selectPalette(uint64_t palette);
    // Takes back the triggers still waiting for their beat: one layer's, or without a layer all of them.
    void cancelTrigger(std::optional<uint64_t> layer = std::nullopt);
    // The layer plays nothing until its next trigger.
    void stopLayer(uint64_t layer);
    void tap();
    void resync();
    // Runs the beat clock this many percent faster (negative: slower) until a nudge of 0.
    void nudge(double percent);
    // The Section ("intro", "build", "drop", "breakdown", "outro") starts on the next bar.
    void markSection(const std::string& section);
    // The whole composition as a file at `path` on the engine machine; loading it is one undo step.
    void saveComposition(const std::string& path);
    void loadComposition(const std::string& path);
    // Asks for the shows in `folder` on the engine machine, or with none its shows folder and recent shows.
    void requestShows(const std::string& folder = {});
    ShowList shows() const;
    std::vector<AudioDevice> audioDevices() const;
    AudioInputState audioInput() const;
    // A device uid, "" for the default input, or "off"; `channels` are 1-based, empty mixes all.
    void selectAudioInput(const std::string& device, const std::vector<int>& channels = {});
    AudioGain audioGain() const;
    // The engine keeps it for the input device in use.
    void setAudioGain(const AudioGain& gain);
    // Starts recording with the engine's settings, or stops.
    void record(bool on);
    // The fields of a "record" "set" (record_service.hpp); the engine keeps them and answers with its state.
    void setRecordSettings(const Message& fields);
    // Asks for the folders in `path` on the engine machine, or with none in its recording folder; folders() follows,
    // with recordChanged.
    void requestFolders(const std::string& path = {});
    FolderList folders() const;
    EngineSettings settings() const;
    // Asks for the settings with what the caches hold now; settings() follows, with settingsChanged.
    void requestSettings();
    // The fields of a "settings" "set"; the engine applies and keeps them.
    void setSettings(const Message& fields);
    void requestClock();
    void requestLibrary();
    // Adds files on the engine's machine to its library; `then` gets each entry as it is added.
    void addMedia(const std::vector<std::string>& paths, std::function<void(const LibraryItem&)> then);
    // Finds missing media again: by hash in the library's folders, or as the file at `path` on the engine machine. When
    // the file holds other content, every cell with the old hash takes the new one in one undo step.
    void relink(const std::string& hash, const std::string& path = {});
    // Machine-local overlays on the engine's outputs, outside the document: the guides on every output, and a corner
    // target through a point (the output's 0..1 space) on one display's output, or none.
    void showGuides(bool shown);
    void setTarget(const std::string& display, std::optional<Point> point);
    // Every output, or one display's, shows its screen's name and output large for a few seconds.
    void identify(const std::string& display = {}, double seconds = 3.0);
    void requestOutputs();
    // Whether the engine ends macOS mirroring between displays that carry different screens (machine-local), as of
    // its last outputs list; setUnmirror asks it to.
    bool unmirror() const;
    void setUnmirror(bool on);
    // Replaces what this client watches; sent again after every reconnect.
    void watchPreviews(const std::set<std::string>& sources);
    // Replaces the clips whose moving previews ("clip:" + hash) this client watches, in a watch of their own at
    // kClipWidth x kClipHeight and kClipFps; sent again after every reconnect. With the cell (layer, column) the clip
    // sits in, the engine draws that clip's own effects over its pictures.
    void watchClipPreviews(const std::set<std::string>& hashes,
                           std::optional<std::pair<uint64_t, uint64_t>> cell = std::nullopt);
    // While on, the engine sends its machine status a few times a second (the Machines tab is open); kept across
    // reconnects.
    void watchMachine(bool on);
    bool watchingMachine() const;
    static constexpr double kMachineRate = 2.0;
    // While on, the engine streams every output's return video (the Machines tab is open); kept across reconnects.
    void watchVideo(bool on);
    bool watchingVideo() const;
    std::optional<VideoTile> videoTile(const std::string& display) const { return video_.tile(display); }
    uint64_t videoBytes() const { return video_.bytes(); }
    // While on, the engine streams its audio input back (return_audio.hpp); kept across reconnects, ended by forget().
    // Its "listen" messages go to the listen sink, on the thread that delivered them.
    void listen(bool on);
    bool listening() const;
    void setListenSink(std::function<void(const Message&)> sink);
    // A machine action (machine_service.hpp), e.g. {"op": "restart"}; a refusal comes back as an engine error.
    void machineAction(Message action);
    // The venue's media sync override: "auto", "allow" or "block" (media_sync.hpp).
    void setMediaSync(const std::string& mode);
    // Sends the Nocturne.app at `app` for the engine machine to install, from a thread of its own; updateProgress()
    // follows, each step with machineChanged. Ignored while one is being packed or sent.
    void sendUpdate(std::filesystem::path app);
    UpdateProgress updateProgress() const;
    // Whether the engine serves clip previews (machine-local engine state, not the document); asked on attach.
    bool clipPreviews() const;
    // Asks the engine to turn them on or off; clipPreviews follows its answer.
    void setClipPreviews(bool on);

    static constexpr uint32_t kSurfaceWidth = 384;
    static constexpr uint32_t kSurfaceHeight = 216;
    static constexpr uint32_t kJpegWidth = 320;
    static constexpr uint32_t kJpegHeight = 180;
    static constexpr uint32_t kClipWidth = 160;
    static constexpr uint32_t kClipHeight = 90;
    static constexpr int kClipFps = 4;

private:
    struct Surfaces;

    void sendLocked(const Message& message);
    void replaceMedia(const std::string& hash, const LibraryItem& item);
    void sendWatchLocked();
    void sendClipWatchLocked();
    void dropUnwatchedLocked();
    void receiveLocked(const std::string& type, const Message& message, std::vector<std::function<void()>>& notify);
    void followFile(const Message& snapshot);
    void receiveLibrary(const Message& message, std::vector<std::function<void()>>& notify);
    void receivePreview(const Message& message, std::vector<std::function<void()>>& notify);
    void receiveClock(const Message& message);
    void receiveModulation(const Message& message);
    void receiveAudio(const Message& message);
    void receiveIsf(const Message& message, std::vector<std::function<void()>>& notify);
    void receivePresets(const Message& message);
    void receiveSwitches(const Message& message);
    void receiveRecord(const Message& message, std::vector<std::function<void()>>& notify);
    void receiveMachine(const Message& message);
    void runUpdate(const std::filesystem::path& app);
    // Sends a part of the Update and waits for the engine's answer; none when it does not come.
    std::optional<Message> exchange(const Message& request);
    void setUpdate(UpdateProgress progress);
    void receiveSettings(const Message& message);
    void showLocked();
    bool editLocally(const std::function<void(PreparedCopy&)>& change);

    Listener& listener_;
    mutable std::mutex mutex_;
    Send send_;
    bool local_ = false;
    DocumentMirror mirror_;
    std::shared_ptr<const Composition> composition_ = std::make_shared<const Composition>();
    ClockState clock_;
    // Layers whose trigger changed after the last clock state, which cannot show it yet.
    std::set<uint64_t> triggeredSinceClock_;
    std::vector<LibraryItem> library_;
    std::map<std::string, std::vector<uint8_t>> stills_;
    std::set<std::string> stillsAsked_;
    std::set<std::string> watched_;
    std::set<std::string> watchedClips_;
    bool clipPreviews_ = false;
    std::optional<std::pair<uint64_t, uint64_t>> watchedCell_;
    std::map<std::string, PreviewPicture> previews_;
    std::map<std::string, std::unique_ptr<Surfaces>> surfaces_;
    std::map<std::string, std::function<void(const LibraryItem&)>> adding_;
    std::set<std::string> failed_;
    std::string skew_;
    std::string build_;
    History history_;
    History savedHistory_;
    // The engine's snapshot said the document had edits its show file has not.
    bool unsavedAtSnapshot_ = false;
    std::string savedPath_;
    std::vector<AudioDevice> audioDevices_;
    AudioInputState audioInput_;
    AudioGain audioGain_;
    ModulationState modulation_;
    AudioLevels audio_;
    Message presets_ = Message::object();
    std::vector<PresetEntry> presetEntries_;
    std::map<std::string, std::string> presetDefaults_;
    Message chainPresets_ = Message::object();
    std::vector<std::string> macroSets_;
    std::optional<MacroSetPreview> macroSetPreview_;
    ShowList shows_;
    std::vector<OutputDisplay> displays_;
    bool unmirror_ = true;
    RecordState record_;
    FolderList folders_;
    EngineSettings settings_;
    bool watchingMachine_ = false;
    bool listening_ = false;
    std::function<void(const Message&)> listenSink_;
    std::optional<MachineStatus> machine_;
    Switches switches_;
    bool watchingVideo_ = false;
    ReturnVideoClient video_;
    UpdateProgress update_;
    std::optional<Message> updateReply_;
    std::condition_variable updateReplied_;
    bool stopping_ = false;
    std::thread updater_;
    uint64_t gesture_ = 0;
    uint64_t lastGesture_ = 0;
    std::unique_ptr<PreparedCopy> prepared_;
};

// The machine itself: previews can come through shared surfaces.
bool isLocalHost(const std::string& host);

} // namespace azl::ui
