#pragma once

#include "audio/audio_analysis.hpp"
#include "document/composition.hpp"
#include "document/composition_file.hpp"
#include "document/cue_edits.hpp"
#include "engine/autopilot_stepper.hpp"
#include "engine/effect_stepper.hpp"
#include "engine/beat_clock.hpp"
#include "engine/chain_compiler.hpp"
#include "engine/frame_guard.hpp"
#include "engine/frame_ring.hpp"
#include "engine/frame_trace.hpp"
#include "engine/fsr_upscaler.hpp"
#include "engine/modulation_plan.hpp"
#include "engine/gpu_profile.hpp"
#include "engine/playhead.hpp"
#include "engine/side_targets.hpp"
#include "engine/static_bake.hpp"
#include "engine/transport.hpp"
#include "media/clip_decoders.hpp"
#include "media/text_renderer.hpp"
#include "platform/profile.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace azl {

struct EngineConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    // Enough for three outputs to hold one frame each while three are kept as the latest and one is being rendered,
    // with one to spare for a frame still on the GPU.
    int frameCount = 9;
    // How many rendered frames may be on the GPU at once; a frame due while that many are waits for one to finish,
    // up to a frame's time, and is skipped when none has. Under GPU overload frames queue up ahead of the outputs' presents (ADR 0003), and the
    // bound lets the presents through. 0: as many as the ring allows.
    int maxInFlight = 0;
    // Deterministic mode for the harness: a frame waits until every clip's frame for its time is decoded. Live
    // rendering never waits.
    bool waitForMedia = false;
    // Bounds the two GOPs a clip playing backward holds. 2 GiB fits two 5 s GOPs of 1080p60 (about 1.9 GB), so reverse
    // decodes each frame once. What a clip actually uses depends on how it plays (see VideoDecoder).
    size_t clipCacheBytes = size_t(2) << 30;
    // Bounds the frames of the clips preloaded for the next trigger. A forward start holds a second (90 MB at 1080p30);
    // a backward start holds the unit up to its first frame, at most half the clip cache (a 5 s GOP of 1080p60 is
    // 0.9 GB). 3 GiB of the Mac mini's 16 GB leaves room for the clips that play.
    size_t preloadBytes = size_t(3) << 30;
    // Seeds Random Jump and random Autopilot; a fixed seed repeats them exactly.
    uint32_t seed = 0;
    // Outputs draw the render frame index as a strip of bars, so dropped or repeated frames are visible.
    bool debugFrameBits = false;
    // Outputs draw opaque slices of the composition without blending (ScreenPainter's opaqueSlices).
    bool opaqueOutputs = false;
    // Harness: every decoded frame takes this much longer, to load the decoders as a busy machine does.
    std::chrono::microseconds decodeFrameCost{0};
    // Start-up: no layer starts until every clip the composition plays or may play next has opened and decoded its
    // start and every effect chain is compiled (at most Engine::kMaxStartHold seconds), then all start together. The
    // outputs stay black meanwhile, instead of the first seconds missing frames while many clips open at once. A resume
    // is held too: the beat clock runs on while every playing clip opens and decodes at its resumed position and the
    // preloads fill, and the first frame drawn shows every layer where the clock has got to by then. While the outputs
    // are black, the decoders decode at full speed rather than at their pace.
    bool holdStart = false;
    // Where compiled pipelines persist across starts (ChainCompiler); none when empty.
    const char* pipelineCache = nullptr;
    // GPU time of every pass by layer, fade side and effects (Engine::takeGpuProfile). It splits the composition's
    // single encoder into one per layer and costs a little GPU time.
    bool gpuProfile = false;
    // The frame budget guard's half-resolution layers scale up with FSR 1 instead of bilinearly.
    bool fsrUpscale = false;
    // Every rendered frame's timeline goes here when set.
    FrameTrace* frameTrace = nullptr;
    // A chain's leading passes whose inputs stay the same are drawn once into a kept texture and read from it after
    // (StaticBake), with the same picture.
    bool bakeStatic = false;
};

