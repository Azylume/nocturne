#include "engine/static_bake.hpp"

#include <algorithm>
#include <string>

namespace azl {

namespace {

bool readsClock(const std::string& metal)
{
    static const char* const clocks[] = {"h.time", "h.beat", "h.dt", "perFrame(", "frames60("};
    return std::any_of(std::begin(clocks), std::end(clocks),
                       [&](const char* clock) { return metal.find(clock) != std::string::npos; });
}

bool staticSource(SourceKind source)
{
    return source == SourceKind::Yuv || source == SourceKind::Texture || source == SourceKind::Text ||
           source == SourceKind::Generator;
}

} // namespace

bool clockFree(const EffectDef& effect)
{
    const bool plain = std::all_of(effect.params.begin(), effect.params.end(),
                                   [](const ParamDef& param) { return param.drive == ParamDrive::Value; });
    const bool timeless = std::none_of(effect.sidePasses.begin(), effect.sidePasses.end(), [](const SidePass& side) {
        return side.persistent || readsClock(side.metal);
    });
    return plain && timeless && !readsClock(effect.metal) && !readsClock(effect.mask) &&
           !readsClock(effect.globals);
}

size_t staticPrefix(const std::vector<const PlannedPass*>& passes,
                    const std::function<bool(const PlannedStage&)>& driven)
{
    size_t prefix = 0;
    for (; prefix + 1 < passes.size(); ++prefix) {
        const PlannedPass& pass = *passes[prefix];
        if (pass.intoComposition || !staticSource(pass.source))
            break;
        const bool still = std::all_of(pass.stages.begin(), pass.stages.end(), [&](const PlannedStage& stage) {
            return clockFree(*stage.effect) && !driven(stage);
        });
        if (!still)
            break;
    }
    return prefix;
}

StaticBake::Action StaticBake::step(size_t prefix, const std::vector<uint32_t>& inputs, const Source& source)
{
    if (prefix == 0 || prefix != prefix_ || inputs != inputs_) {
        prefix_ = prefix;
        inputs_ = prefix == 0 ? std::vector<uint32_t>{} : inputs;
        settled_ = prefix == 0 ? 0 : 1;
        baked_.reset();
    } else if (settled_ < kSettleFrames) {
        ++settled_;
    }
    if (settled_ < kSettleFrames)
        return Action::Live;
    if (baked_ == source)
        return Action::Reuse;
    baked_ = source;
    return Action::Bake;
}

} // namespace azl
