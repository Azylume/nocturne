#include "engine/engine.hpp"

#include "engine/transport.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace azl {

namespace {

// The columns a trigger can start next: the column triggered last and the one after it in the shown deck (wrapping), or
// the shown deck's first column when the last one is in another deck or none was triggered yet.
std::vector<uint64_t> nextColumns(const Composition& composition)
{
    std::vector<uint64_t> columns;
    if (composition.column)
        columns.push_back(composition.column);
    auto deck = std::find_if(composition.decks.begin(), composition.decks.end(),
                             [&](const Deck& d) { return d.id == composition.deck; });
    if (deck == composition.decks.end() || deck->columns.empty())
        return columns;
    const std::vector<Column>& shown = deck->columns;
    auto active = std::find_if(shown.begin(), shown.end(), [&](const Column& c) { return c.id == composition.column; });
    if (active == shown.end() || active + 1 == shown.end())
        columns.push_back(shown.front().id);
    else
        columns.push_back((active + 1)->id);
    return columns;
}

// The columns the next two column Cues to fire will start.
std::vector<uint64_t> cuedColumns(const Composition& composition, double beat)
{
    std::vector<uint64_t> columns;
    if (std::none_of(composition.cues.begin(), composition.cues.end(), [](const Cue& c) { return c.column.has_value(); }))
        return columns;
    for (const CueFiring& firing : upcomingCues(composition, beat)) {
        auto cue = std::find_if(composition.cues.begin(), composition.cues.end(),
                                [&](const Cue& c) { return c.id == firing.cue; });
        if (cue == composition.cues.end() || !cue->column)
            continue;
        if (const uint64_t column = cueColumn(composition, *cue->column))
            columns.push_back(column);
        if (columns.size() == 2)
            break;
    }
    return columns;
}

uint64_t microsSince(std::chrono::steady_clock::time_point t)
{
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now() - t).count());
}

// What a layer's state plays: the media's hash, or for a text or generator clip, which have none, a name no hash can
// take.
const std::string kTextSource = "text";
const std::string kGeneratorSource = "generator";

const std::string& sourceOf(const ClipRef& clip)
{
    return clip.text ? kTextSource : isGeneratorClip(clip) ? kGeneratorSource : clip.hash;
}

bool hasMedia(const std::string& source)
{
    return !source.empty() && source != kTextSource && source != kGeneratorSource;
}

// Every generator clip shows this picture; its chain draws the rest.
const VideoFrameRef& generatorPicture()
{
    static const VideoFrameRef picture = std::make_shared<const VideoFrame>(VideoFrame{.generator = true});
    return picture;
}

const ClipRef* clipAt(const Layer& layer, const std::optional<Play>& play)
{
    if (!play)
        return nullptr;
    auto it = layer.clips.find(play->column);
    return it == layer.clips.end() ? nullptr : &it->second;
}

// The layers and groups whose pictures some slice takes as input.
std::set<uint64_t> slicedPictures(const Composition& composition)
{
    std::set<uint64_t> pictures;
    for (const Screen& screen : composition.screens)
        for (const Slice& slice : screen.slices)
            if (slice.input != 0)
                pictures.insert(slice.input);
    return pictures;
}

uint64_t effectId(const EffectList& effects, const PlannedStage& stage)
{
    return effects[stage.chainIndex]->id;
}

// The passes of a chain that draw `picture`: the test card without one, else its video or text source.
const std::vector<CompiledPass>* passesOf(const CompiledChain& chain, const VideoFrame* picture)
{
    return !picture ? &chain.testCard : picture->text ? &chain.text : picture->generator ? &chain.generator : &chain.yuv;
}

// The deck of the column a layer's state plays, or 0.
template <typename State>
uint64_t playedDeck(const Composition& composition, const State& state)
{
    return state.play ? columnDeck(composition, state.play->column) : 0;
}

// The clip a layer's state plays, while the cell still holds it.
template <typename State>
const ClipRef* clipOf(const Layer& layer, const State& state)
{
    const ClipRef* clip = clipAt(layer, state.play);
    return clip && sourceOf(*clip) == state.hash ? clip : nullptr;
}

// Where a preload of the clip starts: its in point forward, its out point backward.
Preload preloadOf(const ClipKey& key, const Transport& transport)
{
    const bool forward = !playsBackward(transport.playMode, 0.0);
    return {key, forward ? transport.in : transport.out, forward};
}

bool anySolo(const std::vector<Layer>& layers)
{
    return std::any_of(layers.begin(), layers.end(), [](const Layer& layer) { return layer.solo; });
}

bool anySolo(const Composition& composition)
{
    bool solo = false;
    forEachLayer(composition.layers, [&](const Layer& layer) { solo = solo || layer.solo; });
    return solo;
}

// Bypass hides a layer or a whole group. Solo applies to the whole composition: once anything is soloed, a layer
// shows only when it is soloed itself or sits in a soloed group that has no soloed layers of its own, and a group
// shows when it is soloed or holds a soloed layer.
bool shown(const Layer& layer, float opacity, const Layer* group, bool solos)
{
    if (layer.bypass || opacity <= 0.0f)
        return false;
    if (!solos || layer.solo)
        return true;
    if (group)
        return group->solo && !anySolo(group->layers);
    return layer.group && anySolo(layer.layers);
}

// Side targets of a layer's outgoing clip during a transition, apart from its incoming clip's.
uint64_t fadeId(uint64_t layer)
{
    return layer | uint64_t(1) << 63;
}

// Chain slots of a layer in a transition that mixes its clips first: its clips' own chains, and its chain on them.
uint64_t clipSlot(uint64_t layer)
{
    return layer | uint64_t(1) << 62;
}

uint64_t mixSlot(uint64_t layer)
{
    return layer | uint64_t(1) << 61;
}

TransitionMix mixOf(TransitionMode type, double progress, uint32_t width,
                    const std::array<float, kMaxTransitionParams>& params)
{
    const double t = std::clamp(progress, 0.0, 1.0);
    const auto edge = uint32_t(std::lround(t * width));
    TransitionMix mix{uint32_t(type), uint32_t(std::lround(t * 256.0)), type == TransitionMode::Push ? width - edge : edge,
                      0, {}};
    std::copy(params.begin(), params.end(), mix.params);
    return mix;
}

std::array<float, kMaxTransitionParams> transitionParams(const Layer& layer, TransitionMode mode)
{
    std::array<float, kMaxTransitionParams> out{};
    const auto transitions = isfTransitions();
    const IsfTransition* transition = findIsfTransition(*transitions, mode);
    if (!transition)
        return out;
    auto values = layer.transitionParams.find(transitionName(mode));
    const std::vector<float> params =
        transitionParamValues(*transition, values == layer.transitionParams.end() ? std::map<std::string, double>{}
                                                                                 : values->second);
    std::copy_n(params.begin(), std::min(params.size(), out.size()), out.begin());
    return out;
}

// The clip a transition starts from: the one playing, or while that has shown no frame yet, the one it was
// transitioning from.
template <typename State>
std::unique_ptr<State> outgoing(State& state)
{
    if (state.shown && !state.hash.empty()) {
        state.from.reset();
        return std::make_unique<State>(std::move(state));
    }
    return std::move(state.from);
}

} // namespace

Engine::Engine(const gpu::Device& gpuDevice, EngineConfig config, MediaLibrary* library)
    : device_(gpuDevice), config_(config), queue_(gpuDevice.makeQueue()),
      gpu_(device(), "render", ""),
      compiler_(gpuDevice, config.pipelineCache ? config.pipelineCache : ""),
      decoders_(device(), library, config.clipCacheBytes, config.preloadBytes, config.decodeFrameCost),
      texts_(device(), config.height),
      sides_(gpuDevice, config.width, config.height),
      ring_(std::make_shared<FrameRing>(gpuDevice, config.width, config.height, config.frameCount)),
      autopilots_(clock_, cues_, seed_, [this](const Layer& layer, uint64_t column) { return playable(layer, column); }),
      effectSteps_(clock_, cues_, seed_)
{
    seed_ = config.seed;
    maxInFlight_ = config.maxInFlight;
    bakeStatic_ = config.bakeStatic;
    keyedSeed_ = config.seed;
    starting_ = config.holdStart;
    transport_.starting = config.holdStart;
    poolTexture(fades_, 1, kFrameFormat, {config.width, config.height});
    poolTexture(fades_, 0, kFrameFormat, {config.width, config.height});
    if (config.gpuProfile) {
        profile_ = std::make_unique<GpuProfile>(gpuDevice, profile::kEnabled ? 2 : 0);
        std::fprintf(stderr, "nocturne: GPU profile: %s\n", profile_->mode());
        if (!profile_->enabled())
            profile_.reset();
    }
    previewTimer_ = std::make_unique<GpuProfile>(gpuDevice, 1, true);
    if (!previewTimer_->enabled())
        previewTimer_.reset();
    if (config_.fsrUpscale)
        fsr_.emplace(gpuDevice, kFrameFormat);
}

Engine::~Engine()
{
    waitUntilIdle();
}