struct RenderStats {
    std::atomic<uint64_t> rendered{0};
    std::atomic<uint64_t> skipped{0};
    // Frames where a playing clip's frame was not decoded in time, so its layer repeated its previous picture. The
    // harness waits for the frame instead, but counts it all the same.
    std::atomic<uint64_t> videoMisses{0};
    // The same by layer id, until taken.
    std::mutex missesMutex;
    std::map<uint64_t, uint64_t> missesByLayer;
    // GPU passes the last rendered frame drew, intermediates, group passes and copies for shader blend modes included.
    std::atomic<uint64_t> passes{0};
    // Full-resolution textures the last rendered frame wrote besides the composition and group stacks: materialised
    // intermediates and full-resolution side targets such as Trails history.
    std::atomic<uint64_t> intermediates{0};
    // GPU time of finished frames, summed.
    std::atomic<uint64_t> gpuMicros{0};
    // The longest GPU time of one frame since it was last reset.
    std::atomic<uint64_t> maxGpuMicros{0};
    // The longest time between the starts of two consecutive frames on a render thread since it was last reset: a
    // stalled render thread shows here.
    std::atomic<uint64_t> maxGapMicros{0};
    // The longest a frame took on the render thread, and from its start until its GPU work completed.
    std::atomic<uint64_t> maxCpuMicros{0};
    std::atomic<uint64_t> maxDoneMicros{0};
    // From a frame's render start until its GPU work started, summed over finished frames, and the longest: how long
    // frames queue on the GPU.
    std::atomic<uint64_t> queueMicros{0};
    std::atomic<uint64_t> maxQueueMicros{0};
    // Frames that waited for one to leave the GPU (EngineConfig::maxInFlight).
    std::atomic<uint64_t> throttled{0};
    // Render-thread container sizes as of the last frame, to see that nothing grows over a long show.
    std::atomic<uint64_t> layerStates{0};
    std::atomic<uint64_t> fading{0};
    std::atomic<uint64_t> chains{0};
    std::atomic<uint64_t> sideTargets{0};
    std::atomic<uint64_t> sideSpares{0};
    std::atomic<uint64_t> pooledTextures{0};
    // The start-up hold is on (EngineConfig::holdStart).
    std::atomic<bool> starting{false};
    // The frame budget guard (Engine::setFrameBudget): how many layers it draws at half resolution, and its level
    // changes.
    std::atomic<uint64_t> budgetLevel{0};
    std::atomic<uint64_t> budgetChanges{0};
    // Passes the last rendered frame drew only for preview watches (setPreviewPictures): the pictures of layers no
    // slice reads, and everything of those the composition does not show. Their GPU time, summed, is part of gpuMicros;
    // without counter sampling it is the frames' GPU time by those passes' share.
    std::atomic<uint64_t> previewPasses{0};
    std::atomic<uint64_t> previewGpuMicros{0};
    // Passes the last rendered frame read from kept results of static chain prefixes instead of drawing them, and the
    // draws keeping one (EngineConfig::bakeStatic).
    std::atomic<uint64_t> bakedPasses{0};
    std::atomic<uint64_t> bakes{0};
};

// The beat clock and every clip's transport as of the last rendered frame.
struct TransportState {
    struct Clip {
        uint64_t layer = 0;
        // The cell it plays from.
        uint64_t column = 0;
        int frame = 0;
        // The beat count in use, automatic or set.
        int beats = 0;
        bool visible = true;
        // The column Autopilot plays next, 0 when it is off.
        uint64_t next = 0;
        // The bars left of a trigger's hold on Autopilot, this one included; 0 when not held.
        int held = 0;
    };
    double time = 0.0;
    double beat = 0.0;
    double bpm = 120.0;
    double tempo = 120.0;
    double nudge = 0.0;
    // The layers the frame budget guard draws at half resolution.
    uint64_t budgetLevel = 0;
    BeatTimeline timeline;
    std::vector<Clip> clips;
    // Every layer Autopilot steps, and the column Autopilot as layer 0.
    std::vector<ResumePilot> pilots;
    // The column Autopilot: the column it triggered last, the one it plays next and the bars left of a trigger's hold
    // on it, this one included; all 0 when it is off.
    struct Columns {
        uint64_t column = 0;
        uint64_t next = 0;
        int held = 0;
    };
    Columns columns;
    // Every effect with steps: the step it is on, its Autopilot's next and the bars left of a trigger's hold.
    std::vector<EffectStepper::Shown> effects;
    // The palette Autopilot as `effects` shows an effect's (none without palettes), and each palette slot's colour as
    // the last frame drew it.
    std::optional<EffectStepper::Shown> palette;
    std::vector<Rgba> paletteColours;
    // The cell each layer plays, whether its trigger's or Autopilot's.
    struct Playing {
        uint64_t layer = 0;
        std::optional<Play> play;
    };
    std::vector<Playing> playing;
    // The start-up hold is on and the outputs are black.
    bool starting = false;
    // The time of the first frame drawn after the hold, 0 until then.
    double startedAt = 0.0;
};

