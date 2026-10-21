#pragma once

#include "audio/audio_service.hpp"
#include "audio/return_audio.hpp"
#include "effects/preset_service.hpp"
#include "engine/clock_service.hpp"
#include "engine/cue_service.hpp"
#include "engine/desk_service.hpp"
#include "engine/engine.hpp"
#include "engine/isf_library.hpp"
#include "engine/isf_service.hpp"
#include "engine/machine_service.hpp"
#include "engine/modulation_service.hpp"
#include "engine/render_thread.hpp"
#include "engine/preview_service.hpp"
#include "engine/screen_painter.hpp"
#include "engine/settings_service.hpp"
#include "engine/switch_log.hpp"
#include "engine/trigger_service.hpp"
#include "engine/wall_cue_service.hpp"
#include "harness/fake_audio_input.hpp"
#include "harness/fake_displays.hpp"
#include "harness/image.hpp"
#include "media/composition_service.hpp"
#include "media/library_service.hpp"
#include "media/media_library.hpp"
#include "output/output_service.hpp"
#include "output/outputs.hpp"
#include "output/record_service.hpp"
#include "output/return_video_service.hpp"
#include "protocol/document_server.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

namespace azl {

// Runs an engine in-process on an injected clock: no render thread, no display link, no window. Clients edit its
// document, its Media Library and its beat clock through the protocol, either directly via document() or over a
// transport attached to it. Displays are simulated: screens bound to an attached fake display get a real output with
// its own present thread, and so is the audio input (FakeAudioInput). The settings, the picked audio input and the
// display per screen role are kept in the library root, so a harness on the same root starts with them as a restarted
// engine does.
class EngineHarness {
public:
    // An empty library root means a fresh temporary one; effect presets are kept in it too. `initial` is the
    // composition it loads, as the app loads its saved state: a screen whose role the root's map names goes to that
    // display.
    explicit EngineHarness(EngineConfig config = {}, double fps = 60.0, std::filesystem::path libraryRoot = {},
                           Composition initial = defaultComposition());
    ~EngineHarness();

    // Renders `frames` frames, the first at the current time, advancing the clock by 1/fps after each. Each frame
    // waits until every clip's frame for its time is decoded and every effect chain is compiled, so the picture
    // depends on the clock only.
    void step(int frames = 1);
    // While held, frames render as a live engine does during a compile: without waiting for it.
    void holdCompiles(bool held);
    // The composition picture shown full screen, as RGBA8 at composition size.
    Image readScreen();
    // What that screen's output shows right now, as RGBA8 at composition size.
    Image readScreen(uint64_t screen);
    // The newest frame's picture of a layer or group full screen, black where it has none, as RGBA8 at composition
    // size; the composition for 0.
    Image readPicture(uint64_t layer);
    // Renders for `seconds` of wall-clock time on a render thread at `fps`, phase-locked to the main output, as the
    // app does.
    void runLive(double seconds, double fps = 60.0);
    // The same until stopLive(); meanwhile time() is engine time, so clients act at the time the engine renders.
    void startLive(double fps = 60.0);
    void stopLive();
    // Fault injection: the decoder of the clip on `layer` fails once when it reaches clip frame `frame`.
    void injectDecoderFault(uint64_t layer, int frame) { engine_.injectDecoderFault(layer, frame); }
    // One read of that clip's decoder, open or not yet, reaching clip frame `frame` blocks for `duration`, as a stuck
    // or slow AVAssetReader.
    void injectDecoderStall(uint64_t layer, int frame, std::chrono::milliseconds duration)
    {
        engine_.injectDecoderStall(layer, frame, duration);
    }
    // While held, no clip opens and frames do not wait for one, as with a slow disk: a trigger of a clip that is not
    // open yet renders as live.
    void holdClipOpens(bool held) { engine_.holdClipOpens(held); }
    // Audio modulators read this analysis; it must outlive the harness or be reset to null first.
    void setAudio(AudioAnalysis* audio)
    {
        engine_.setAudio(audio);
        machineService_.setAudio(audio);
    }
    // Continues a saved show at the next frame, as a restarted engine does.
    void resume(double beat, std::vector<ResumePilot> pilots) { engine_.resume(beat, time(), std::move(pilots)); }
    TransportState transport() const { return engine_.transport(); }