// Each layer's last pass blends straight into the composition frame; a group's layers blend into the group's
// sub-stack, which then blends into the frame as one layer. Everything a stack reads is encoded before it, so the
// frame is drawn by one encoder, split only where a shader blend mode copies what is below it.
void Engine::renderFrame(double seconds)
{
    AZL_ZONE("render frame");
    gpu::Scope scope;
    if (const int bound = maxInFlight_; bound > 0) {
        // Waits for a frame to leave the GPU rather than skipping this one: the GPU goes straight on to the next
        // frame, while only `bound` are ever queued ahead of the outputs' presents. A GPU stuck for a frame's time is
        // the ring's problem, not this wait's.
        std::unique_lock lock(inFlightMutex_);
        if (inFlight_ >= bound) {
            stats_.throttled++;
            if (!inFlightDone_.wait_for(lock, std::chrono::duration<double>(frameDt_ > 0.0 ? frameDt_ : 1.0 / 60.0),
                                        [&] { return inFlight_ < bound; })) {
                // Still that many on the GPU after a frame's time: this frame is skipped, never queued over the bound.
                stats_.skipped++;
                return;
            }
        }
    }
    std::shared_ptr<Frame> frame = ring_->claim();
    if (!frame) {
        stats_.skipped++;
        return;
    }
    frameDt_ = lastFrameSeconds_ < 0.0 ? 1.0 / 60.0 : std::clamp(seconds - lastFrameSeconds_, 0.0, 0.25);
    lastFrameSeconds_ = seconds;
    frame->index = nextIndex_++;
    frame->started = std::chrono::steady_clock::now();
    FrameTrace::Render mark{.index = frame->index, .started = traceSeconds(frame->started)};

    // Clips keep their time while their layer is hidden.
    std::shared_ptr<const Composition> current;
    std::shared_ptr<ModulationPlan> modulation;
    std::shared_ptr<FrameTap> tap;
    {
        std::lock_guard lock(compositionMutex_);
        current = composition_;
        modulation = modulationShared_;
        tap = tap_;
    }
    const double beat = advanceClock(*current, seconds);
    frameBeat_ = beat;
    const bool settled = compiler_.idle() && (!modulation->usesAudio() || !audio_.load() ||
                                              (startHeldAt_ && seconds - *startHeldAt_ >= kAudioSettle));
    const bool starting = !resuming_ && holding(seconds, startHeldAt_ && decoders_.settled() && settled);
    rekey();
    cueStateAt(*current, beat, cues_);
    modulation->cue(cues_.macros);
    effectSteps_.step(*current, beat);
    modulation->step(effectSteps_.positions());
    modulate(modulation, seconds, beat);
    autopilots_.fireColumnCues(*current, beat);
    if (!starting)
        autopilots_.stepColumns(*current, beat);
    std::set<uint64_t> ids;
    forEachLayer(current->layers, [&](const Layer& layer) {
        ids.insert(layer.id);
        if (layer.group || starting)
            return;
        switchIfDue(layer, *current, autopilots_.steer(layer, *current, beat), seconds, beat);
        endFade(layers_[layer.id], beat);
    });
    mark.stepped = traceNow();
    std::erase_if(layers_, [&](const auto& state) { return !ids.contains(state.first); });
    autopilots_.keepLayers(ids);
    updateWants(*current, beat);
    bool onTime = true;
    forEachLayer(current->layers, [&](const Layer& layer) {
        if (layer.group)
            return;
        LayerState& state = layers_[layer.id];
        auto scrub = scrubs_.find(layer.id);
        onTime &= updateLayer(layer, state, seconds, beat,
                              scrub == scrubs_.end() ? std::nullopt : std::optional<double>(scrub->second));
        if (state.from && state.from->hash != state.hash)
            onTime &= updateLayer(layer, *state.from, seconds, beat, std::nullopt);
    });
    mark.updated = traceNow();
    const bool held = starting || (resuming_ && holding(seconds, onTime && decoders_.preloaded() && settled));
    std::set<uint64_t> sided = ids;
    sided.insert(0);
    for (const auto& [id, state] : layers_)
        if (state.from)
            sided.insert({fadeId(id), clipSlot(id), fadeId(clipSlot(id)), mixSlot(id)});
    std::erase_if(chains_, [&](const auto& state) { return !sided.contains(state.first); });
    std::set<uint64_t> effects;
    forEachChain(*current, [&](const std::vector<Effect>& chain, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : chain)
            effects.insert(effect.id);
    });
    sides_.keep(sided, effects);
    std::erase_if(bakes_, [&](const auto& bake) { return !bakeStatic_ || !sided.contains(bake.first); });

    // The GPU reads these frames' textures and these pipelines until the command buffer completes.
    std::vector<VideoFrameRef> pictures;
    for (const auto& [id, state] : layers_) {
        if (state.shown)
            pictures.push_back(state.shown);
        if (state.from && state.from->shown)
            pictures.push_back(state.from->shown);
    }
    std::vector<std::shared_ptr<const CompiledChain>> chains;
    std::set<uint64_t> published = slicedPictures(*current);
    std::set<uint64_t> previewOnly;
    {
        std::lock_guard lock(compositionMutex_);
        previewOnly = previewPictures_;
    }
    std::erase_if(previewOnly, [&](uint64_t id) { return published.contains(id); });
    published.insert(previewOnly.begin(), previewOnly.end());
    std::vector<Draw> draws;
    if (!held) {
        draws = collect(current->layers, nullptr, anySolo(*current), published, current, chains);
        addCompositionChain(draws, current, chains,
                            std::clamp(modulation_->opacity(0, float(current->opacity)), 0.0f, 1.0f));
        guardBudget(draws, chains, current->frameGuard);
        scaleDraws(draws, chains);
    }
    publishTransport(*current, seconds, beat, held);

    FrameEncoding encoding{queue_.commandBuffer(), *frame, float(seconds)};
    if (profile_)
        encoding.profile = profile_->begin();
    if (previewTimer_ && !previewOnly.empty())
        encoding.previewTimes = previewTimer_->begin();
    encoding.previewOnly = std::move(previewOnly);
    encodeStack(encoding, &frame->texture, gpu::ClearColor{0, 0, 0, 1}, draws);
    stats_.passes = encoding.passes;
    stats_.previewPasses = encoding.previewPasses;
    stats_.bakedPasses = encoding.bakedPasses;
    stats_.bakes = bakes_.size();
    stats_.intermediates = encoding.fullResolutionTargets;
    stats_.layerStates = layers_.size();
    stats_.fading = std::count_if(layers_.begin(), layers_.end(), [](const auto& s) { return s.second.from != nullptr; });
    stats_.chains = chains_.size();
    stats_.sideTargets = sides_.count();
    stats_.sideSpares = sides_.spares();
    stats_.pooledTextures = intermediates_[0].size() + intermediates_[1].size() + groups_.size() + below_.size() +
                            fades_.size();
    FrameTap::Done tapped = tap ? tap->encode(encoding.cmd.native(), *frame, *current, seconds) : nullptr;
    mark.encoded = traceNow();
    mark.fading = stats_.fading;

    {
        std::lock_guard lock(inFlightMutex_);
        inFlight_++;
    }
    encoding.cmd.onCompleted([this, frame, pictures = std::move(pictures),
                              chains = std::move(chains),
                              tapped = std::move(tapped),
                              profile = encoding.profile,
                              previewTimes = encoding.previewTimes,
                              mark, timeline = gpu_](const gpu::Completion& done) mutable {
        AZL_GPU_COLLECT(timeline);
        pictures.clear();
        chains.clear();
        if (tapped)
            tapped(done.ok);
        const auto gpu = uint64_t(std::max(0.0, done.gpuEnd - done.gpuStart) * 1e6);
        stats_.gpuMicros += gpu;
        recentGpu_ = (recentGpu_ * 7 + gpu) / 8;
        stats_.maxGpuMicros = std::max(stats_.maxGpuMicros.load(), gpu);
        stats_.maxDoneMicros = std::max(stats_.maxDoneMicros.load(), microsSince(frame->started));
        const auto queued =
            uint64_t(std::max(0.0, traceFromHost(done.gpuStart) - traceSeconds(frame->started)) * 1e6);
        stats_.queueMicros += queued;
        stats_.maxQueueMicros = std::max(stats_.maxQueueMicros.load(), queued);
        if (profile)
            profile_->finish(*profile, done);
        if (previewTimes)
            stats_.previewGpuMicros += previewTimer_->finish(*previewTimes, done);
        traceDone(mark, done);
        finished(std::move(frame), done.ok);
    });
    encoding.cmd.commit();
    AZL_FRAME_MARK;
    stats_.maxCpuMicros = std::max(stats_.maxCpuMicros.load(), microsSince(frame->started));
}

// A slice's picture of a layer ignores the layer's opacity, blend mode and solo, but not its bypass. An output-only
// group never reaches the composition.
std::vector<Engine::Draw> Engine::collect(const std::vector<Layer>& layers, const Layer* group, bool solos,
                                          const std::set<uint64_t>& pictures,
                                          const std::shared_ptr<const Composition>& document,
                                          std::vector<std::shared_ptr<const CompiledChain>>& chains)
{
    AZL_ZONE("collect draws");
    std::vector<Draw> draws;
    for (const Layer& layer : layers) {
        const float opacity = modulation_->opacity(layer.id, float(layer.opacity));
        const bool composite = shown(layer, opacity, group, solos) && !layer.outputOnly;
        const bool publish = pictures.contains(layer.id) && !layer.bypass;
        if (!composite && !publish)
            continue;
        const LayerState& state = layers_[layer.id];
        const ClipRef* clip = layer.group ? nullptr : clipOf(layer, state);
        const uint64_t deck = layer.group ? 0 : playedDeck(*document, state);
        const std::vector<Effect>& layerChain = deckEffects(layer, deck);
        const LayerChain* chain = layer.group
                                      ? pickChain(layer.id, ChainKind::Group, effectList(layer.effects), document)
                                      : pickChain(layer.id, chainKind(clip), clipChain(layerChain, clip), document,
                                                  holdsText(layer));
        if (!chain || !state.visible)
            continue;
        // A layer playing no clip shows nothing; only one without any clip shows the test card.
        if (!layer.group && state.hash.empty() && !layer.clips.empty())
            continue;
        const bool fromShown = !state.shown && state.from;
        const VideoFrame* picture = fromShown ? state.from->shown.get() : state.shown.get();
        const CompiledChain& compiled = *chain->chain;
        Draw draw{layer.group ? &compiled.group : passesOf(compiled, picture), &chain->effects, picture, opacity,
                  deckBlend(layer, deck), layer.id, {}};
        // A chain compiled before the layer held a text clip has no text passes yet.
        if (draw.passes->empty())
            continue;
        if (picture && picture->text)
            draw.text = textUniforms(layer.id, fromShown ? *state.from : state);
        draw.composite = composite;
        draw.publish = publish ? layer.id : 0;
        draw.fullResolution = layer.fullResolution;
        const uint64_t parent = group ? group->id : 0;
        sides_.use(layer.id, state.hash, parent, layer.group ? layer.effects : layerChain);
        if (layer.group) {
            draw.layers = collect(layer.layers, &layer, composite && solos, pictures, document, chains);
            if (draw.layers.empty())
                continue;
        }
        if (state.from && state.shown) {
            const LayerState& from = *state.from;
            const ClipRef* fromClip = clipOf(layer, from);
            const VideoFrame* shown = from.visible ? from.shown.get() : nullptr;
            const uint64_t fromDeck = playedDeck(*document, from);
            const std::vector<Effect>& fromLayerChain = deckEffects(layer, fromDeck);
            draw.mix = mixOf(state.fade.mode, state.fadeProgress, config_.width, state.fadeParams);
            if (const BlendMode fromBlend = deckBlend(layer, fromDeck); fromBlend != draw.blend)
                draw.fromBlend = fromBlend;
            const bool oneChain = &fromLayerChain == &layerChain && !draw.fromBlend;
            if (oneChain && state.fade.mode == TransitionMode::Alpha && !compiled.yuvFade.empty() &&
                chainKind(clip) == ChainKind::Layer && chainKind(fromClip) == ChainKind::Layer && picture &&
                !picture->text && shown && !shown->text) {
                draw.passes = &compiled.yuvFade;
                draw.fadeFrom = shown;
                chains.push_back(chain->chain);
                draws.push_back(std::move(draw));
                continue;
            }
            if (oneChain && !layerChain.empty() &&
                mixClipsFirst(draw, layer, from, layerChain, clip, fromClip, shown, parent, document, chains)) {
                chains.push_back(chain->chain);
                draws.push_back(std::move(draw));
                continue;
            }
            const LayerChain* own = pickChain(fadeId(layer.id), chainKind(fromClip), clipChain(fromLayerChain, fromClip),
                                              document, holdsText(layer));
            const LayerChain* outgoing = own && !passesOf(*own->chain, shown)->empty() ? own : chain;
            chains.push_back(outgoing->chain);
            draw.from.push_back(outgoingDraw(*outgoing, shown, from, layer.id, parent));
        }
        chains.push_back(chain->chain);
        draws.push_back(std::move(draw));
    }
    return draws;
}