// Sees every rendered frame on the render thread, e.g. to record it.
class FrameTap {
public:
    using Done = std::function<void(bool ok)>;
    virtual ~FrameTap() = default;
    // Adds GPU work reading `frame` to the frame's command buffer and must never wait. What it returns runs once that
    // command buffer completed, with whether it succeeded.
    virtual Done encode(gpu::CommandBuffer& cmd, const Frame& frame, const Composition& composition,
                        double seconds) = 0;
};

// Renders the composition into the frame ring. It has no clock of its own: whoever calls renderFrame decides time.
class Engine {
public:
    static constexpr double kMaxStartHold = 5.0;
    // Auto gain finds its level in about a second of audio, so a start driven by audio holds at least that long and
    // its modulators are right from the first frame shown.
    static constexpr double kAudioSettle = 1.0;

    // Clips resolve their media through `library`; without one, layers with clips keep the test card.
    Engine(const gpu::Device& device, EngineConfig config = {}, MediaLibrary* library = nullptr);
    ~Engine();

    // Any thread. The render thread picks it up at its next frame and never waits for the document.
    void setComposition(std::shared_ptr<const Composition> composition);

    // Any thread: the composition as of the last setComposition.
    std::shared_ptr<const Composition> composition() const;
    // Any thread: these layers and groups publish their pictures with every frame, as if a slice read them.
    void setPreviewPictures(std::set<uint64_t> layers);
    // Any thread: from the next frame on, `tap` sees every rendered frame; null removes it.
    void setFrameTap(std::shared_ptr<FrameTap> tap);

    // Audio modulators read `audio` once per frame, at the frame's time less its latency offset. Without one they read
    // silence. The engine keeps its band edges on the composition's.
    void setAudio(AudioAnalysis* audio);
    const AudioAnalysis* audio() const { return audio_.load(); }
    // Any thread: the macros and modulators as of the last rendered frame.
    std::shared_ptr<const ModulationPlan> modulation() const;

    // Render thread only. The frame is published once the GPU has finished it.
    void renderFrame(double seconds);
    void waitUntilIdle();

    // Any thread, never blocks the render thread: live beat clock actions at engine time `seconds`, applied at the
    // next frame. A nudge of 0 percent releases it.
    void nudge(double percent, double seconds);
    void resync(double seconds);
    // Any thread, never blocks the render thread: moves the clip on `layer` to `position` (0..1 of the clip) at the
    // next frame; it plays on from there.
    void scrub(uint64_t layer, double position);
    // Any thread, before or while rendering: continues a saved show at the next frame, with `beat` at engine time
    // `seconds` and Autopilot where it was on every layer that still plays the same trigger.
    void resume(double beat, double seconds, std::vector<ResumePilot> pilots);
    // Any thread: from the next frame the beat clock follows `timeline` (another engine's, mapped onto this engine's
    // time) and ignores live clock actions and the document's tempo, until unfollow. Then it runs on by itself from
    // where it is. With `fromClock`, a follow that starts now bends from the clock's own beat like any later target,
    // instead of jumping onto the timeline.
    void follow(BeatTimeline timeline, bool fromClock = false);
    void unfollow();
    // Any thread: Random Jump and random Autopilot draw from this seed (EngineConfig::seed until set), so engines
    // with the same seed make the same choices; it applies to clips and steps that start from now on.
    void setSeed(uint32_t seed) { seed_ = seed; }
    uint32_t seed() const { return seed_; }
    // Any thread: at the next frame, Autopilot continues these pilots on every layer that still plays their trigger
    // (another engine's, so a node joining mid-show steps as the main engine does).
    void adoptPilots(std::vector<ResumePilot> pilots);
    // Any thread: media with this hash arrived in the library, so clips that could not open it try again.
    void mediaAdded(const std::string& hash) { decoders_.retry(hash); }
    TransportState transport() const;

