#include "engine/clip_effects_preview.hpp"

#include "engine/frame_ring.hpp"

#include <algorithm>
#include <cstring>

namespace azl {

namespace {

// A pass begins with nothing bound, so a texture that is absent stays unbound.
void bind(gpu::RenderPass& pass, uint32_t slot, const gpu::Texture* texture)
{
    if (texture)
        pass.setTexture(slot, *texture);
}

} // namespace

ClipEffectsPreview::ClipEffectsPreview(const gpu::Device& device) : device_(device), compiler_(device)
{
}

bool ClipEffectsPreview::draw(const gpu::Queue& queue, const std::shared_ptr<const Composition>& composition,
                              const ClipRef& clip, const std::string& slot, std::vector<uint8_t>& bgra,
                              uint32_t width, uint32_t height, double seconds, double beat,
                              const AudioAnalysis* audio)
{
    const EffectList effects = effectList(clip.effects);
    compiler_.prepare(*composition, ChainKind::Group, effects);
    const std::shared_ptr<const CompiledChain> chain = compiler_.find(chainKey(*composition, ChainKind::Group, effects));
    if (!chain || chain->group.empty() || bgra.size() != size_t(width) * height * 4)
        return false;
    const std::vector<CompiledPass>& passes = chain->group;

    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        sides_ = std::make_unique<SideTargets>(device_, width, height);
        source_ = {};
        target_ = {};
        readback_ = {};
        for (auto& pair : intermediates_)
            for (auto& texture : pair)
                texture = {};
    }
    if (composition != planned_) {
        auto plan = std::make_unique<ModulationPlan>(*composition);
        if (plan_)
            plan->carry(*plan_);
        plan_ = std::move(plan);
        planned_ = composition;
    }
    dt_ = lastSeconds_ < 0.0 ? 1.0 / 60.0 : std::clamp(seconds - lastSeconds_, 0.0, 0.25);
    lastSeconds_ = seconds;
    plan_->evaluate(seconds, beat, plan_->usesAudio() && audio ? audio->at(seconds) : AudioFrame{});
    std::set<uint64_t> ids;
    for (const Effect* effect : effects)
        ids.insert(effect->id);
    sides_->keep({0}, ids);
    sides_->use(0, slot, 0);

    gpu::Scope scope;
    const gpu::Texture& source = texture(source_, gpu::Format::BGRA8, gpu::Memory::Upload);
    device_.upload(source, 0, bgra.data(), size_t(width) * 4);
    gpu::CommandBuffer cmd = queue.commandBuffer();
    const gpu::Texture* input = &source;
    size_t written = 0;
    for (size_t i = 0; i + 1 < passes.size(); ++i) {
        const CompiledPass& pass = passes[i];
        if (pass.plan.side) {
            const PlannedStage& stage = pass.plan.stages.back();
            const uint64_t effect = effects[stage.chainIndex]->id;
            if (effects[stage.chainIndex]->bypass) {
                sides_->restart(0, effect);
                continue;
            }
            const size_t side = *pass.plan.side;
            const SideTargets::Target target =
                sides_->begin(0, effect, side, stage.effect->sidePasses[side], pass.plan.float16);
            if (target.fresh)
                cmd.begin({target.prev, gpu::Load::Clear}).end();
            gpu::RenderPass into = cmd.begin({target.write, gpu::Load::DontCare});
            bind(into, kPrevSlot, target.prev);
            for (size_t j = 0; j < side; ++j)
                bind(into, uint32_t(kSideSlot + j), sides_->output(0, effect, j));
            encodePass(into, pass, effects, *input, float(seconds));
            into.end();
            continue;
        }
        const auto format = pass.plan.float16 ? gpu::Format::RGBA16F : gpu::Format::BGRA8;
        const gpu::Texture& target =
            texture(intermediates_[pass.plan.float16][written++ % 2], format, gpu::Memory::Device);
        gpu::RenderPass into = cmd.begin({&target, gpu::Load::DontCare});
        encodePass(into, pass, effects, *input, float(seconds));
        into.end();
        input = &target;
    }
    const gpu::Texture& target = texture(target_, kFrameFormat, gpu::Memory::Device);
    if (!readback_)
        readback_ = gpu::Readback(device_, target);
    gpu::RenderPass into = cmd.begin({&target, gpu::Load::Clear, {0, 0, 0, 1}});
    encodePass(into, passes.back(), effects, *input, float(seconds));
    into.end();
    readback_.encode(cmd);
    cmd.commit();
    cmd.waitUntilCompleted();
    const gpu::Completion done = cmd.completion();
    if (done.ok) {
        std::memcpy(bgra.data(), readback_.data(), bgra.size());
        ++stats_.pictures;
        stats_.gpuMicros += uint64_t((done.gpuEnd - done.gpuStart) * 1e6);
    }
    return done.ok;
}

void ClipEffectsPreview::release()
{
    width_ = 0;
    height_ = 0;
    sides_.reset();
    source_ = {};
    target_ = {};
    readback_ = {};
    for (auto& pair : intermediates_)
        for (auto& texture : pair)
            texture = {};
    planned_.reset();
    plan_.reset();
}

std::map<std::string, size_t> ClipEffectsPreview::sizes() const
{
    size_t bytes = sides_ ? sides_->bytes() : 0;
    for (const auto* texture : {&source_, &target_, &intermediates_[0][0], &intermediates_[0][1],
                                &intermediates_[1][0], &intermediates_[1][1]})
        if (*texture)
            bytes += texture->allocatedBytes();
    return {{"previewChains", compiler_.sizes().first},
            {"previewSideTargets", sides_ ? sides_->count() : 0},
            {"previewSideSpares", sides_ ? sides_->spares() : 0},
            {"previewEffectKB", bytes >> 10}};
}

const gpu::Texture& ClipEffectsPreview::texture(gpu::Texture& texture, gpu::Format format, gpu::Memory memory)
{
    if (!texture)
        texture = device_.makeTexture(
            {format, width_, height_, 1, gpu::Usage::Sampled | gpu::Usage::RenderTarget, memory});
    return texture;
}

// Mirrors Engine::encodePass for a group source at full opacity, blending Normal in fixed function.
void ClipEffectsPreview::encodePass(gpu::RenderPass& into, const CompiledPass& pass, const EffectList& effects,
                                    const gpu::Texture& input, float seconds)
{
    PassHeader header{};
    header.size[0] = float(width_);
    header.size[1] = float(height_);
    header.time = seconds;
    header.dt = float(dt_);
    header.opacity = 1.0f;
    header.blend = uint32_t(BlendMode::Normal);
    into.setTexture(0, input);
    const std::vector<size_t> combined = combinedStages(pass.plan);
    for (size_t j = 0; j < combined.size(); ++j) {
        const PlannedStage& stage = pass.plan.stages[combined[j]];
        bind(into, uint32_t(kAuxSlot + j),
             sides_->output(0, effects[stage.chainIndex]->id, stage.effect->sidePasses.size() - 1));
    }
    paramsOf(pass.plan, effects, params_);
    plan_->apply(pass.plan, effects, params_);
    bypass(pass.plan, effects, params_);
    rates_ = params_;
    plan_->rates(pass.plan, effects, rates_);
    derive(pass.plan, params_, rates_, header);
    if (params_.empty())
        params_.push_back(0.0f);
    into.setPipeline(pass.pipelines[size_t(BlendVariant::Normal)]);
    into.setBytes(0, &header, sizeof(header));
    into.setBytes(1, params_.data(), params_.size() * sizeof(float));
    into.draw(3);
}

} // namespace azl