// The outgoing clip of a layer's transition through `chain` at full opacity, only into the transition's mix.
Engine::Draw Engine::outgoingDraw(const LayerChain& chain, const VideoFrame* shown, const LayerState& from,
                                  uint64_t layer, uint64_t parent)
{
    Draw out{passesOf(*chain.chain, shown), &chain.effects, shown, 1.0f, BlendMode::Normal, fadeId(layer), {}};
    if (out.passes->empty())
        out.picture = nullptr;
    if (out.picture && out.picture->text)
        out.text = textUniforms(layer, from);
    out.composite = false;
    if (out.picture)
        sides_.use(out.layer, from.hash, parent);
    return out;
}

// The layer's own chain runs once on its clips mixed first (Resolume's order): each clip through its own clip chain,
// the incoming one with the layer's id so its state runs on after the transition, mixed into a sub-stack.
bool Engine::mixClipsFirst(Draw& draw, const Layer& layer, const LayerState& from,
                           const std::vector<Effect>& layerChain, const ClipRef* clip, const ClipRef* fromClip,
                           const VideoFrame* shown, uint64_t parent, const std::shared_ptr<const Composition>& document,
                           std::vector<std::shared_ptr<const CompiledChain>>& chains)
{
    static const std::vector<Effect> none;
    const bool text = holdsText(layer);
    const LayerChain* own = readyChain(mixSlot(layer.id), ChainKind::Group, effectList(layerChain), document);
    const LayerChain* in = readyChain(clipSlot(layer.id), chainKind(clip), clipChain(none, clip), document, text);
    const LayerChain* out =
        readyChain(fadeId(clipSlot(layer.id)), chainKind(fromClip), clipChain(none, fromClip), document, text);
    if (!own || !in || !out || own->chain->group.empty() || passesOf(*in->chain, draw.picture)->empty())
        return false;
    Draw clips{passesOf(*in->chain, draw.picture), &in->effects, draw.picture, 1.0f, BlendMode::Normal, layer.id, {}};
    clips.text = draw.text;
    clips.mix = draw.mix;
    clips.from.push_back(outgoingDraw(*out, shown, from, layer.id, parent));
    draw.layers.push_back(std::move(clips));
    draw.passes = &own->chain->group;
    draw.effects = &own->effects;
    draw.picture = nullptr;
    draw.mix = {};
    draw.clipMix = true;
    chains.insert(chains.end(), {own->chain, in->chain, out->chain});
    return true;
}

// At start-up the first frame only tells the decoders what the composition plays (its cells are preloaded at their
// starts, since no layer plays yet). A resume's layers play from its first frame, which asks the decoders for their
// frames there. Either hold ends at the first frame that finds everything ready.
bool Engine::holding(double seconds, bool ready)
{
    if (!starting_)
        return false;
    if (!startHeldAt_)
        startHeldAt_ = seconds;
    if (ready || seconds - *startHeldAt_ >= kMaxStartHold)
        starting_ = resuming_ = false;
    decoders_.hurry(starting_);
    stats_.starting = starting_;
    return starting_;
}

// Live actions happened before this frame, each at its own time; a tempo set in the document lands at the frame.
double Engine::advanceClock(const Composition& composition, double seconds)
{
    std::vector<ClockAction> actions;
    std::optional<Resumed> resumed;
    std::optional<std::vector<ResumePilot>> adopted;
    std::optional<BeatTimeline> followed;
    bool fromClock = false;
    scrubs_.clear();
    {
        std::lock_guard lock(clockMutex_);
        actions.swap(clockActions_);
        scrubs_.swap(queuedScrubs_);
        resumed.swap(resume_);
        adopted.swap(adopt_);
        followed.swap(followed_);
        fromClock = followFromClock_;
        if (unfollow_)
            follower_.reset();
        unfollow_ = false;
    }
    if (adopted)
        autopilots_.restore(composition, *adopted);
    bool resynced = false;
    if (followed) {
        if (!follower_) {
            follower_.emplace();
            if (fromClock) {
                BeatTimeline own = clock_.timeline();
                own.resyncs = followed->resyncs;
                follower_->target(own, seconds);
            }
        }
        resynced = follower_->target(*followed, seconds);
    }
    if (follower_) {
        const BeatTimeline& timeline = follower_->timeline();
        clock_.follow(follower_->beatAt(seconds), seconds, timeline.bpm, timeline.nudge, resynced);
        return clock_.beatAt(seconds);
    }
    for (const ClockAction& action : actions) {
        const double at = std::min(action.seconds, seconds);
        if (action.nudge)
            clock_.setNudge(*action.nudge, at);
        else
            clock_.resync(at);
    }
    if (composition.bpm != clock_.bpm())
        clock_.setBpm(composition.bpm, seconds);
    if (resumed)
        applyResume(composition, *resumed);
    return clock_.beatAt(seconds);
}

// A resumed show's clips start mid-way, so its hold waits for the frames the layers play rather than their starts, and
// for the preloads, which would otherwise take the decoders from the layers in the first seconds.
void Engine::applyResume(const Composition& composition, const Resumed& resumed)
{
    starting_ = resuming_ = config_.holdStart;
    startHeldAt_.reset();
    clock_.restore(resumed.beat, resumed.seconds);
    cueStateAt(composition, resumed.beat, cues_);
    autopilots_.restore(composition, resumed.pilots);
}

bool Engine::playable(const Layer& layer, uint64_t column)
{
    auto it = layer.clips.find(column);
    return it != layer.clips.end() && (!hasMedia(sourceOf(it->second)) || !decoders_.failed({layer.id, it->second.hash}));
}

// A trigger takes over on the first frame at or after its beat, anchored on that beat; without a beat, or once a
// resync has moved the beats under it, at once. A new clip in the cell being played starts at once too.
void Engine::switchIfDue(const Layer& layer, const Composition& composition, const std::optional<Play>& playing,
                         double seconds, double beat)
{
    const Transition transition = deckTransition(layer, playing ? columnDeck(composition, playing->column) : 0);
    LayerState& state = layers_[layer.id];
    const ClipRef* clip = clipAt(layer, playing);
    if (playing == state.play && (clip ? sourceOf(*clip) : "") == state.hash)
        return;
    if (playing == state.play)
        return start(state, layer, transition, playing, seconds, beat, std::nullopt);
    if (playing != state.pending) {
        state.pending = playing;
        state.pendingResyncs = clock_.resyncs();
    }
    std::optional<double> at = playing ? playing->beat : std::nullopt;
    if (state.pendingResyncs != clock_.resyncs())
        at.reset();
    if (at && beat < *at - kBeatEpsilon)
        return;
    start(state, layer, transition, playing, seconds, beat, at);
}

// The layer keeps its picture until the new clip delivers a frame, so it never blinks black; an empty cell clears it.
//
// With a transition, the clip playing moves aside and plays on while the new one mixes in over the transition's beats
// from the trigger's beat, so it follows the tempo.
void Engine::start(LayerState& state, const Layer& layer, const Transition& transition,
                   const std::optional<Play>& playing, double seconds, double beat, std::optional<double> at)
{
    const ClipRef* clip = clipAt(layer, playing);
    const double beats = clip ? clip->transition.value_or(transition.beats) : 0.0;
    std::unique_ptr<LayerState> fading = beats > 0.0 && beats <= kMaxTransitionBeats ? outgoing(state) : nullptr;
    VideoFrameRef shown = std::move(state.shown);
    TextClip look = std::move(state.look);
    if (!shown && state.from) {
        shown = std::move(state.from->shown);
        look = std::move(state.from->look);
    }
    state = {};
    state.look = std::move(look);
    state.play = state.pending = playing;
    if (!clip)
        return;
    const double from = at.value_or(beat);
    state.hash = sourceOf(*clip);
    state.transport = clip->transport;
    state.anchorBeat = from;
    state.anchorTime = seconds - (beat - from) * 60.0 / clock_.tempo();
    state.resyncs = clock_.resyncs();
    state.delivered = shown || fading;
    state.shown = std::move(shown);
    state.key = layerKey(seed_, Choice::Jump, layer.id);
    if (!fading)
        return;
    state.from = std::move(fading);
    state.fade = {transition.mode, beats};
    state.fadeParams = transitionParams(layer, transition.mode);
    state.fadeStart = from;
    state.fadeResyncs = clock_.resyncs();
}

