#include "engine/clip_effects_preview.hpp"

#include "engine/frame_ring.hpp"

#include <algorithm>
#include <cstring>

namespace azl {

ClipEffectsPreview::ClipEffectsPreview(MTL::Device* device) : device_(NS::RetainPtr(device)), compiler_(device) {}

bool ClipEffectsPreview::draw(MTL::CommandQueue* queue, const std::shared_ptr<const Composition>& composition,
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
        sides_ = std::make_unique<SideTargets>(device_.get(), width, height);
        source_.reset();
        target_.reset();
        for (auto& pair : intermediates_)
            for (auto& texture : pair)
                texture.reset();
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

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::Texture* source = texture(source_, MTL::PixelFormatBGRA8Unorm, true);
    source->replaceRegion(MTL::Region(0, 0, width, height), 0, bgra.data(), size_t(width) * 4);
    MTL::CommandBuffer* cmd = queue->commandBuffer();
    MTL::Texture* input = source;
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
                begin(cmd, target.prev, MTL::LoadActionClear)->endEncoding();
            auto* enc = begin(cmd, target.write, MTL::LoadActionDontCare);
            if (target.prev)
                enc->setFragmentTexture(target.prev, kPrevSlot);
            for (size_t j = 0; j < side; ++j)
                enc->setFragmentTexture(sides_->output(0, effect, j), kSideSlot + j);
            encodePass(enc, pass, effects, input, float(seconds));
            enc->endEncoding();
            continue;
        }
        const auto format = pass.plan.float16 ? MTL::PixelFormatRGBA16Float : MTL::PixelFormatBGRA8Unorm;
        MTL::Texture* target = texture(intermediates_[pass.plan.float16][written++ % 2], format, false);
        auto* enc = begin(cmd, target, MTL::LoadActionDontCare);
        encodePass(enc, pass, effects, input, float(seconds));
        enc->endEncoding();
        input = target;
    }
    MTL::Texture* target = texture(target_, kFrameFormat, true);
    auto* enc = begin(cmd, target, MTL::LoadActionClear, MTL::ClearColor(0, 0, 0, 1));
    encodePass(enc, passes.back(), effects, input, float(seconds));
    enc->endEncoding();
    if (target->storageMode() == MTL::StorageModeManaged) {
        auto* blit = cmd->blitCommandEncoder();
        blit->synchronizeResource(target);
        blit->endEncoding();
    }
    cmd->commit();
    cmd->waitUntilCompleted();
    const bool ok = cmd->status() == MTL::CommandBufferStatusCompleted;
    if (ok) {
        target->getBytes(bgra.data(), size_t(width) * 4, MTL::Region(0, 0, width, height), 0);
        ++stats_.pictures;
        stats_.gpuMicros += uint64_t((cmd->GPUEndTime() - cmd->GPUStartTime()) * 1e6);
    }
    pool->release();
    return ok;
}

void ClipEffectsPreview::release()
{
    width_ = 0;
    height_ = 0;
    sides_.reset();
    source_.reset();
    target_.reset();
    for (auto& pair : intermediates_)
        for (auto& texture : pair)
            texture.reset();
    planned_.reset();
    plan_.reset();
}

std::map<std::string, size_t> ClipEffectsPreview::sizes() const
{
    size_t bytes = sides_ ? sides_->bytes() : 0;
    for (const auto* texture : {&source_, &target_, &intermediates_[0][0], &intermediates_[0][1],
                                &intermediates_[1][0], &intermediates_[1][1]})
        if (*texture)
            bytes += (*texture)->allocatedSize();
    return {{"previewChains", compiler_.sizes().first},
            {"previewSideTargets", sides_ ? sides_->count() : 0},
            {"previewSideSpares", sides_ ? sides_->spares() : 0},
            {"previewEffectKB", bytes >> 10}};
}

MTL::Texture* ClipEffectsPreview::texture(NS::SharedPtr<MTL::Texture>& texture, MTL::PixelFormat format, bool cpu)
{
    if (texture)
        return texture.get();
    auto* desc = MTL::TextureDescriptor::texture2DDescriptor(format, width_, height_, false);
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    desc->setStorageMode(!cpu                         ? MTL::StorageModePrivate
                         : device_->hasUnifiedMemory() ? MTL::StorageModeShared
                                                       : MTL::StorageModeManaged);
    texture = NS::TransferPtr(device_->newTexture(desc));
    return texture.get();
}

MTL::RenderCommandEncoder* ClipEffectsPreview::begin(MTL::CommandBuffer* cmd, MTL::Texture* target,
                                                     MTL::LoadAction load, MTL::ClearColor clear)
{
    auto* desc = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = desc->colorAttachments()->object(0);
    color->setTexture(target);
    color->setLoadAction(load);
    color->setClearColor(clear);
    color->setStoreAction(MTL::StoreActionStore);
    return cmd->renderCommandEncoder(desc);
}

// Mirrors Engine::encodePass for a group source at full opacity, blending Normal in fixed function.
void ClipEffectsPreview::encodePass(MTL::RenderCommandEncoder* enc, const CompiledPass& pass,
                                    const EffectList& effects, MTL::Texture* input, float seconds)
{
    PassHeader header{};
    header.size[0] = float(width_);
    header.size[1] = float(height_);
    header.time = seconds;
    header.dt = float(dt_);
    header.opacity = 1.0f;
    header.blend = uint32_t(BlendMode::Normal);
    enc->setFragmentTexture(input, 0);
    const std::vector<size_t> combined = combinedStages(pass.plan);
    for (size_t j = 0; j < combined.size(); ++j) {
        const PlannedStage& stage = pass.plan.stages[combined[j]];
        enc->setFragmentTexture(
            sides_->output(0, effects[stage.chainIndex]->id, stage.effect->sidePasses.size() - 1), kAuxSlot + j);
    }
    paramsOf(pass.plan, effects, params_);
    plan_->apply(pass.plan, effects, params_);
    bypass(pass.plan, effects, params_);
    rates_ = params_;
    plan_->rates(pass.plan, effects, rates_);
    derive(pass.plan, params_, rates_, header);
    if (params_.empty())
        params_.push_back(0.0f);
    enc->setRenderPipelineState(pass.pipelines[size_t(BlendVariant::Normal)].get());
    enc->setFragmentBytes(&header, sizeof(header), 0);
    enc->setFragmentBytes(params_.data(), params_.size() * sizeof(float), 1);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
}

} // namespace azl