    FrameRef latestFrame() const { return ring_->latest(); }
    FrameRef nextFrame(uint64_t shown, std::chrono::steady_clock::time_point time) const
    {
        return ring_->next(shown, time);
    }
    const gpu::Device& gpuDevice() const { return device_; }
    // The backend's device, for the call sites not yet on the GPU interface.
    gpu::NativeDevice device() const { return device_.native(); }
    // The Tracy timeline of the render queue, for encoders that write into the frame's command buffer.
    const profile::GpuTimeline& gpuTimeline() const { return gpu_; }
    const EngineConfig& config() const { return config_; }
    RenderStats& stats() { return stats_; }
    // Any thread. When the GPU time of recent frames nears `seconds`, the most expensive layers draw at half
    // resolution, one more each time it stays near, until the frames fit again with room to spare; 0 turns it off.
    void setFrameBudget(double seconds) { frameBudget_ = seconds; }
    // EngineConfig::maxInFlight, changed live.
    void setMaxInFlight(int frames) { maxInFlight_ = frames; }
    // How the frame budget guard trades resolution for frame rate (guardThresholds). Any thread.
    void setGuardProfile(GuardProfile profile, bool protectTransitions)
    {
        guardProfile_ = profile;
        protectTransitions_ = protectTransitions;
    }
    // The clips' frame caches and the preload budget (ClipDecoders::setLimits). Any thread.
    void setCacheLimits(size_t clipCacheBytes, size_t preloadBytes) { decoders_.setLimits(clipCacheBytes, preloadBytes); }
    ClipDecoders::Use cacheUse() { return decoders_.use(); }
    // Any thread: from the next frame on, static chain prefixes are kept and reused (EngineConfig::bakeStatic).
    void setBakeStatic(bool on) { bakeStatic_ = on; }
    bool bakeStatic() const { return bakeStatic_; }
    // Any thread: from the next frame on, the layers below the lowest one showing text or kept at output size render at
    // `percent` (100, 83 or 67) of the composition size into a stack of their own, which one pass scales up into the
    // frame.
    void setRenderScale(int percent) { renderScale_ = percent; }
    int renderScale() const { return renderScale_; }
    // Any thread: GPU time by pass label since the last take; empty unless EngineConfig::gpuProfile.
    std::map<std::string, GpuProfile::Total> takeGpuProfile();
    const DecodeStats& decodeStats() const { return decoders_.stats(); }
    CompileStats& compileStats() { return compiler_.stats(); }
    // Any thread: container sizes, for the stats.
    std::map<std::string, size_t> sizes();

    // For the harness: hold effect-chain compiles back, or wait until every queued chain is compiled.
    void holdCompiles(bool held) { compiler_.hold(held); }
    // Any thread. Reports a chain that failed to compile; its layers keep what they showed.
    void onChainFailure(ChainCompiler::Failure failure) { compiler_.onFailure(std::move(failure)); }
    // Any thread: compiles every chain again for the registered effects and ISF transitions as they are now.
    void refreshEffects();
    void waitForCompiles() { compiler_.waitIdle(); }
    // For the harness: wait until every clip has decoded what its cache wants, so the next frame finds exactly what a
    // decoder keeping up would have ready.
    void waitForDecoders() { decoders_.waitIdle(); }
    // Fault injection for the harness: the decoder of the clip on `layer` fails once when it reaches frame `index`.
    void injectDecoderFault(uint64_t layer, int index);
    // Fault injection for the harness: one read of that decoder reaching frame `index` blocks for `duration`.
    void injectDecoderStall(uint64_t layer, int index, std::chrono::milliseconds duration);
    // For the harness: no clip opens while held, as with a slow disk.
    void holdClipOpens(bool held) { decoders_.holdOpens(held); }
    // Any thread. Reports a clip that cannot be played; its layer keeps what it showed.
    void onClipFailure(ClipDecoders::OnFailure failure) { decoders_.onFailure(std::move(failure)); }

private:
    // What a layer plays and showed last, kept until its clip delivers a newer frame. Null means the test card.
    struct LayerState : Playhead {
        std::optional<Play> play;
        // A trigger waiting for its beat, and the resync count when it was first seen: a resync starts it at once.
        std::optional<Play> pending;
        uint64_t pendingResyncs = 0;
        std::string hash;
        int frame = 0;
        bool visible = true;
        // The layer has shown a clip's picture, so a frame not ready in time is a miss.
        bool delivered = false;
        VideoFrameRef shown;
        // The clip a transition moves away from, playing on until the transition ends.
        std::unique_ptr<LayerState> from;
        Transition fade;
        // The ISF transition's parameter values.
        std::array<float, kMaxTransitionParams> fadeParams{};
        double fadeStart = 0.0;
        double fadeProgress = 0.0;
        uint64_t fadeResyncs = 0;
        // A text clip's look as of its last frame, which its picture is drawn with.
        TextClip look;
    };