// A transition ends once its beats have passed and the incoming clip shows; a resync ends it at once.
void Engine::endFade(LayerState& state, double beat)
{
    if (!state.from)
        return;
    state.fadeProgress = (beat - state.fadeStart) / state.fade.beats;
    const bool over = state.fadeProgress >= 1.0 - kBeatEpsilon || state.fadeResyncs != clock_.resyncs();
    if (over && state.shown)
        state.from.reset();
}

// The decoders keep what plays open and, most urgent first, what a trigger can start next: triggers waiting for their
// beat, Autopilot's next cells, the active and next column, armed cells, then the first column of every deck, where a
// deck change usually starts. Each preload starts where its play mode starts.
void Engine::updateWants(const Composition& composition, double beat)
{
    std::set<ClipKey> playing;
    for (const auto& [id, state] : layers_) {
        if (hasMedia(state.hash))
            playing.insert({id, state.hash});
        if (state.from && hasMedia(state.from->hash))
            playing.insert({id, state.from->hash});
    }
    std::set<TextLayout> texts;
    auto shownText = [&](const Layer& layer, const LayerState* state) {
        if (const ClipRef* clip = state ? clipAt(layer, state->play) : nullptr; clip && clip->text)
            texts.insert(layoutOf(*clip->text));
    };
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (auto it = layers_.find(layer.id); it != layers_.end()) {
            shownText(layer, &it->second);
            shownText(layer, it->second.from.get());
        }
    });
    std::vector<Preload> preloads;
    auto preload = [&](const Layer& layer, uint64_t column) {
        auto it = layer.clips.find(column);
        if (it == layer.clips.end())
            return;
        if (it->second.text) {
            texts.insert(layoutOf(*it->second.text));
            return;
        }
        if (isGeneratorClip(it->second))
            return;
        const ClipKey key{layer.id, it->second.hash};
        auto same = [&](const Preload& p) { return p.clip == key; };
        const bool listed = std::any_of(preloads.begin(), preloads.end(), same);
        if (playing.contains(key) || listed)
            return;
        preloads.push_back(preloadOf(key, it->second.transport));
    };
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (const std::optional<Play> play = autopilots_.playOf(layer))
            preload(layer, play->column);
    });
    if (const uint64_t next = autopilots_.columnNext())
        forEachLayer(composition.layers, [&](const Layer& layer) { preload(layer, next); });
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (const uint64_t next = autopilots_.layerNext(layer, composition))
            preload(layer, next);
    });
    for (const uint64_t column : cuedColumns(composition, beat))
        forEachLayer(composition.layers, [&](const Layer& layer) { preload(layer, column); });
    for (const uint64_t column : nextColumns(composition))
        forEachLayer(composition.layers, [&](const Layer& layer) { preload(layer, column); });
    forEachLayer(composition.layers, [&](const Layer& layer) {
        for (const uint64_t column : layer.armed)
            preload(layer, column);
    });
    for (const Deck& deck : composition.decks)
        if (!deck.columns.empty())
            forEachLayer(composition.layers, [&](const Layer& layer) { preload(layer, deck.columns.front().id); });
    if (texts != textWants_) {
        textWants_ = texts;
        texts_.want(std::move(texts));
    }
    if (playing == playing_ && preloads == preloads_)
        return;
    playing_ = playing;
    preloads_ = preloads;
    decoders_.want(std::move(playing), std::move(preloads));
}

// A clip that cannot be played holds the layer's picture without counting misses, as does a layer during the hold.
void Engine::countMiss(const LayerState& state, const ClipKey& key)
{
    if (state.delivered && !starting_ && !decoders_.failed(key))
        addMisses(key.first, 1);
}

void Engine::addMisses(uint64_t layer, uint64_t count)
{
    if (!count)
        return;
    stats_.videoMisses += count;
    std::lock_guard lock(stats_.missesMutex);
    stats_.missesByLayer[layer] += count;
}

// The clip shown may be one a waiting trigger is about to replace, so its transport is read from its own cell. True
// when the layer shows its frame for this time, nothing, or a clip that failed.
bool Engine::updateLayer(const Layer& layer, LayerState& state, double seconds, double beat,
                         std::optional<double> scrub)
{
    AZL_ZONE("update layer");
    if (state.hash.empty())
        return true;
    if (state.hash == kTextSource)
        return updateText(layer, state);
    if (state.hash == kGeneratorSource) {
        state.visible = true;
        state.delivered = true;
        state.shown = generatorPicture();
        return true;
    }
    const ClipKey key{layer.id, state.hash};
    const std::optional<ClipLength> length = decoders_.length(key, config_.waitForMedia);
    if (!length) {
        countMiss(state, key);
        return decoders_.failed(key);
    }
    const auto cell = state.play ? layer.clips.find(state.play->column) : layer.clips.end();
    const bool same = cell != layer.clips.end() && cell->second.hash == state.hash;
    state.retime(same ? cell->second.transport : state.transport, *length, seconds, beat, clock_.resyncs());
    if (scrub)
        state.scrub(*scrub, seconds, beat);
    std::vector<FrameTarget> targets;
    const FramePick shown = state.pick(seconds, beat, targets);
    // Autopilot restarting the clip needs its first frame, which BPM-synced clips prefetch anyway.
    const std::optional<uint64_t> next = autopilots_.pickedNext(layer.id);
    if (next && *next == state.play->column && state.transport.sync == SyncMode::Timeline) {
        const Region region = regionOf(state.transport, state.length.frames);
        const FramePick first = inRegion(pickFrame(state.transport.playMode, 0.0, region.frames), region);
        targets.push_back({first.index, first.forward});
    }
    state.frame = shown.index;
    state.visible = shown.visible;
    if (!shown.visible)
        return true;
    VideoFrameRef frame = decoders_.frame(key, shown.index, shown.forward, targets, false);
    if (!frame)
        countMiss(state, key);
    if (!frame && config_.waitForMedia)
        frame = decoders_.frame(key, shown.index, shown.forward, targets, true);
    if (!frame)
        return decoders_.failed(key);
    state.delivered = true;
    state.shown = std::move(frame);
    return true;
}

// A text clip shows its last picture until the one for its current layout is rasterised.
bool Engine::updateText(const Layer& layer, LayerState& state)
{
    const ClipRef* clip = clipAt(layer, state.play);
    if (!clip || !clip->text)
        return true;
    VideoFrameRef frame = texts_.frame(layoutOf(*clip->text), config_.waitForMedia);
    if (!frame)
        return false;
    if (state.look != *clip->text)
        state.look = *clip->text;
    state.delivered = true;
    state.shown = std::move(frame);
    return true;
}

// The fill colour, the outline colour and the placement of the state's text picture, with the text parameters
// modulated. The picture is scaled from the size it was rasterised at. A text that stands still is placed on whole
// pixels, so it stays crisp; a moving or scaling one on fractions of a pixel, so it glides instead of jumping.
std::array<float, 12> Engine::textUniforms(uint64_t layer, const LayerState& state) const
{
    const TextClip& look = state.look;
    const VideoFrame::Text& text = *state.shown->text;
    auto param = [&](TextParam p, double base) { return modulation_->text(layer, p, float(base)); };
    auto colour = [&](const char* group, const std::array<double, 4>& own) {
        auto link = look.links.find(group);
        const auto linked = link == look.links.end() ? std::nullopt : modulation_->linkedColour(link->second);
        return linked ? std::array{linked->r, linked->g, linked->b, linked->a} : own;
    };
    const std::array<double, 4> fill = colour(kTextColour, look.color), o = colour(kTextOutline, look.outlineColor);
    const float scale = param(TextParam::Size, look.size) / text.size;
    const float left = param(TextParam::X, look.x), top = param(TextParam::Y, look.y);
    const bool still = scale == 1.0f && left == float(look.x) && top == float(look.y);
    const auto place = [&](float at) { return still ? std::round(at) : at; };
    const float x = place(left * float(config_.width) + text.bounds[0] * scale);
    const float y = place(top * float(config_.height) + text.bounds[1] * scale);
    return {param(TextParam::Red, fill[0]), param(TextParam::Green, fill[1]), param(TextParam::Blue, fill[2]),
            param(TextParam::Alpha, fill[3]),
            float(o[0]), float(o[1]), float(o[2]), float(o[3]),
            x,           y,           1.0f / scale, 0.0f};
}

void Engine::publishTransport(const Composition& composition, double seconds, double beat, bool held)
{
    if (!held && !startedAt_)
        startedAt_ = seconds;
    std::lock_guard lock(clockMutex_);
    transport_.starting = held;
    transport_.startedAt = startedAt_.value_or(0.0);
    transport_.time = seconds;
    transport_.beat = beat;
    transport_.budgetLevel = stats_.budgetLevel;
    transport_.bpm = clock_.bpm();
    transport_.tempo = clock_.tempo();
    transport_.nudge = clock_.nudge();
    transport_.timeline = clock_.timeline();
    transport_.clips.clear();
    transport_.pilots = autopilots_.pilots(composition);
    forEachLayer(composition.layers, [&](const Layer& layer) {
        auto it = layers_.find(layer.id);
        if (it == layers_.end() || it->second.hash.empty() ||
            (hasMedia(it->second.hash) && it->second.length.frames == 0))
            return;
        const LayerState& state = it->second;
        const AutopilotStepper::Shown pilot = autopilots_.shown(layer, composition, beat);
        transport_.clips.push_back(
            {layer.id, state.play->column, state.frame, state.beats, state.visible, pilot.next, pilot.held});
    });
    const AutopilotStepper::Columns columns = autopilots_.columns(composition, beat);
    transport_.columns = {columns.column, columns.next, columns.held};
    transport_.effects = effectSteps_.shown(composition, beat);
    transport_.palette = effectSteps_.shownPalette(beat);
    transport_.paletteColours = modulation_ ? modulation_->paletteColours() : std::vector<Rgba>{};
    transport_.playing.clear();
    for (const auto& [id, state] : layers_)
        transport_.playing.push_back({id, state.play});
}

void Engine::nudge(double percent, double seconds)
{
    std::lock_guard lock(clockMutex_);
    clockActions_.push_back({seconds, percent});
}

