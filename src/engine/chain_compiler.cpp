#include "engine/chain_compiler.hpp"

#include "engine/frame_ring.hpp"
#include "engine/isf_compiler.hpp"
#include "engine/metal_util.hpp"
#include "engine/thread_priority.hpp"
#include "platform/profile.hpp"
#include "supervisor/crash_guard.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>

namespace azl {

namespace {

std::vector<std::string> typesOf(const Composition& composition, const EffectList& effects)
{
    std::vector<std::string> types;
    for (const Effect* effect : effects)
        types.push_back(shaderType(composition, *effect));
    return types;
}

bool driven(const Composition& composition, uint64_t effect, const std::string& param)
{
    auto is = [&](const ParamRef& ref) { return ref.effect == effect && ref.param == param; };
    for (const Modulator& m : composition.modulators)
        if (!m.target.macro && is(m.target.param))
            return true;
    for (const Macro& macro : composition.macros)
        for (const MacroTarget& target : macro.targets)
            if (is(target.param))
                return true;
    return false;
}

bool stepped(const Effect& effect, const std::string& param)
{
    return std::any_of(effect.steps.begin(), effect.steps.end(),
                       [&](const EffectStep& step) { return step.params.contains(param); });
}

MTL::PixelFormat targetFormat(const PlannedPass& pass)
{
    if (pass.intoComposition)
        return kFrameFormat;
    return pass.float16 ? MTL::PixelFormatRGBA16Float : MTL::PixelFormatBGRA8Unorm;
}

constexpr Blending kFixedFunction[] = {Blending::Normal, Blending::Add, Blending::Screen, Blending::Multiply};

} // namespace

BlendVariant blendVariant(BlendMode mode)
{
    return mode < BlendMode::Difference ? BlendVariant(int(mode)) : BlendVariant::Shader;
}

EffectList effectList(const std::vector<Effect>& effects)
{
    EffectList list;
    for (const Effect& effect : effects)
        list.push_back(&effect);
    return list;
}

EffectList clipChain(const std::vector<Effect>& layerEffects, const ClipRef* clip)
{
    EffectList list = clip ? effectList(clip->effects) : EffectList{};
    for (const Effect& effect : layerEffects)
        list.push_back(&effect);
    return list;
}

bool holdsText(const Layer& layer)
{
    return std::any_of(layer.clips.begin(), layer.clips.end(),
                       [](const auto& cell) { return cell.second.text.has_value(); });
}

ChainKind chainKind(const ClipRef* clip)
{
    return clip && !clip->effects.empty() ? ChainKind::Clip : ChainKind::Layer;
}

void paramsOf(const PlannedPass& pass, const EffectList& effects, std::vector<float>& values)
{
    values.clear();
    for (const PlannedStage& stage : pass.stages) {
        const Effect& effect = *effects[stage.chainIndex];
        for (const ParamDef& param : stage.effect->params) {
            auto it = effect.params.find(param.name);
            values.push_back(float(it == effect.params.end() ? param.defaultValue : it->second));
        }
    }
}

void bypass(const PlannedPass& pass, const EffectList& effects, std::vector<float>& values)
{
    size_t offset = 0;
    for (const PlannedStage& stage : pass.stages) {
        const size_t count = stage.effect->params.size();
        if (effects[stage.chainIndex]->bypass && count > 0 && offset + count <= values.size())
            values[offset + count - 1] = 0.0f;
        offset += count;
    }
}

void derive(const PlannedPass& pass, std::vector<float>& values, const std::vector<float>& rates,
            const PassHeader& header)
{
    size_t offset = 0;
    for (const PlannedStage& stage : pass.stages) {
        const EffectDef& effect = *stage.effect;
        if (effect.derived > 0) {
            const auto from = values.begin() + long(offset);
            const std::vector<float> p(from, from + long(effect.params.size()));
            values.resize(values.size() + effect.derived, 0.0f);
            effect.derive(p.data(), rates.data() + offset, header.time, header.beat,
                          values.data() + values.size() - effect.derived);
        }
        offset += effect.params.size();
    }
}

std::string shaderType(const Composition& composition, const Effect& effect)
{
    const EffectDef* def = findEffect(effect.type);
    if (!def)
        return effect.type;
    for (const EffectVariant& variant : def->variants) {
        auto on = [&](const std::string& name) {
            auto value = effect.params.find(name);
            auto param = std::find_if(def->params.begin(), def->params.end(),
                                      [&](const ParamDef& p) { return p.name == name; });
            const double v = value != effect.params.end() ? value->second
                             : param != def->params.end() ? param->defaultValue
                                                          : 0.0;
            const bool set = variant.when ? variant.when(v)
                                          : param != def->params.end() && v != param->defaultValue;
            return set || driven(composition, effect.id, name) || stepped(effect, name);
        };
        if (std::any_of(variant.params.begin(), variant.params.end(), on))
            return effect.type + "_" + variant.name;
    }
    return effect.type;
}

std::string chainKey(const Composition& composition, ChainKind kind, const EffectList& effects, bool text)
{
    std::string key = kind == ChainKind::Group ? "group:" : kind == ChainKind::Clip ? "clip:" : "";
    if (text)
        key = "text+" + key;
    for (const std::string& type : typesOf(composition, effects))
        key += type + ",";
    return key;
}

ChainCompiler::ChainCompiler(MTL::Device* device, std::filesystem::path cache)
    : device_(NS::RetainPtr(device)), cachePath_(std::move(cache)), thread_([this] {
          setThreadRole(ThreadRole::Background);
          AZL_THREAD_NAME("compile");
          openArchive();
          run();
      })
{
    prepareIsf(cachePath_.empty() ? std::filesystem::path() : cachePath_.parent_path().parent_path() / "isf");
}

// An archive that does not load, as after a kill mid-write or an OS update, starts empty.
void ChainCompiler::openArchive()
{
    if (cachePath_.empty())
        return;
    if (!archivesWork(device_->name()->utf8String())) {
        std::fprintf(stderr, "nocturne: no pipeline cache on %s, which traps adding to one\n",
                     device_->name()->utf8String());
        return;
    }
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    auto desc = NS::TransferPtr(MTL::BinaryArchiveDescriptor::alloc()->init());
    std::error_code ec;
    if (std::filesystem::exists(cachePath_, ec))
        desc->setUrl(NS::URL::fileURLWithPath(NS::String::string(cachePath_.c_str(), NS::UTF8StringEncoding)));
    NS::Error* error = nullptr;
    archive_ = NS::TransferPtr(device_->newBinaryArchive(desc.get(), &error));
    if (!archive_ && desc->url()) {
        std::fprintf(stderr, "nocturne: pipeline cache %s not loaded, starting empty\n", cachePath_.c_str());
        desc->setUrl(nullptr);
        archive_ = NS::TransferPtr(device_->newBinaryArchive(desc.get(), &error));
    }
    pool->release();
}

void ChainCompiler::reportFirstIdle()
{
    if (cachePath_.empty() || std::exchange(reported_, true))
        return;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - created_).count();
    std::printf("effects: every chain ready %.2f s after start, %llu pipelines compiled, %llu from the cache\n",
                seconds, (unsigned long long)compiled_, (unsigned long long)cached_);
    std::fflush(stdout);
}

