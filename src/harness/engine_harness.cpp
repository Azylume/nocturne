#include "harness/engine_harness.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace azl {

namespace {

constexpr MTL::PixelFormat kReadbackFormat = MTL::PixelFormatRGBA8Unorm;

std::filesystem::path temporaryLibrary()
{
    std::string pattern = (std::filesystem::temp_directory_path() / "nocturne-library-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
        throw std::runtime_error("cannot create a temporary media library");
    return pattern;
}

EngineConfig deterministic(EngineConfig config)
{
    config.waitForMedia = true;
    return config;
}

// Tests record to temporary folders whatever the machine's disk holds.
Recorder::Settings testRecording()
{
    Recorder::Settings settings;
    settings.reserveBytes = uint64_t(256) << 20;
    settings.reservePercent = 0.0;
    return settings;
}

MachineSettings settingsOf(const EngineConfig& config)
{
    MachineSettings settings;
    settings.frameCacheMB = config.clipCacheBytes >> 20;
    settings.preloadMB = config.preloadBytes >> 20;
    return settings;
}

Composition placedByRole(Composition composition, const RoleDisplays& roles)
{
    std::vector<std::string> warnings;
    placeByRole(composition, roles, warnings);
    return composition;
}

} // namespace

EngineHarness::EngineHarness(EngineConfig config, double fps, std::filesystem::path libraryRoot, Composition initial)
    : fps_(fps), device_(NS::TransferPtr(MTL::CreateSystemDefaultDevice())),
      libraryRoot_(libraryRoot.empty() ? temporaryLibrary() : libraryRoot), library_(libraryRoot_),
      userIsf_(libraryRoot_ / "isf-user", libraryRoot_ / "isf"),
      engine_(device_.get(), deterministic(config), &library_), screenDisplays_(libraryRoot_ / "screen-displays.json"),
      outputs_(engine_, displays_, {}, &known_),
      document_(placedByRole(std::move(initial), screenDisplays_.roles()),
                [this](auto composition) {
                    screenDisplays_.keep(*composition);
                    engine_.setComposition(composition);
                    outputs_.setComposition(composition);
                    std::lock_guard lock(compositionMutex_);
                    composition_ = std::move(composition);
                }),
      libraryService_(document_, library_, [this](const std::string& hash) { engine_.mediaAdded(hash); }),
      compositionService_(document_, library_,
                          [this] { return DisplayPlacement{displayIds(displays_), known_.ids(), false, screenDisplays_.roles()}; },
                          {libraryRoot_ / "shows", {libraryRoot_}}),
      clockService_(document_, engine_, [this] { return time(); }),
      outputService_(document_, displays_, outputs_, known_, screenDisplays_, [](auto work) { work(); }),
      triggerService_(document_, engine_, [this] { return time(); }),
      cueService_(document_, engine_, [this] { return time(); }),
      wallCueService_(document_, engine_, [this] { return time(); }, [this] { return wallTime(); },
                      [this](int64_t t) { return zone_(t); }),
      presetService_(document_, libraryRoot_ / "presets.json"),
      isfService_(document_, userIsf_, [this] { engine_.refreshEffects(); }),
      modulationService_(document_, engine_, libraryRoot_ / "macro-sets.json"), previewService_(document_, engine_),
      returnVideoService_(document_, engine_),
      recordService_(document_, engine_, fps, testRecording()),
      switchLog_(document_, {.now = [this] { return time(); },
                             .wall = [this] { return wallTime(); },
                             .zone = [this](int64_t t) { return zone_(t); },
                             .folder =
                                 [this] {
                                     const std::filesystem::path path = recordService_.settings().path;
                                     return path.empty() ? libraryRoot_ : Recorder::movieFile(path).parent_path();
                                 },
                             .recorder = &recordService_.recorder()}),
      deskService_(document_, {.wall = [this] { return wallTime(); },
                               .zone = [this](int64_t t) { return zone_(t); },
                               .file = libraryRoot_ / "desk.json"}),
      analysis_([this] { return time(); }, {.thread = false}),
      audioService_(document_, analysis_, &audioInput_, [this] { return time(); }, {},
                    libraryRoot_ / "audio-devices.json"),
      returnAudio_(document_, analysis_),
      settingsService_(document_, startingSettings(loadMachineSettings(libraryRoot_ / "settings.json", settingsOf(config)), {}),
                       libraryRoot_ / "settings.json",
                       [this](const MachineSettings& s) { applySettings(s, engine_, previewService_); },
                       [this] { return settingsUse(engine_, previewService_); }),
      // NOCTURNE_OPAQUE_OUTPUTS=1 reads every screen back as --opaque-outputs draws it, to check it changes nothing.
      painter_(device_.get(), kReadbackFormat, {config.width, config.height}, config.debugFrameBits,
               config.opaqueOutputs || std::getenv("NOCTURNE_OPAQUE_OUTPUTS")),
      queue_(NS::TransferPtr(device_->newCommandQueue())),
      machineService_(document_, engine_, displays_, &known_, [this] { return time(); }, [](auto work) { work(); },
                      {.stats = [this] { return machineStats(); },
                       .thermal =
                           [this] {
                               std::lock_guard lock(machineMutex_);
                               return thermal_;
                           },
                       .djlink = [this] { return machineSource(djlink_); },
                       .media = [this] { return machineSource(media_); },
                       .identify = [this](double seconds) { outputs_.identify({}, seconds); },
                       .unmirror = [this] { return outputService_.unmirrorNow(); },
                       .showLock = libraryRoot_ / "show-lock.json",
                       .restart =
                           [this](bool safeMode) {
                               std::lock_guard lock(machineMutex_);
                               if (restart_)
                                   restart_(safeMode);
                           },
                       .update = {.incoming = libraryRoot_ / "incoming",
                                  .install = [this](const std::filesystem::path& app) -> std::optional<std::string> {
                                      std::lock_guard lock(machineMutex_);
                                      return install_ ? install_(app) : "no installer in the harness";
                                  },
                                  .result = libraryRoot_ / "update.json"},
                       .wall = [this] { return wallTime(); },
                       .zone = [this](int64_t t) { return zone_(t); }})
{
    engine_.onChainFailure(
        [this](const auto& chain, const auto& error) { document_.broadcast(msg::chainFailed(chain, error)); });
    previewService_.setClipLibrary(&library_);
    recordService_.setWallClock([this] { return wallTime(); }, [this](int64_t t) { return zone_(t); });
    if (std::filesystem::exists(libraryRoot_ / "settings.json"))
        applySettings(settingsService_.settings(), engine_, previewService_);
    std::string log;
    const AudioChoice input = startingInput(
        {}, AudioDeviceSettings(libraryRoot_ / "audio-devices.json", true, 0.0).picked(), audioInput_.inputDevices(), log);
    audioInput_.select(input.device, input.channels);
}

// The document goes before the engine, so no compile may report to it any more.
EngineHarness::~EngineHarness()
{
    stopLive();
    holdCompiles(false);
    engine_.onChainFailure(nullptr);
    engine_.waitForCompiles();
}

void EngineHarness::step(int frames)
{
    for (int i = 0; i < frames; ++i) {
        wallCueService_.poll();
        if (!compilesHeld_)
            engine_.waitForCompiles();
        engine_.renderFrame(time());
        engine_.waitUntilIdle();
        engine_.waitForDecoders();
        ++frame_;
    }
}

void EngineHarness::holdCompiles(bool held)
{
    compilesHeld_ = held;
    engine_.holdCompiles(held);
}

Image EngineHarness::readScreen()
{
    return paint(ScreenView{Screen{.slices = {Slice{}}}});
}

Image EngineHarness::readScreen(uint64_t id)
{
    std::shared_ptr<const Composition> composition;
    {
        std::lock_guard lock(compositionMutex_);
        composition = composition_;
    }
    for (const Screen& screen : composition->screens)
        if (screen.id == id)
            return paint(outputs_.view(*composition, screen));
    throw std::runtime_error("no screen " + std::to_string(id));
}

Image EngineHarness::readPicture(uint64_t layer)
{
    return paint(ScreenView{Screen{.slices = {Slice{.input = layer}}}});
}

double EngineHarness::time() const
{
    return liveClock_ ? engineTime() : double(frame_) / fps_;
}

void EngineHarness::setWallClock(double unixNow, UtcOffset zone)
{
    wallBase_ = unixNow - time();
    zone_ = std::move(zone);
    wallCueService_.restart();
}

double EngineHarness::wallTime() const
{
    return wallBase_ ? *wallBase_ + time() : unixTime();
}

void EngineHarness::runLive(double seconds, double fps)
{
    startLive(fps);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stopLive();
}

void EngineHarness::startLive(double fps)
{
    liveClock_ = true;
    ranLive_ = true;
    live_ = std::make_unique<RenderThread>(engine_, fps, &outputs_.vsync());
}

void EngineHarness::stopLive()
{
    live_.reset();
    liveClock_ = false;
}

void EngineHarness::setThermal(Thermal thermal)
{
    std::lock_guard lock(machineMutex_);
    thermal_ = std::move(thermal);
}

void EngineHarness::onRestart(std::function<void(bool safeMode)> restart)
{
    std::lock_guard lock(machineMutex_);
    restart_ = std::move(restart);
}

void EngineHarness::onMediaStatus(std::function<Message()> media)
{
    std::lock_guard lock(machineMutex_);
    media_ = std::move(media);
}

void EngineHarness::onDjlinkStatus(std::function<Message()> djlink)
{
    std::lock_guard lock(machineMutex_);
    djlink_ = std::move(djlink);
}

Message EngineHarness::machineSource(const std::function<Message()>& source)
{
    std::function<Message()> copy;
    {
        std::lock_guard lock(machineMutex_);
        copy = source;
    }
    return copy ? copy() : Message(nullptr);
}

void EngineHarness::onInstall(InstallBundle install)
{
    std::lock_guard lock(machineMutex_);
    install_ = std::move(install);
}

Message EngineHarness::machineStats()
{
    std::lock_guard lock(machineMutex_);
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - statsAt_).count();
    if (s < 0.5 && !stats_.is_null())
        return stats_;
    const auto since = [](uint64_t now, uint64_t& last) {
        const uint64_t delta = now >= last ? now - last : 0;
        last = now;
        return delta;
    };
    const uint64_t frames = since(engine_.stats().rendered.load(), rendered_);
    const uint64_t gpu = since(engine_.stats().gpuMicros.load(), gpuMicros_);
    Message outputs = Message::array();
    for (DisplayOutput* output : outputs_.all()) {
        auto& [presents, hitches] = presentsAndHitches_[output->display()];
        outputs.push_back({{"display", output->display()},
                           {"presents", since(output->stats().presents.load(), presents) / s},
                           {"hitches", since(output->stats().hitches.load(), hitches)}});
    }
    const bool live = ranLive_.exchange(liveClock_.load());
    statsAt_ = now;
    stats_ = {{"render",
               {{"fps", live ? Message(frames / s) : Message(nullptr)},
                {"gpuMs", frames ? gpu / 1000.0 / double(frames) : 0.0}}},
              {"outputs", outputs}};
    return stats_;
}

Image EngineHarness::paint(const ScreenView& view)
{
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    const uint32_t width = engine_.config().width;
    const uint32_t height = engine_.config().height;
    const size_t rowBytes = size_t(width) * 4;

    auto desc = NS::RetainPtr(MTL::TextureDescriptor::texture2DDescriptor(kReadbackFormat, width, height, false));
    desc->setUsage(MTL::TextureUsageRenderTarget);
    desc->setStorageMode(MTL::StorageModePrivate);
    auto target = NS::TransferPtr(device_->newTexture(desc.get()));
    auto buffer = NS::TransferPtr(device_->newBuffer(rowBytes * height, MTL::ResourceStorageModeShared));

    const FrameRef frame = engine_.latestFrame();
    auto* cmd = queue_->commandBuffer();
    painter_.encode(cmd, frame.get(), view, target.get());
    auto* blit = cmd->blitCommandEncoder();
    blit->copyFromTexture(target.get(), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1), buffer.get(), 0,
                          rowBytes, rowBytes * height);
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();

    Image image{width, height, std::vector<uint8_t>(rowBytes * height)};
    std::memcpy(image.rgba.data(), buffer->contents(), image.rgba.size());
    pool->release();
    return image;
}

} // namespace azl