    struct ClockAction {
        double seconds = 0.0;
        // A nudge in percent, or a resync when empty.
        std::optional<double> nudge;
    };

    struct Resumed {
        double beat = 0.0;
        double seconds = 0.0;
        std::vector<ResumePilot> pilots;
    };

    double advanceClock(const Composition& composition, double seconds);
    bool holding(double seconds, bool ready);
    void applyResume(const Composition& composition, const Resumed& resumed);
    void rekey();
    bool playable(const Layer& layer, uint64_t column);
    void switchIfDue(const Layer& layer, const Composition& composition, const std::optional<Play>& playing,
                     double seconds, double beat);
    void start(LayerState& state, const Layer& layer, const Transition& transition, const std::optional<Play>& playing,
               double seconds, double beat, std::optional<double> at);
    void endFade(LayerState& state, double beat);
    void updateWants(const Composition& composition, double beat);
    bool updateLayer(const Layer& layer, LayerState& state, double seconds, double beat, std::optional<double> scrub);
    bool updateText(const Layer& layer, LayerState& state);
    std::array<float, 12> textUniforms(uint64_t layer, const LayerState& state) const;
    void countMiss(const LayerState& state, const ClipKey& key);
    void addMisses(uint64_t layer, uint64_t count);
    void publishTransport(const Composition& composition, double seconds, double beat, bool held);

    // The compiled chain a layer, a transition's outgoing clip or the composition draws with. While its current chain
    // compiles, or when it failed, it keeps the previous one with the parameter values of the document that chain
    // belongs to; without one it shows its source with no effects.
    struct LayerChain {
        std::shared_ptr<const CompiledChain> chain;
        std::shared_ptr<const Composition> document;
        EffectList effects;
    };

    // A layer, or a group with the layers it shows, as this frame draws it.
    struct Draw {
        const std::vector<CompiledPass>* passes;
        // Holds the parameter values the passes read.
        const EffectList* effects;
        // Null for the test card and for a group.
        const VideoFrame* picture;
        float opacity;
        BlendMode blend;
        uint64_t layer;
        std::vector<Draw> layers;
        // A group's sub-stack, which its first pass reads.
        const gpu::Texture* source = nullptr;
        // What the last pass reads.
        const gpu::Texture* input = nullptr;
        // Drawn into the stack it is in; false when only its picture is published.
        bool composite = true;
        // The layer whose picture this draw publishes with the frame, or 0.
        uint64_t publish = 0;
        // During a transition, the outgoing clip through its own clip chain and its deck's layer chain; its picture is
        // null when it shows nothing.
        // Both render into `mixed`, which the transition pass mixes in place of the last pass.
        std::vector<Draw> from{};
        TransitionMix mix{};
        const gpu::Texture* mixed[2] = {};
        // Instead, during an alpha transition between two clips with only the same layer chain, the outgoing clip's
        // picture, which the chain's YuvFade passes mix in at their source so the chain runs once.
        const VideoFrame* fadeFrom = nullptr;
        // Or during any other transition with a layer chain: `layers` holds the layer's clips through their own
        // chains, mixed by the transition, which the layer's chain reads once as a group reads its sub-stack.
        bool clipMix = false;
        // The outgoing clip's blend mode when its deck's differs: then each clip's share composites on its own.
        std::optional<BlendMode> fromBlend{};
        // Drawn at the composition size divided by this, into `reduced`, which the group pass scales up.
        uint32_t divisor = 1;
        const gpu::Texture* reduced = nullptr;
        // Drawn at this size before the divisor at a render scale; 0 is the composition size.
        uint32_t width = 0;
        uint32_t height = 0;
        // Its sub-stack is drawn at its layers' size, which its pass scales up.
        bool upscales = false;
        // Drawn at output size at any render scale (Layer::fullResolution).
        bool fullResolution = false;
        // The composition's sub-stack starts opaque black, as the frame does.
        bool opaque = false;
        // A text picture's colours and placement.
        std::array<float, 12> text{};
    };