// Written aside and renamed, so a kill mid-write leaves the previous archive.
void ChainCompiler::saveArchive()
{
    if (!archive_ || !archiveChanged_)
        return;
    AZL_ZONE("save pipeline archive");
    archiveChanged_ = false;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    const std::filesystem::path written = cachePath_.string() + ".new";
    NS::Error* error = nullptr;
    std::error_code ec;
    if (archive_->serializeToURL(NS::URL::fileURLWithPath(NS::String::string(written.c_str(), NS::UTF8StringEncoding)),
                                 &error))
        std::filesystem::rename(written, cachePath_, ec);
    else
        std::fprintf(stderr, "nocturne: pipeline cache %s not written: %s\n", cachePath_.c_str(),
                     error ? error->localizedDescription()->utf8String() : "");
    pool->release();
}

ChainCompiler::~ChainCompiler()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        changed_.notify_all();
    }
    thread_.join();
}

void ChainCompiler::prepare(const Composition& composition)
{
    std::lock_guard lock(mutex_);
    wanted_.clear();
    auto want = [&](ChainKind kind, const EffectList& effects, bool text = false) {
        wantLocked(composition, kind, effects, text);
    };
    want(ChainKind::Layer, {});
    want(ChainKind::Layer, {}, true);
    want(ChainKind::Group, {});
    if (!composition.effects.empty())
        want(ChainKind::Group, effectList(composition.effects));
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (layer.group)
            return want(ChainKind::Group, effectList(layer.effects));
        const bool text = holdsText(layer);
        auto layerChain = [&](const std::vector<Effect>& effects) {
            want(ChainKind::Layer, clipChain(effects, nullptr), text);
            if (!effects.empty())
                want(ChainKind::Group, effectList(effects));
        };
        layerChain(layer.effects);
        for (const auto& [deck, settings] : layer.decks)
            if (settings.effects)
                layerChain(*settings.effects);
        for (const auto& [column, clip] : layer.clips) {
            if (chainKind(&clip) != ChainKind::Clip)
                continue;
            want(ChainKind::Clip, clipChain(deckEffects(layer, columnDeck(composition, column)), &clip), text);
            want(ChainKind::Clip, clipChain({}, &clip), text);
        }
    });
    changed_.notify_all();
}