void Engine::resync(double seconds)
{
    std::lock_guard lock(clockMutex_);
    clockActions_.push_back({seconds, std::nullopt});
}

void Engine::scrub(uint64_t layer, double position)
{
    std::lock_guard lock(clockMutex_);
    queuedScrubs_[layer] = position;
}

void Engine::resume(double beat, double seconds, std::vector<ResumePilot> pilots)
{
    std::lock_guard lock(clockMutex_);
    resume_ = Resumed{beat, seconds, std::move(pilots)};
}

void Engine::follow(BeatTimeline timeline, bool fromClock)
{
    std::lock_guard lock(clockMutex_);
    followed_ = timeline;
    followFromClock_ = fromClock;
    unfollow_ = false;
}

// A new seed takes over Random Jump at once: clips playing redraw from their anchors, as an engine with that seed
// always did.
void Engine::rekey()
{
    const uint32_t seed = seed_;
    if (seed == keyedSeed_)
        return;
    keyedSeed_ = seed;
    for (auto& [id, state] : layers_) {
        state.key = layerKey(seed, Choice::Jump, id);
        state.jumps.reset();
        if (state.from) {
            state.from->key = state.key;
            state.from->jumps.reset();
        }
    }
}

void Engine::adoptPilots(std::vector<ResumePilot> pilots)
{
    std::lock_guard lock(clockMutex_);
    adopt_ = std::move(pilots);
}

void Engine::unfollow()
{
    std::lock_guard lock(clockMutex_);
    followed_.reset();
    unfollow_ = true;
}

void Engine::injectDecoderFault(uint64_t layer, int index)
{
    const std::shared_ptr<const Composition> current = composition();
    forEachLayer(current->layers, [&](const Layer& l) {
        if (const ClipRef* clip = playingClip(l); l.id == layer && clip)
            decoders_.injectFault({layer, clip->hash}, index);
    });
}

void Engine::injectDecoderStall(uint64_t layer, int index, std::chrono::milliseconds duration)
{
    const std::shared_ptr<const Composition> current = composition();
    forEachLayer(current->layers, [&](const Layer& l) {
        if (const ClipRef* clip = playingClip(l); l.id == layer && clip)
            decoders_.injectStall({layer, clip->hash}, index, duration);
    });
}

TransportState Engine::transport() const
{
    std::lock_guard lock(clockMutex_);
    return transport_;
}

const Engine::LayerChain* Engine::pickChain(uint64_t slot, ChainKind kind, EffectList effects,
                                            const std::shared_ptr<const Composition>& document, bool text)
{
    LayerChain& state = chains_[slot];
    const std::string key = chainKey(*document, kind, effects, text);
    if (auto chain = compiler_.find(key))
        state = {std::move(chain), document, std::move(effects)};
    else if (!state.chain && compiler_.failed(key))
        state = {compiler_.find(kind == ChainKind::Group ? "group:" : chainKey(*document, ChainKind::Layer, {}, true)),
                 document, {}};
    return state.chain ? &state : nullptr;
}

const Engine::LayerChain* Engine::readyChain(uint64_t slot, ChainKind kind, EffectList effects,
                                             const std::shared_ptr<const Composition>& document, bool text)
{
    std::shared_ptr<const CompiledChain> chain = compiler_.find(chainKey(*document, kind, effects, text));
    if (!chain)
        return nullptr;
    return &(chains_[slot] = {std::move(chain), document, std::move(effects)});
}

// The composition chain reads the whole stack as a group reads its layers and draws into the frame, over black at the
// master opacity. Below full opacity and without effects the stack still renders as a group.
void Engine::addCompositionChain(std::vector<Draw>& draws, const std::shared_ptr<const Composition>& document,
                                 std::vector<std::shared_ptr<const CompiledChain>>& chains, float master)
{
    if (document->effects.empty() && master >= 1.0f)
        return;
    const LayerChain* chain = pickChain(0, ChainKind::Group, effectList(document->effects), document);
    if (!chain) {
        draws.clear();
        return;
    }
    Draw draw{&chain->chain->group, &chain->effects, nullptr, master, BlendMode::Normal, 0, std::move(draws)};
    draw.opaque = true;
    sides_.use(0, "", 0);
    chains.push_back(chain->chain);
    draws.clear();
    draws.push_back(std::move(draw));
}

void Engine::encodeStack(FrameEncoding& frame, const gpu::Texture* target, gpu::ClearColor clear,
                         std::vector<Draw>& draws)
{
    AZL_ZONE("encode stack");
    for (Draw& draw : draws) {
        const bool outer = frame.preview;
        const bool previewOnly = draw.publish && frame.previewOnly.contains(draw.publish);
        frame.preview = outer || (previewOnly && !draw.composite);
        encodeInputs(frame, draw);
        if (!draw.from.empty())
            encodeFade(frame, draw);
        frame.preview = outer;
        if (draw.divisor > 1 || (draw.publish && draw.width))
            encodeReduced(frame, draw);
        frame.preview = outer || previewOnly;
        if (draw.publish)
            encodePicture(frame, draw);
        frame.preview = outer;
    }

    if (frame.profile)
        frame.label = "stack | clear";
    gpu::RenderPass enc = beginPass(frame, target, gpu::Load::Clear, clear);
    const bool opaque = clear.a == 1.0;
    for (Draw& draw : draws) {
        if (!draw.composite)
            continue;
        if (draw.fromBlend && !draw.reduced) {
            Draw out = draw;
            out.blend = *draw.fromBlend;
            out.mix.side = 2;
            enc = composite(frame, target, enc, out, opaque);
            draw.mix.side = 1;
        }
        enc = composite(frame, target, enc, draw, opaque);
    }
    endPass(frame, enc);
}

// Under Add or Screen into an opaque stack (one whose alpha is already 1), a pixel the draw gives no colour leaves the
// stack as it is, drawn or not.
bool Engine::addsOnly(const Draw& draw, bool opaque) const
{
    return opaque && draw.from.empty() && !draw.fadeFrom && !draw.reduced &&
           (draw.blend == BlendMode::Add || draw.blend == BlendMode::Screen);
}