    struct Size {
        uint32_t width;
        uint32_t height;
    };

    // The resources a frame takes from the render thread's pools, and its pass count.
    struct FrameEncoding {
        gpu::CommandBuffer cmd;
        Frame& frame;
        float seconds;
        size_t intermediates = 0;
        size_t groups = 0;
        size_t fades = 0;
        size_t reduced = 0;
        uint64_t passes = 0;
        uint64_t fullResolutionTargets = 0;
        // While profiling: the frame's timestamps and the label of the next pass.
        std::shared_ptr<GpuProfile::Frame> profile{};
        std::string label{};
        // Layers published only for preview watches, whether the pass being encoded is drawn only for them, those
        // passes and their timestamps.
        std::set<uint64_t> previewOnly{};
        bool preview = false;
        uint64_t previewPasses = 0;
        std::shared_ptr<GpuProfile::Frame> previewTimes{};
        uint64_t bakedPasses = 0;
    };

    // A draw's kept static prefix.
    struct Baked {
        StaticBake state;
        gpu::Texture texture;
        // Keeps the baked picture's pixels from going back to its decoder's pool.
        std::shared_ptr<void> picture;
    };
    // What encodeInputs does with a draw's static prefix this frame. `kept` is the last intermediate the prefix
    // materialises, which `texture` keeps; none when the prefix has only side passes.
    struct BakePlan {
        StaticBake::Action action = StaticBake::Action::Live;
        size_t prefix = 0;
        std::optional<size_t> kept{};
        const gpu::Texture* texture = nullptr;
    };