void ChainCompiler::prepare(const Composition& composition, ChainKind kind, const EffectList& effects)
{
    std::lock_guard lock(mutex_);
    if (wanted_.size() == 1 && wanted_.contains(chainKey(composition, kind, effects)))
        return;
    wanted_.clear();
    wantLocked(composition, kind, effects, false);
    changed_.notify_all();
}

void ChainCompiler::wantLocked(const Composition& composition, ChainKind kind, const EffectList& effects, bool text)
{
    const std::string key = chainKey(composition, kind, effects, text);
    wanted_.insert(key);
    const bool queued = std::any_of(queue_.begin(), queue_.end(), [&](const Job& job) { return job.key == key; });
    if ((!ready_.contains(key) || stale_.contains(key)) && !failed_.contains(key) && !queued)
        queue_.push_back({key, typesOf(composition, effects), kind, text});
}

void ChainCompiler::refresh()
{
    std::lock_guard lock(mutex_);
    failed_.clear();
    reportedFailures_.clear();
    for (const auto& [key, chain] : ready_)
        stale_.insert(key);
}

void ChainCompiler::onFailure(Failure failure)
{
    std::lock_guard lock(mutex_);
    onFailure_ = std::move(failure);
}

std::shared_ptr<const CompiledChain> ChainCompiler::find(const std::string& key) const
{
    std::lock_guard lock(mutex_);
    auto it = ready_.find(key);
    return it == ready_.end() ? nullptr : it->second;
}

bool ChainCompiler::failed(const std::string& key) const
{
    std::lock_guard lock(mutex_);
    return failed_.contains(key);
}

std::pair<size_t, size_t> ChainCompiler::sizes() const
{
    std::lock_guard lock(mutex_);
    return {ready_.size(), wanted_.size()};
}

void ChainCompiler::hold(bool held)
{
    std::lock_guard lock(mutex_);
    held_ = held;
    changed_.notify_all();
}

void ChainCompiler::waitIdle()
{
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return (queue_.empty() && !busy_) || held_; });
}

bool ChainCompiler::idle() const
{
    std::lock_guard lock(mutex_);
    return queue_.empty() && !busy_;
}

// Chains a later edit no longer needs are dropped unbuilt.
void ChainCompiler::run()
{
    std::unique_lock lock(mutex_);
    while (true) {
        changed_.wait(lock, [this] { return stopping_ || (!held_ && !queue_.empty()); });
        if (stopping_) {
            lock.unlock();
            saveArchive();
            return;
        }
        Job job = std::move(queue_.front());
        queue_.pop_front();
        if (!wanted_.contains(job.key)) {
            changed_.notify_all();
            continue;
        }
        busy_ = true;
        lock.unlock();
        std::shared_ptr<const CompiledChain> chain = compile(job);
        lock.lock();
        if (queue_.empty()) {
            lock.unlock();
            saveArchive();
            reportFirstIdle();
            lock.lock();
        }
        busy_ = false;
        stale_.erase(job.key);
        if (chain)
            ready_[job.key] = std::move(chain);
        else
            failed_.insert(job.key);
        changed_.notify_all();
    }
}