// Where a text layer's chain keeps black, its transparent surroundings add nothing, so it draws only over its picture's
// rectangle, with a pixel to spare.
std::optional<gpu::Scissor> Engine::textScissor(const Draw& draw, bool opaque) const
{
    if (!draw.picture || !draw.picture->text || !addsOnly(draw, opaque) || !keepsBlack(draw))
        return std::nullopt;
    const double step = draw.text[10];
    if (!(step > 0.0))
        return std::nullopt;
    const double width = double(config_.width), height = double(config_.height);
    const double left = std::clamp(std::floor(draw.text[8]) - 1.0, 0.0, width);
    const double top = std::clamp(std::floor(draw.text[9]) - 1.0, 0.0, height);
    const double right =
        std::clamp(std::ceil(draw.text[8] + double(draw.picture->luma->width()) / step) + 1.0, 0.0, width);
    const double bottom =
        std::clamp(std::ceil(draw.text[9] + double(draw.picture->luma->height()) / step) + 1.0, 0.0, height);
    if (right <= left || bottom <= top)
        return std::nullopt;
    return gpu::Scissor{uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
}

// The last pass's parameter values this frame, into params_.
void Engine::lastPassParams(const Draw& draw) const
{
    const CompiledPass& pass = draw.passes->back();
    paramsOf(pass.plan, *draw.effects, params_);
    modulation_->apply(pass.plan, *draw.effects, params_);
    bypass(pass.plan, *draw.effects, params_);
}

namespace {

// At dry/wet 0, or keeping black at these values.
bool keepsBlackStage(const EffectDef& effect, const float* p)
{
    return p[effect.params.size() - 1] == 0.0f ||
           (effect.kind == StageKind::Pointwise && effect.keepsBlack && effect.keepsBlack(p));
}

} // namespace

// One pass whose every stage keeps black this frame.
bool Engine::keepsBlack(const Draw& draw) const
{
    if (draw.passes->size() != 1)
        return false;
    lastPassParams(draw);
    size_t offset = 0;
    for (const PlannedStage& stage : draw.passes->back().plan.stages) {
        if (!keepsBlackStage(*stage.effect, &params_[offset]))
            return false;
        offset += stage.effect->params.size();
    }
    return true;
}

// The stage (from 1) of the last pass whose result is black wherever its mask is 0, at full dry/wet, with every stage
// after it keeping black; 0 when there is none. The pass discards those pixels before evaluating its source.
uint32_t Engine::blankStage(const Draw& draw, bool opaque) const
{
    if (!addsOnly(draw, opaque))
        return 0;
    lastPassParams(draw);
    uint32_t blank = 0;
    size_t offset = 0;
    const std::vector<PlannedStage>& stages = draw.passes->back().plan.stages;
    for (size_t k = 0; k < stages.size(); ++k) {
        const EffectDef& effect = *stages[k].effect;
        const float* p = &params_[offset];
        offset += effect.params.size();
        if (!effect.mask.empty() && effect.blackOutsideMask && p[effect.params.size() - 1] == 1.0f &&
            effect.blackOutsideMask(p))
            blank = uint32_t(k + 1);
        else if (!keepsBlackStage(effect, p))
            blank = 0;
    }
    return blank;
}

gpu::RenderPass Engine::composite(FrameEncoding& frame, const gpu::Texture* target, gpu::RenderPass enc,
                                  const Draw& draw, bool opaque)
{
    const auto variant = blendVariant(draw.blend);
    const gpu::Texture* below = nullptr;
    if (variant == BlendVariant::Shader || frame.profile) {
        endPass(frame, enc);
        if (variant == BlendVariant::Shader)
            below = copyBelow(frame, target);
        label(frame, draw, &draw.passes->back(), draw.from.empty() ? nullptr : "mix");
        enc = beginPass(frame, target, gpu::Load::Keep);
    }
    if (draw.reduced) {
        encodeUpscaled(enc, draw, variant, below, frame.seconds, draw.opacity, baseOf(draw));
        frame.passes++;
        return enc;
    }
    const auto scissor = textScissor(draw, opaque);
    if (scissor)
        enc.setScissor(*scissor);
    blankStage_ = blankStage(draw, opaque);
    encodeLast(enc, draw, variant, below, frame.seconds, draw.opacity);
    blankStage_ = 0;
    if (scissor)
        enc.setScissor({0, 0, target->width(), target->height()});
    frame.passes++;
    return enc;
}

// Renders what a draw's last pass reads: a group's sub-stack, then the intermediate passes of its chain.
void Engine::encodeInputs(FrameEncoding& frame, Draw& draw)
{
    if (!draw.layers.empty() || draw.opaque) {
        draw.source = poolTexture(groups_, frame.groups++, kFrameFormat,
                                  draw.upscales ? baseOf(draw.layers.front()) : sizeOf(draw));
        encodeStack(frame, draw.source, gpu::ClearColor{0, 0, 0, draw.opaque ? 1.0 : 0.0}, draw.layers);
    }
    draw.input = draw.source;
    if (draw.passes->size() < 2)
        return;
    const BakePlan bake = planBake(draw, frame.seconds);
    const bool reuse = bake.action == StaticBake::Action::Reuse;
    if (reuse) {
        frame.bakedPasses += bake.prefix;
        if (bake.kept)
            draw.input = bake.texture;
    }
    const size_t ordinal = frame.intermediates++;
    size_t written = 0;
    for (size_t i = reuse ? bake.prefix : 0; i + 1 < draw.passes->size(); ++i) {
        const CompiledPass& pass = (*draw.passes)[i];
        if (pass.plan.side && (*draw.effects)[pass.plan.stages.back().chainIndex]->bypass) {
            sides_.restart(draw.layer, effectId(*draw.effects, pass.plan.stages.back()));
            continue;
        }
        frame.passes++;
        if (pass.plan.side) {
            encodeSide(frame, pass, draw);
            continue;
        }
        auto& pool = intermediates_[pass.plan.float16];
        const auto format = pass.plan.float16 ? gpu::Format::RGBA16F : gpu::Format::BGRA8;
        const gpu::Texture* target = bake.action == StaticBake::Action::Bake && bake.kept == i
                                         ? bake.texture
                                         : poolTexture(pool, ordinal * 2 + written++ % 2, format, sizeOf(draw));
        label(frame, draw, &pass);
        gpu::RenderPass enc = beginPass(frame, target, gpu::Load::DontCare);
        encodePass(enc, pass, 0, draw, draw.input, nullptr, frame.seconds, 1.0f);
        endPass(frame, enc);
        frame.fullResolutionTargets++;
        draw.input = target;
    }
}

// Whether the draw's static prefix is drawn, drawn into its bake or read from it. A bake is a materialised
// intermediate drawn the same way into a texture of its own, so reading it gives the same picture.
Engine::BakePlan Engine::planBake(const Draw& draw, float seconds)
{
    if (!bakeStatic_)
        return {};
    std::vector<const PlannedPass*> plans;
    for (const CompiledPass& pass : *draw.passes)
        plans.push_back(&pass.plan);
    const size_t prefix = staticPrefix(plans, [&](const PlannedStage& stage) {
        return modulation_->drives((*draw.effects)[stage.chainIndex]->id);
    });
    if (prefix == 0) {
        bakes_.erase(draw.layer);
        return {};
    }
    Baked& baked = bakes_[draw.layer];
    const VideoFrame* picture = draw.picture;
    const StaticBake::Source source{picture, picture ? picture->luma : nullptr, picture ? picture->chroma : nullptr,
                                    picture ? picture->resources.get() : nullptr};
    BakePlan plan{baked.state.step(prefix, bakeInputs(draw, prefix, seconds), source), prefix};
    if (plan.action == StaticBake::Action::Live)
        return plan;
    for (size_t i = 0; i < prefix; ++i)
        if (!(*draw.passes)[i].plan.side)
            plan.kept = i;
    if (plan.action == StaticBake::Action::Bake) {
        baked.picture = picture ? picture->resources : nullptr;
        if (plan.kept) {
            const auto format =
                (*draw.passes)[*plan.kept].plan.float16 ? gpu::Format::RGBA16F : gpu::Format::BGRA8;
            const auto [width, height] = sizeOf(draw);
            if (!baked.texture || baked.texture.width() != width || baked.texture.height() != height ||
                baked.texture.format() != format)
                baked.texture = device_.makeTexture({.format = format,
                                                     .width = width,
                                                     .height = height,
                                                     .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled});
        }
    }
    plan.texture = &baked.texture;
    return plan;
}

// Everything the prefix's passes read besides their picture: what they are made of, their headers without the clocks
// and their parameter values as the shaders get them.
std::vector<uint32_t> Engine::bakeInputs(const Draw& draw, size_t prefix, float seconds) const
{
    std::vector<uint32_t> inputs;
    auto put = [&](const void* data, size_t bytes) {
        const size_t at = inputs.size();
        inputs.resize(at + (bytes + 3) / 4);
        std::memcpy(&inputs[at], data, bytes);
    };
    const uint32_t shape[] = {uint32_t(prefix), sizeOf(draw).width, sizeOf(draw).height};
    put(shape, sizeof(shape));
    for (size_t i = 0; i < prefix; ++i) {
        const PlannedPass& plan = (*draw.passes)[i].plan;
        const uint32_t pass[] = {uint32_t(plan.source), plan.side ? uint32_t(*plan.side) + 1 : 0, plan.divisor,
                                 plan.float16};
        put(pass, sizeof(pass));
        for (const PlannedStage& stage : plan.stages) {
            const Effect& effect = *(*draw.effects)[stage.chainIndex];
            put(&stage.effect, sizeof(stage.effect));
            put(&effect.id, sizeof(effect.id));
            const uint32_t bypassed = effect.bypass;
            put(&bypassed, sizeof(bypassed));
            for (size_t side = 0; side < stage.effect->sidePasses.size(); ++side) {
                const gpu::NativeTexture target = gpu::native(sides_.output(draw.layer, effect.id, side));
                put(&target, sizeof(target));
            }
        }
        PassHeader header = passHeader(draw, seconds, 1.0f);
        paramsOf(plan, *draw.effects, params_);
        modulation_->apply(plan, *draw.effects, params_);
        bypass(plan, *draw.effects, params_);
        rates_ = params_;
        modulation_->rates(plan, *draw.effects, rates_);
        derive(plan, params_, rates_, header);
        put(params_.data(), params_.size() * sizeof(float));
        header.time = 0.0f;
        header.beat = 0.0f;
        header.dt = 0.0f;
        if (plan.source == SourceKind::Text)
            std::memcpy(header.yuvToRgb, draw.text.data(), sizeof(header.yuvToRgb));
        put(&header, sizeof(header));
    }
    return inputs;
}

// The draw's last pass at full opacity into a transparent picture the frame keeps.
void Engine::encodePicture(FrameEncoding& frame, const Draw& draw)
{
    gpu::Texture picture = ring_->claimPicture();
    label(frame, draw, &draw.passes->back(), "picture");
    gpu::RenderPass enc = beginPass(frame, &picture, gpu::Load::Clear, gpu::ClearColor{0, 0, 0, 0});
    if (draw.reduced)
        encodeUpscaled(enc, draw, BlendVariant::Normal, nullptr, frame.seconds, 1.0f, {config_.width, config_.height});
    else
        encodeLast(enc, draw, BlendVariant::Normal, nullptr, frame.seconds, 1.0f);
    endPass(frame, enc);
    frame.passes++;
    frame.frame.pictures.emplace_back(draw.publish, std::move(picture));
}

// The outgoing clip of a transition through the layer's chain at full opacity into a transparent picture, clear while
// it shows nothing; the incoming clip's last pass mixes it in. When both clips draw with the same single pass on a
// video source, that pass evaluates the outgoing clip itself and no picture is written.
void Engine::encodeFade(FrameEncoding& frame, Draw& draw)
{
    Draw& from = draw.from.front();
    if (singlePass(draw))
        return;
    if (from.picture)
        encodeInputs(frame, from);
    draw.mixed[0] = poolTexture(fades_, frame.fades, kFrameFormat, sizeOf(draw));
    label(frame, from, &from.passes->back(), from.picture ? nullptr : "clear");
    // The pass blends over the target, so where the picture is transparent (around a text) it keeps what is there.
    gpu::RenderPass enc = beginPass(frame, draw.mixed[0], gpu::Load::Clear, gpu::ClearColor{0, 0, 0, 0});
    if (from.picture)
        encodePass(enc, from.passes->back(), size_t(BlendVariant::Normal), from, from.input, nullptr, frame.seconds,
                   1.0f);
    endPass(frame, enc);
    frame.passes++;
    frame.fullResolutionTargets++;
    frame.fades++;
}

// The draw's last pass at full opacity into a transparent picture at its size, which the group pass scales up into the
// stack and into a published picture. With FSR, EASU scales a reduced one up to its base size and RCAS sharpens it.
void Engine::encodeReduced(FrameEncoding& frame, Draw& draw)
{
    const size_t index = frame.reduced++;
    draw.reduced = poolTexture(reduced_, index, kFrameFormat, sizeOf(draw));
    label(frame, draw, &draw.passes->back(), "reduced");
    gpu::RenderPass enc = beginPass(frame, draw.reduced, gpu::Load::Clear, gpu::ClearColor{0, 0, 0, 0});
    encodeLast(enc, draw, BlendVariant::Normal, nullptr, frame.seconds, 1.0f);
    endPass(frame, enc);
    frame.passes++;
    if (!fsr_ || draw.divisor == 1)
        return;

    const gpu::Texture* scaled = poolTexture(easu_, 0, kFrameFormat, baseOf(draw));
    label(frame, draw, &draw.passes->back(), "easu");
    enc = beginPass(frame, scaled, gpu::Load::DontCare);
    fsr_->encodeEasu(enc, *draw.reduced, *scaled);
    endPass(frame, enc);
    draw.reduced = poolTexture(upscaled_, index, kFrameFormat, baseOf(draw));
    label(frame, draw, &draw.passes->back(), "rcas");
    enc = beginPass(frame, draw.reduced, gpu::Load::DontCare);
    fsr_->encodeRcas(enc, *scaled);
    endPass(frame, enc);
    frame.passes += 2;
}

PassHeader Engine::passHeader(const Draw& draw, float seconds, float opacity) const
{
    PassHeader header{};
    const Size size = sizeOf(draw);
    header.size[0] = float(size.width);
    header.size[1] = float(size.height);
    header.time = seconds;
    header.beat = float(frameBeat_);
    header.dt = float(frameDt_);
    header.opacity = opacity;
    header.blend = uint32_t(draw.blend);
    if (draw.picture) {
        std::memcpy(header.yuvToRgb, draw.picture->yuvToRgb.data(), sizeof(header.yuvToRgb));
        header.turns = draw.picture->headerTurns();
    }
    return header;
}

bool Engine::singlePass(const Draw& draw)
{
    if (draw.from.empty() || draw.fromBlend)
        return false;
    const Draw& from = draw.from.front();
    const CompiledPass& last = draw.passes->back();
    return from.picture && draw.picture && draw.passes->size() == 1 && from.passes->size() == 1 &&
           last.pipelines.size() > kFadeSinglePipelines &&
           from.passes->back().pipelines.front().native() == last.pipelines.front().native();
}

// A draw's last pass, or during a transition the pass that mixes its two pictures.
void Engine::encodeLast(gpu::RenderPass& enc, const Draw& draw, BlendVariant variant,
                        const gpu::Texture* below, float seconds, float opacity) const
{
    if (draw.from.empty())
        return encodePass(enc, draw.passes->back(), size_t(variant), draw, draw.input, below, seconds, opacity);
    const TransitionMix mix = scaledMix(draw);
    if (singlePass(draw)) {
        const Draw& from = draw.from.front();
        PassHeader header = passHeader(from, seconds, 1.0f);
        paramsOf(from.passes->back().plan, *from.effects, fromParams_);
        modulation_->apply(from.passes->back().plan, *from.effects, fromParams_);
        bypass(from.passes->back().plan, *from.effects, fromParams_);
        rates_ = fromParams_;
        modulation_->rates(from.passes->back().plan, *from.effects, rates_);
        derive(from.passes->back().plan, fromParams_, rates_, header);
        if (fromParams_.empty())
            fromParams_.push_back(0.0f);
        enc.native()->setFragmentTexture(from.picture->luma, kFromLumaSlot);
        enc.native()->setFragmentTexture(from.picture->chroma, kFromChromaSlot);
        enc.setBytes(3, &header, sizeof(header));
        enc.setBytes(4, fromParams_.data(), fromParams_.size() * sizeof(float));
        return encodePass(enc, draw.passes->back(), kFadeSinglePipelines + size_t(variant), draw, draw.input, below,
                          seconds, opacity, &mix, nullptr);
    }
    encodePass(enc, draw.passes->back(), kFadePipelines + size_t(variant), draw, draw.input, below, seconds, opacity,
               &mix, draw.mixed[0]);
}

void Engine::encodeUpscaled(gpu::RenderPass& enc, const Draw& draw, BlendVariant variant,
                            const gpu::Texture* below,
                            float seconds, float opacity, Size into) const
{
    Draw scaled{nullptr, &noEffects_, nullptr, opacity, draw.blend, draw.layer, {}};
    scaled.width = into.width;
    scaled.height = into.height;
    encodePass(enc, upscale_->group.back(), size_t(variant), scaled, draw.reduced, below, seconds, opacity);
}

// A side pass reads what the draw's chain reads so far; persistent targets are cleared when their history restarts.
void Engine::encodeSide(FrameEncoding& frame, const CompiledPass& pass, const Draw& draw)
{
    const PlannedStage& stage = pass.plan.stages.back();
    const uint64_t effect = effectId(*draw.effects, stage);
    const size_t side = *pass.plan.side;
    const SideTargets::Target target =
        sides_.begin(draw.layer, effect, side, stage.effect->sidePasses[side], pass.plan.float16, sizeOf(draw).width,
                     sizeOf(draw).height);
    label(frame, draw, &pass);
    if (target.fresh)
        endPass(frame, beginPass(frame, target.prev, gpu::Load::Clear, gpu::ClearColor{0, 0, 0, 0}));
    frame.fullResolutionTargets += pass.plan.divisor == 1;

    gpu::RenderPass enc = beginPass(frame, target.write, gpu::Load::DontCare);
    if (target.prev)
        enc.setTexture(kPrevSlot, target.prev);
    for (size_t i = 0; i < side; ++i)
        enc.setTexture(uint32_t(kSideSlot + i), sides_.output(draw.layer, effect, i));
    encodePass(enc, pass, 0, draw, draw.input, nullptr, frame.seconds, 1.0f);
    endPass(frame, enc);
}

gpu::RenderPass Engine::beginPass(FrameEncoding& frame, const gpu::Texture* target, gpu::Load load,
                                  gpu::ClearColor clear) const
{
    gpu::RenderTarget into{target, load, clear};
    if (frame.profile)
        profile_->attach(*frame.profile, into, frame.label);
    if (frame.preview)
        frame.previewPasses++;
    if (frame.previewTimes)
        frame.previewTimes->passes++;
    if (timesPreview(frame))
        previewTimer_->attach(*frame.previewTimes, into, "preview");
    const gpu::RenderPassSetup setup = frame.cmd.setup(into);
    AZL_GPU_ZONE(&gpu_, setup.native(), "pass");
    gpu::RenderPass pass = frame.cmd.begin(setup);
    if (frame.profile)
        profile_->sample(*frame.profile, pass, true);
    if (timesPreview(frame))
        previewTimer_->sample(*frame.previewTimes, pass, true);
    return pass;
}

void Engine::endPass(FrameEncoding& frame, gpu::RenderPass enc) const
{
    if (frame.profile)
        profile_->sample(*frame.profile, enc, false);
    if (timesPreview(frame))
        previewTimer_->sample(*frame.previewTimes, enc, false);
    enc.end();
}

bool Engine::timesPreview(const FrameEncoding& frame)
{
    return frame.preview && frame.previewTimes;
}

void Engine::label(FrameEncoding& frame, const Draw& draw, const CompiledPass* pass, const char* what) const
{
    if (!frame.profile)
        return;
    const uint64_t id = draw.layer & ~fadeId(0);
    std::string who =
        draw.layer == 0 ? "comp" : (draw.layers.empty() || draw.clipMix ? "L" : "G") + std::to_string(id);
    if (draw.layer != id)
        who += " out";
    else if (!draw.from.empty())
        who += " in";
    else if (draw.fadeFrom || draw.clipMix)
        who += " fade";
    std::string effects;
    for (const PlannedStage& stage : pass->plan.stages)
        effects += (effects.empty() ? "" : "+") + stage.effect->type;
    if (pass->plan.side)
        effects += ":s" + std::to_string(*pass->plan.side);
    if (what)
        effects += (effects.empty() ? "" : " ") + std::string(what);
    frame.label = who + " | " + (effects.empty() ? "source" : effects);
}

// Shader blend modes read the target as it is so far from a copy: Intel GPUs cannot read a render target in place.
const gpu::Texture* Engine::copyBelow(FrameEncoding& frame, const gpu::Texture* target)
{
    const gpu::Texture* below = poolTexture(below_, 0, kFrameFormat, {target->width(), target->height()});
    frame.cmd.copy(*target, *below);
    frame.passes++;
    return below;
}

void Engine::encodePass(gpu::RenderPass& enc, const CompiledPass& pass, size_t pipeline, const Draw& draw,
                        const gpu::Texture* input, const gpu::Texture* below, float seconds, float opacity,
                        const TransitionMix* mix, const gpu::Texture* from) const
{
    PassHeader header = passHeader(draw, seconds, opacity);
    if (pass.plan.intoComposition)
        header.blank = blankStage_;
    if (pass.plan.source == SourceKind::Yuv || pass.plan.source == SourceKind::YuvFade) {
        enc.native()->setFragmentTexture(draw.picture->luma, 0);
        enc.native()->setFragmentTexture(draw.picture->chroma, 1);
    }
    if (pass.plan.source == SourceKind::YuvFade) {
        PassHeader from = header;
        std::memcpy(from.yuvToRgb, draw.fadeFrom->yuvToRgb.data(), sizeof(from.yuvToRgb));
        from.turns = draw.fadeFrom->headerTurns();
        enc.native()->setFragmentTexture(draw.fadeFrom->luma, kFadeLumaSlot);
        enc.native()->setFragmentTexture(draw.fadeFrom->chroma, kFadeChromaSlot);
        enc.setBytes(2, &draw.mix, sizeof(draw.mix));
        enc.setBytes(3, &from, sizeof(from));
    }
    if (pass.plan.source == SourceKind::Text) {
        std::memcpy(header.yuvToRgb, draw.text.data(), sizeof(header.yuvToRgb));
        enc.native()->setFragmentTexture(draw.picture->luma, 0);
    }
    if (pass.plan.source == SourceKind::Texture || pass.plan.source == SourceKind::Group)
        enc.setTexture(0, input);
    if (below)
        enc.setTexture(kBelowSlot, below);
    const std::vector<size_t> combined = combinedStages(pass.plan);
    for (size_t j = 0; j < combined.size(); ++j) {
        const PlannedStage& stage = pass.plan.stages[combined[j]];
        enc.setTexture(uint32_t(kAuxSlot + j),
                       sides_.output(draw.layer, effectId(*draw.effects, stage), stage.effect->sidePasses.size() - 1));
    }

    paramsOf(pass.plan, *draw.effects, params_);
    modulation_->apply(pass.plan, *draw.effects, params_);
    bypass(pass.plan, *draw.effects, params_);
    rates_ = params_;
    modulation_->rates(pass.plan, *draw.effects, rates_);
    derive(pass.plan, params_, rates_, header);
    if (params_.empty())
        params_.push_back(0.0f);
    enc.setPipeline(pass.pipelines[pipeline]);
    enc.setBytes(0, &header, sizeof(header));
    enc.setBytes(1, params_.data(), params_.size() * sizeof(float));
    if (mix) {
        if (from)
            enc.setTexture(kFromSlot, from);
        enc.setBytes(2, mix, sizeof(*mix));
    }
    enc.draw(3);
}

const gpu::Texture* Engine::poolTexture(std::deque<gpu::Texture>& pool, size_t index, gpu::Format format, Size size)
{
    if (pool.size() <= index)
        pool.resize(index + 1);
    if (!pool[index] || pool[index].width() != size.width || pool[index].height() != size.height)
        pool[index] = device_.makeTexture({.format = format,
                                           .width = size.width,
                                           .height = size.height,
                                           .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled});
    return &pool[index];
}

void Engine::setPreviewPictures(std::set<uint64_t> layers)
{
    std::lock_guard lock(compositionMutex_);
    previewPictures_ = std::move(layers);
}

void Engine::setComposition(std::shared_ptr<const Composition> composition)
{
    auto modulation = std::make_shared<ModulationPlan>(*composition);
    const std::shared_ptr<const ModulationPlan> previous = this->modulation();
    for (const Modulator& m : composition->modulators) {
        auto in = [&](const ModulationPlan& plan) { return std::ranges::count(plan.dangling(), m.id) > 0; };
        if (in(*modulation) && !in(*previous))
            std::fprintf(stderr, "nocturne: modulator %s drives a missing parameter: layer %llu effect %llu %s\n",
                         m.name.c_str(), (unsigned long long)m.target.param.layer,
                         (unsigned long long)m.target.param.effect, m.target.param.param.c_str());
    }
    std::lock_guard lock(compositionMutex_);
    compiler_.prepare(*composition);
    composition_.swap(composition);
    modulationShared_.swap(modulation);
    if (AudioAnalysis* audio = audio_.load())
        audio->setBandEdges(composition_->bandEdges);
}

void Engine::setAudio(AudioAnalysis* audio)
{
    std::lock_guard lock(compositionMutex_);
    audio_ = audio;
    if (audio && composition_)
        audio->setBandEdges(composition_->bandEdges);
}

void Engine::refreshEffects()
{
    std::lock_guard lock(compositionMutex_);
    compiler_.refresh();
    if (composition_)
        compiler_.prepare(*composition_);
}

std::shared_ptr<const ModulationPlan> Engine::modulation() const
{
    std::lock_guard lock(compositionMutex_);
    return modulationShared_;
}

void Engine::modulate(const std::shared_ptr<ModulationPlan>& plan, double seconds, double beat)
{
    if (modulation_ && modulation_ != plan)
        plan->carry(*modulation_);
    modulation_ = plan;
    const AudioAnalysis* audio = audio_.load(std::memory_order_relaxed);
    plan->evaluate(seconds, beat, plan->usesAudio() && audio ? audio->at(seconds) : AudioFrame{});
}

void Engine::setFrameTap(std::shared_ptr<FrameTap> tap)
{
    std::lock_guard lock(compositionMutex_);
    tap_ = std::move(tap);
}

std::shared_ptr<const Composition> Engine::composition() const
{
    std::lock_guard lock(compositionMutex_);
    return composition_;
}

namespace {

// What a draw costs the GPU, in full-resolution passes: a side pass at a divisor costs its share of the pixels.
double cost(const std::vector<CompiledPass>& passes)
{
    double sum = 0.0;
    for (const CompiledPass& pass : passes)
        sum += 1.0 / double(pass.plan.divisor * pass.plan.divisor);
    return sum;
}

} // namespace

void Engine::guardBudget(std::vector<Draw>& draws, std::vector<std::shared_ptr<const CompiledChain>>& chains,
                         bool enabled)
{
    AZL_ZONE("guard budget");
    const double budget = frameBudget_;
    if (budget <= 0.0)
        return;
    if (!enabled) {
        guard_ = {};
        stats_.budgetLevel = 0;
        return;
    }
    if (!upscaleReady())
        return;
    std::vector<std::pair<double, Draw*>> layers;
    auto texts = [](const Draw& draw) {
        return (draw.picture && draw.picture->text) ||
               (!draw.from.empty() && draw.from.front().picture && draw.from.front().picture->text);
    };
    auto costOf = [](const Draw& draw) {
        return cost(*draw.passes) + (draw.from.empty() ? 0.0 : 1.0 + cost(*draw.from.front().passes));
    };
    const std::function<void(std::vector<Draw>&)> gather = [&](std::vector<Draw>& list) {
        for (Draw& draw : list) {
            if (draw.clipMix) {
                if (draw.composite && !texts(draw.layers.front()))
                    layers.emplace_back(1.0 + cost(*draw.passes) + costOf(draw.layers.front()), &draw);
                continue;
            }
            gather(draw.layers);
            if (draw.layer == 0 || !draw.layers.empty() || !draw.composite || texts(draw))
                continue;
            layers.emplace_back(costOf(draw), &draw);
        }
    };
    gather(draws);
    std::stable_sort(layers.begin(), layers.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    const double gpu = double(recentGpu_) / 1e6;
    const bool fading = std::any_of(layers_.begin(), layers_.end(), [](const auto& s) { return s.second.from != nullptr; });
    FrameGuard& g = guard_;
    if (g.step(gpu, budget, layers.size(), guardThresholds(guardProfile_, protectTransitions_, fading))) {
        stats_.budgetChanges++;
        std::fprintf(stderr, "nocturne: frame budget: GPU %.1f ms of %.1f ms, %zu layer(s) at half resolution\n",
                     gpu * 1e3, budget * 1e3, g.level);
    }
    const size_t reduced = std::min(g.level, layers.size());
    auto halve = [](Draw& draw) {
        draw.divisor = 2;
        for (Draw& from : draw.from)
            from.divisor = 2;
    };
    for (size_t i = 0; i < reduced; ++i) {
        halve(*layers[i].second);
        if (layers[i].second->clipMix)
            halve(layers[i].second->layers.front());
    }
    if (reduced)
        chains.push_back(upscale_);
    stats_.budgetLevel = reduced;
}

bool Engine::upscaleReady()
{
    if (!upscale_)
        upscale_ = compiler_.find("group:");
    return upscale_ != nullptr;
}

Engine::Size Engine::baseOf(const Draw& draw) const
{
    return draw.width ? Size{draw.width, draw.height} : Size{config_.width, config_.height};
}

Engine::Size Engine::sizeOf(const Draw& draw) const
{
    const Size base = baseOf(draw);
    return {base.width / draw.divisor, base.height / draw.divisor};
}

// The transition's mix with its wipe or push edge in the draw's pixels.
TransitionMix Engine::scaledMix(const Draw& draw) const
{
    TransitionMix mix = draw.mix;
    mix.edge = uint32_t(std::lround(double(mix.edge) * sizeOf(draw).width / config_.width));
    return mix;
}

void Engine::scaleDraws(std::vector<Draw>& draws, std::vector<std::shared_ptr<const CompiledChain>>& chains)
{
    const int percent = renderScale_;
    if (percent >= 100 || draws.empty())
        return;
    const Size size = percent >= 83 ? Size{config_.width * 5 / 6, config_.height * 5 / 6}
                                    : Size{config_.width * 2 / 3, config_.height * 2 / 3};
    const auto showsText = [](const Draw& draw, const auto& self) -> bool {
        const auto text = [](const Draw& d) { return d.fullResolution || (d.picture && d.picture->text); };
        return text(draw) || std::any_of(draw.from.begin(), draw.from.end(), text) ||
               std::any_of(draw.layers.begin(), draw.layers.end(), [&](const Draw& d) { return self(d, self); });
    };
    const auto texts = [&](const Draw& draw) { return showsText(draw, showsText); };
    // The composition chain runs at full size over text; its layers below the text are scaled within it.
    if (draws.size() == 1 && draws.front().layer == 0 && draws.front().opaque && texts(draws.front()))
        return scaleDraws(draws.front().layers, chains);
    const auto text = std::find_if(draws.begin(), draws.end(), texts);
    if (text == draws.begin() || !upscaleReady())
        return;
    const auto scale = [&](Draw& draw, const auto& self) -> void {
        draw.width = size.width;
        draw.height = size.height;
        for (Draw& d : draw.layers)
            self(d, self);
        for (Draw& d : draw.from)
            self(d, self);
    };
    Draw stack{&upscale_->group, &noEffects_, nullptr, 1.0f, BlendMode::Normal, 0,
               {std::make_move_iterator(draws.begin()), std::make_move_iterator(text)}};
    for (Draw& draw : stack.layers)
        scale(draw, scale);
    stack.opaque = true;
    stack.upscales = true;
    draws.erase(draws.begin(), text);
    draws.insert(draws.begin(), std::move(stack));
    chains.push_back(upscale_);
}

std::map<std::string, GpuProfile::Total> Engine::takeGpuProfile()
{
    return profile_ ? profile_->take() : std::map<std::string, GpuProfile::Total>{};
}

std::map<std::string, size_t> Engine::sizes()
{
    std::map<std::string, size_t> sizes = decoders_.sizes();
    const auto [free, freePictures, published] = ring_->sizes();
    const auto [compiled, wanted] = compiler_.sizes();
    sizes.insert({{"freeFrames", free}, {"freePictures", freePictures}, {"publishedFrames", published},
                  {"compiledChains", compiled}, {"wantedChains", wanted}, {"layerStates", stats_.layerStates},
                  {"fading", stats_.fading}, {"layerChains", stats_.chains}, {"sideTargets", stats_.sideTargets},
                  {"sideSpares", stats_.sideSpares},
                  {"pooledTextures", stats_.pooledTextures}, {"bakes", stats_.bakes}});
    return sizes;
}

void Engine::traceDone(FrameTrace::Render& mark, const gpu::Completion& done) const
{
    if (!config_.frameTrace)
        return;
    mark.gpuStart = traceFromHost(done.gpuStart);
    mark.gpuEnd = traceFromHost(done.gpuEnd);
    mark.done = traceNow();
    config_.frameTrace->add(mark);
}

void Engine::finished(std::shared_ptr<Frame> frame, bool ok)
{
    if (ok) {
        ring_->publish(std::move(frame));
        stats_.rendered++;
    }
    std::lock_guard lock(inFlightMutex_);
    inFlight_--;
    inFlightDone_.notify_all();
}

void Engine::waitUntilIdle()
{
    std::unique_lock lock(inFlightMutex_);
    inFlightDone_.wait(lock, [this] { return inFlight_ == 0; });
}

} // namespace azl