    // `slot` is a layer id, a layer's fadeId for its outgoing clip, or 0 for the composition. Until the slot's first
    // chain is compiled it has none, so nothing shows; only a chain that failed falls back to no effects.
    const LayerChain* pickChain(uint64_t slot, ChainKind kind, EffectList effects,
                                const std::shared_ptr<const Composition>& document, bool text = false);
    // The chain itself, null until it is compiled.
    const LayerChain* readyChain(uint64_t slot, ChainKind kind, EffectList effects,
                                 const std::shared_ptr<const Composition>& document, bool text = false);
    Draw outgoingDraw(const LayerChain& chain, const VideoFrame* shown, const LayerState& from, uint64_t layer,
                      uint64_t parent);
    // Turns a layer's draw in a transition into a clipMix one; false while a chain it needs is not compiled.
    bool mixClipsFirst(Draw& draw, const Layer& layer, const LayerState& from, const std::vector<Effect>& layerChain,
                       const ClipRef* clip, const ClipRef* fromClip, const VideoFrame* shown, uint64_t parent,
                       const std::shared_ptr<const Composition>& document,
                       std::vector<std::shared_ptr<const CompiledChain>>& chains);
    void addCompositionChain(std::vector<Draw>& draws, const std::shared_ptr<const Composition>& document,
                             std::vector<std::shared_ptr<const CompiledChain>>& chains, float master);
    // `pictures` are the layers some slice reads.
    std::vector<Draw> collect(const std::vector<Layer>& layers, const Layer* group, bool solos,
                              const std::set<uint64_t>& pictures, const std::shared_ptr<const Composition>& document,
                              std::vector<std::shared_ptr<const CompiledChain>>& chains);
    void encodeStack(FrameEncoding& frame, const gpu::Texture* target, gpu::ClearColor clear,
                     std::vector<Draw>& draws);
    static bool timesPreview(const FrameEncoding& frame);
    gpu::RenderPass composite(FrameEncoding& frame, const gpu::Texture* target, gpu::RenderPass enc, const Draw& draw,
                              bool opaque);
    bool addsOnly(const Draw& draw, bool opaque) const;
    std::optional<gpu::Scissor> textScissor(const Draw& draw, bool opaque) const;
    void lastPassParams(const Draw& draw) const;
    bool keepsBlack(const Draw& draw) const;
    uint32_t blankStage(const Draw& draw, bool opaque) const;
    void encodeInputs(FrameEncoding& frame, Draw& draw);
    BakePlan planBake(const Draw& draw, float seconds);
    std::vector<uint32_t> bakeInputs(const Draw& draw, size_t prefix, float seconds) const;
    void encodePicture(FrameEncoding& frame, const Draw& draw);
    void encodeFade(FrameEncoding& frame, Draw& draw);
    void encodeReduced(FrameEncoding& frame, Draw& draw);
    // Moves the draws below the lowest one showing text or kept at output size into a stack at the render scale.
    void scaleDraws(std::vector<Draw>& draws, std::vector<std::shared_ptr<const CompiledChain>>& chains);
    bool upscaleReady();
    Size baseOf(const Draw& draw) const;
    Size sizeOf(const Draw& draw) const;
    TransitionMix scaledMix(const Draw& draw) const;
    // Picks the layers the frame budget guard draws at half resolution this frame.
    void guardBudget(std::vector<Draw>& draws, std::vector<std::shared_ptr<const CompiledChain>>& chains, bool enabled);
    static bool singlePass(const Draw& draw);
    PassHeader passHeader(const Draw& draw, float seconds, float opacity) const;
    void encodeLast(gpu::RenderPass& enc, const Draw& draw, BlendVariant variant, const gpu::Texture* below,
                    float seconds, float opacity) const;
    // A reduced draw's picture scaled up to the composition size.
    void encodeUpscaled(gpu::RenderPass& enc, const Draw& draw, BlendVariant variant,
                        const gpu::Texture* below,
                        float seconds, float opacity, Size into) const;
    void encodeSide(FrameEncoding& frame, const CompiledPass& pass, const Draw& draw);
    gpu::RenderPass beginPass(FrameEncoding& frame, const gpu::Texture* target, gpu::Load load,
                              gpu::ClearColor clear = {}) const;
    void endPass(FrameEncoding& frame, gpu::RenderPass enc) const;
    // While profiling, names the passes that follow: "L3", "L3 in", "L3 out", "G7" or "comp", then the pass's effects.
    void label(FrameEncoding& frame, const Draw& draw, const CompiledPass* pass, const char* what = nullptr) const;
    const gpu::Texture* copyBelow(FrameEncoding& frame, const gpu::Texture* target);
    void encodePass(gpu::RenderPass& enc, const CompiledPass& pass, size_t pipeline, const Draw& draw,
                    const gpu::Texture* input, const gpu::Texture* below, float seconds, float opacity,
                    const TransitionMix* mix = nullptr, const gpu::Texture* from = nullptr) const;
    // The pools are deques: a texture handed out this frame keeps its address while the pool grows.
    const gpu::Texture* poolTexture(std::deque<gpu::Texture>& pool, size_t index, gpu::Format format, Size size);
    void traceDone(FrameTrace::Render& mark, const gpu::Completion& done) const;
    void finished(std::shared_ptr<Frame> frame, bool ok);
    void modulate(const std::shared_ptr<ModulationPlan>& plan, double seconds, double beat);