std::shared_ptr<const CompiledChain> ChainCompiler::compile(const Job& job)
{
    AZL_ZONE("compile chain");
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    std::shared_ptr<CompiledChain> chain;
    try {
        if (crash::skipped("effect", job.key))
            throw std::runtime_error("skipped: compiling it crashed the engine before");
        std::optional<crash::Scope> compiling;
        if (!job.types.empty())
            compiling.emplace("effect", job.key, job.key);
        std::vector<const EffectDef*> effects;
        for (const std::string& type : job.types) {
            const EffectDef* effect = findEffect(type);
            if (!effect)
                throw std::runtime_error("unknown effect " + type);
            effects.push_back(effect);
        }
        chain = std::make_shared<CompiledChain>();
        if (!effects.empty() && effects.front()->generator) {
            chain->generator = compilePasses(SourceKind::Generator, effects);
            pool->release();
            return chain;
        }
        if (job.kind == ChainKind::Group)
            chain->group = compilePasses(SourceKind::Group, effects);
        if (job.kind == ChainKind::Layer)
            chain->testCard = compilePasses(SourceKind::TestCard, effects);
        if (job.kind != ChainKind::Group)
            chain->yuv = compilePasses(SourceKind::Yuv, effects);
        if (job.kind == ChainKind::Layer && !effects.empty())
            chain->yuvFade = compilePasses(SourceKind::YuvFade, effects);
        if (job.text)
            chain->text = compilePasses(SourceKind::Text, effects);
    } catch (const std::exception& e) {
        chain.reset();
        fail(job.types, e.what());
    }
    pool->release();
    return chain;
}

void ChainCompiler::fail(const std::vector<std::string>& types, const std::string& error)
{
    std::string chain;
    for (const std::string& type : types)
        chain += (chain.empty() ? "" : " > ") + type;
    std::fprintf(stderr, "nocturne: effect chain [%s] failed to compile: %s\n", chain.c_str(), error.c_str());
    Failure failure;
    {
        std::lock_guard lock(mutex_);
        if (reportedFailures_.insert(types).second)
            failure = onFailure_;
    }
    if (failure)
        failure(types, error);
}

std::vector<CompiledPass> ChainCompiler::compilePasses(SourceKind source, const std::vector<const EffectDef*>& chain)
{
    std::vector<CompiledPass> passes;
    for (PlannedPass& plan : planChain(source, chain)) {
        auto states = pipelines(plan);
        passes.push_back({std::move(plan), std::move(states)});
    }
    return passes;
}

std::vector<NS::SharedPtr<MTL::RenderPipelineState>> ChainCompiler::pipelines(const PlannedPass& pass)
{
    const MTL::PixelFormat format = targetFormat(pass);
    const std::string key = pass.metal + "|" + std::to_string(format);
    if (auto it = pipelines_.find(key); it != pipelines_.end())
        return it->second;

    AZL_ZONE("compile pipelines");
    const auto start = std::chrono::steady_clock::now();
    auto library = makeLibrary(device_.get(), pass.metal.c_str());
    std::vector<NS::SharedPtr<MTL::RenderPipelineState>> states;
    size_t archived = 0;
    auto make = [&](const char* fragment, Blending blending) {
        bool hit = false;
        auto state = makePipeline(device_.get(), library.get(), "fullscreen", fragment, format, blending, archive_.get(),
                                  &hit);
        archived += hit;
        return state;
    };
    if (!pass.intoComposition) {
        states.push_back(make("passMain", Blending::None));
    } else {
        std::vector<std::pair<const char*, const char*>> entries{{"passMain", "passBlend"}};
        if (pass.source != SourceKind::YuvFade)
            entries.push_back({"fadeMain", "fadeBlend"});
        if (singlePassFade(pass))
            entries.push_back({"fadeSingleMain", "fadeSingleBlend"});
        for (const auto& [main, blend] : entries) {
            for (Blending blending : kFixedFunction)
                states.push_back(make(main, blending));
            states.push_back(make(blend, Blending::None));
        }
    }
    const auto micros = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    stats_.pipelines += states.size() - archived;
    stats_.archived += archived;
    compiled_ += states.size() - archived;
    cached_ += archived;
    archiveChanged_ = archiveChanged_ || archived < states.size();
    stats_.maxMicros = std::max(stats_.maxMicros.load(), micros);
    pipelines_[key] = states;
    return states;
}

} // namespace azl