    // The time of the next frame; beat clock actions sent between steps happen at it.
    double time() const;
    // The venue clock reads `unixNow` now and runs with time() from here, in `zone`; Cues at a time of day are checked
    // before each frame, starting over as after a restart. Without it the clock is the system's.
    void setWallClock(double unixNow, UtcOffset zone = systemUtcOffset);
    double wallTime() const;
    DocumentServer& document() { return document_; }
    Engine& engine() { return engine_; }
    // The library root's isf-user folder.
    UserIsfLibrary& userIsf() { return userIsf_; }
    MediaLibrary& library() { return library_; }
    FakeDisplays& displays() { return displays_; }
    KnownDisplays& knownDisplays() { return known_; }
    // This machine's display per screen role, kept in the library root's screen-displays.json.
    const ScreenDisplays& screenDisplays() const { return screenDisplays_; }
    Outputs& outputs() { return outputs_; }
    Recorder& recorder() { return recordService_.recorder(); }
    RecordService& recordService() { return recordService_; }
    // Writes into the recording folder, or the library root while no recording path is set.
    SwitchLog& switchLog() { return switchLog_; }
    // Keeps its state in the library root's desk.json.
    DeskService& desk() { return deskService_; }
    RenderStats& stats() { return engine_.stats(); }
    const DecodeStats& decodeStats() const { return engine_.decodeStats(); }
    CompileStats& compileStats() { return engine_.compileStats(); }
    const PreviewService::ClipStats& clipPreviewStats() const { return previewService_.clipStats(); }
    std::map<std::string, size_t> previewSizes() { return previewService_.sizes(); }
    const PreviewService::PaintStats& previewPaintStats() const { return previewService_.paintStats(); }
    std::map<std::string, size_t> previewWatchers() { return previewService_.watchers(); }
    const StillCounts& stills() const { return libraryService_.stills(); }
    // Clip previews are off by default, as on the app's engine without --clip-previews; a client may turn them on.
    void enableClipPreviews() { previewService_.setClipPreviews(true); }
    bool clipPreviews() const { return previewService_.clipPreviews(); }
    ClipEffectsPreview* clipEffects() { return previewService_.clipEffects(); }
    MachineService& machine() { return machineService_; }
    ReturnVideoService& returnVideo() { return returnVideoService_; }
    FakeAudioInput& audioInput() { return audioInput_; }
    // The analysis the audio service serves and return audio streams; it runs no thread: push, then pump.
    AudioAnalysis& analysis() { return analysis_; }
    ReturnAudio& returnAudio() { return returnAudio_; }
    SettingsService& settings() { return settingsService_; }
    // What the machine service reads as the machine's thermal state.
    void setThermal(Thermal thermal);
    // Called for each restart the machine service would ask the supervisor for; the Show lock is kept in the library
    // root, so a harness on the same root finds it.
    void onRestart(std::function<void(bool safeMode)> restart);
    // What the machine status reports as "media"; null without it.
    void onMediaStatus(std::function<Message()> media);
    // What the machine status reports as the clock's "djlink"; null without it.
    void onDjlinkStatus(std::function<Message()> djlink);
    // Called with each bundle Update has received and unpacked (in the library root's "incoming"), in place of the
    // installer; the installer's result is read from the library root's update.json.
    void onInstall(InstallBundle install);

private:
    Image paint(const ScreenView& view);
    // The render and present figures since the previous call, as the app's stats printer collects them each second;
    // calls less than half a second apart get the same figures. A stepped harness has no render rate: fps is null
    // unless it rendered live since the previous call.
    Message machineStats();
    // One of the machine status's sources set by a test, called outside the lock; null when unset.
    Message machineSource(const std::function<Message()>& source);

    double fps_;
    uint64_t frame_ = 0;
    bool compilesHeld_ = false;
    std::unique_ptr<RenderThread> live_;
    std::atomic<bool> liveClock_{false};
    std::atomic<bool> ranLive_{false};
    NS::SharedPtr<MTL::Device> device_;
    std::filesystem::path libraryRoot_;
    MediaLibrary library_;
    UserIsfLibrary userIsf_;
    Engine engine_;
    FakeDisplays displays_;
    KnownDisplays known_;
    ScreenDisplays screenDisplays_;
    Outputs outputs_;
    std::mutex compositionMutex_;
    std::shared_ptr<const Composition> composition_;
    DocumentServer document_;
    LibraryService libraryService_;
    CompositionService compositionService_;
    ClockService clockService_;
    OutputService outputService_;
    TriggerService triggerService_;
    CueService cueService_;
    std::optional<double> wallBase_;
    UtcOffset zone_ = systemUtcOffset;
    WallCueService wallCueService_;
    PresetService presetService_;
    IsfService isfService_;
    ModulationService modulationService_;
    PreviewService previewService_;
    ReturnVideoService returnVideoService_;
    RecordService recordService_;
    SwitchLog switchLog_;
    DeskService deskService_;
    FakeAudioInput audioInput_;
    AudioAnalysis analysis_;
    AudioService audioService_;
    ReturnAudio returnAudio_;
    SettingsService settingsService_;
    ScreenPainter painter_;
    NS::SharedPtr<MTL::CommandQueue> queue_;
    std::mutex machineMutex_;
    Thermal thermal_;
    std::function<void(bool)> restart_;
    std::function<Message()> media_;
    std::function<Message()> djlink_;
    InstallBundle install_;
    std::chrono::steady_clock::time_point statsAt_ = std::chrono::steady_clock::now();
    uint64_t rendered_ = 0;
    uint64_t gpuMicros_ = 0;
    std::map<std::string, std::pair<uint64_t, uint64_t>> presentsAndHitches_;
    Message stats_;
    MachineService machineService_;
};

} // namespace azl