    gpu::Device device_;
    EngineConfig config_;
    gpu::Queue queue_;
    profile::GpuTimeline gpu_;
    ChainCompiler compiler_;
    ClipDecoders decoders_;
    TextRenderer texts_;
    // Render thread only: what the text renderer was last asked to keep.
    std::set<TextLayout> textWants_;
    // Render thread only, by layer id.
    std::map<uint64_t, LayerState> layers_;
    // Render thread only: the Cues holding at this frame's beat.
    CueState cues_;
    // Render thread only: the beat clock at the frame being drawn, for effects that follow it.
    double frameBeat_ = 0.0;
    // Render thread only: seconds since the previous rendered frame, for effects that fade by elapsed time.
    double frameDt_ = 1.0 / 60.0;
    double lastFrameSeconds_ = -1.0;
    // Render thread only: what the decoders were last asked to keep.
    std::set<ClipKey> playing_;
    std::vector<Preload> preloads_;
    std::map<uint64_t, LayerChain> chains_;
    // Render thread only: two per layer with intermediate passes, 8-bit and 16-bit float; one per group; one copy of
    // the target for shader blend modes, which draw one after another.
    std::deque<gpu::Texture> intermediates_[2];
    std::deque<gpu::Texture> groups_;
    std::deque<gpu::Texture> below_;
    // Two per layer in a transition: its outgoing and incoming pictures.
    std::deque<gpu::Texture> fades_;
    // One per layer drawn at reduced resolution, and at full size its upscaled picture and the one EASU writes.
    std::deque<gpu::Texture> reduced_;
    std::deque<gpu::Texture> upscaled_;
    std::deque<gpu::Texture> easu_;
    std::optional<FsrUpscaler> fsr_;
    // Render thread only, by draw layer.
    std::map<uint64_t, Baked> bakes_;
    std::atomic<bool> bakeStatic_{false};
    std::atomic<int> renderScale_{100};
    std::atomic<double> frameBudget_{0.0};
    // GPU time of recent frames, averaged, in microseconds.
    std::atomic<uint64_t> recentGpu_{0};
    std::atomic<GuardProfile> guardProfile_{GuardProfile::Smooth};
    std::atomic<bool> protectTransitions_{false};
    // Render thread only.
    FrameGuard guard_;
    // The no-effects group chain, whose pass scales a reduced picture up into its stack.
    std::shared_ptr<const CompiledChain> upscale_;
    const EffectList noEffects_;
    SideTargets sides_;
    BeatClock clock_;
    bool starting_ = false;
    // The hold is a resume's: layers play from the first frame, but nothing is drawn until they all show their frame.
    bool resuming_ = false;
    std::optional<double> startHeldAt_;
    std::optional<double> startedAt_;
    // Guards only the queued clock actions and the published transport state.
    mutable std::mutex clockMutex_;
    std::vector<ClockAction> clockActions_;
    std::map<uint64_t, double> queuedScrubs_;
    std::optional<Resumed> resume_;
    std::optional<std::vector<ResumePilot>> adopt_;
    std::optional<BeatTimeline> followed_;
    bool followFromClock_ = false;
    bool unfollow_ = false;
    // Render thread only.
    std::optional<BeatFollower> follower_;
    // Render thread only: the scrubs this frame applies.
    std::map<uint64_t, double> scrubs_;
    TransportState transport_;
    std::shared_ptr<FrameRing> ring_;
    uint64_t nextIndex_ = 0;
    // Guards only the pointer swap and the preview pictures.
    mutable std::mutex compositionMutex_;
    std::shared_ptr<const Composition> composition_ = std::make_shared<const Composition>();
    std::set<uint64_t> previewPictures_;
    std::shared_ptr<FrameTap> tap_;
    std::shared_ptr<ModulationPlan> modulationShared_ = std::make_shared<ModulationPlan>(Composition{});
    // Render thread only: the plan of the frame being rendered and the parameter values of the pass being encoded.
    std::shared_ptr<ModulationPlan> modulation_;
    mutable std::vector<float> params_;
    mutable std::vector<float> fromParams_;
    mutable std::vector<float> rates_;
    // While a draw's last pass is encoded: its PassHeader::blank.
    uint32_t blankStage_ = 0;
    std::atomic<AudioAnalysis*> audio_{nullptr};
    std::atomic<uint32_t> seed_;
    // Render thread only: the seed the layers' Random Jump keys come from.
    uint32_t keyedSeed_ = 0;
    // Render thread only.
    AutopilotStepper autopilots_;
    EffectStepper effectSteps_;
    RenderStats stats_;
    std::unique_ptr<GpuProfile> profile_;
    std::unique_ptr<GpuProfile> previewTimer_;

    std::mutex inFlightMutex_;
    std::condition_variable inFlightDone_;
    int inFlight_ = 0;
    std::atomic<int> maxInFlight_{0};
};

} // namespace azl
