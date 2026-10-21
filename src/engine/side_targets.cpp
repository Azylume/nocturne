#include "engine/side_targets.hpp"

#include <algorithm>

namespace azl {

namespace {

// Five seconds at 60 fps: longer than a bar at any usual tempo, so the spares of one trigger serve the next.
constexpr uint64_t kSpareFrames = 300;

MTL::PixelFormat formatOf(bool float16)
{
    return float16 ? MTL::PixelFormatRGBA16Float : MTL::PixelFormatBGRA8Unorm;
}

} // namespace

SideTargets::SideTargets(MTL::Device* device, uint32_t width, uint32_t height)
    : device_(NS::RetainPtr(device)), width_(width), height_(height)
{
}

SideTargets::Key SideTargets::key(uint64_t layer, uint64_t effect, size_t side)
{
    return {effect, side, layer >> 63 != 0};
}

void SideTargets::use(uint64_t layer, const std::string& clip, uint64_t parent, const std::vector<Effect>& own)
{
    Layer& state = layers_[layer];
    if (state.clip == clip && state.parent == parent)
        return;
    const bool moved = state.parent != parent;
    state = {clip, parent};
    std::erase_if(targets_, [&](auto& entry) {
        const uint64_t effect = std::get<0>(entry.first);
        const bool kept = !moved && std::any_of(own.begin(), own.end(), [&](const Effect& e) { return e.id == effect; });
        if (entry.second.layer != layer || kept)
            return false;
        release(entry.second);
        return true;
    });
}

size_t SideTargets::bytes() const
{
    size_t n = 0;
    for (const auto& [key, textures] : targets_)
        for (const NS::SharedPtr<MTL::Texture>& texture : textures.texture)
            if (texture)
                n += texture->allocatedSize();
    for (const Spare& spare : spares_)
        n += spare.texture->allocatedSize();
    return n;
}

void SideTargets::keep(const std::set<uint64_t>& layers, const std::set<uint64_t>& effects)
{
    ++frame_;
    std::erase_if(layers_, [&](const auto& entry) { return !layers.contains(entry.first); });
    std::erase_if(targets_, [&](auto& entry) {
        if (layers.contains(entry.second.layer) && effects.contains(std::get<0>(entry.first)))
            return false;
        release(entry.second);
        return true;
    });
    std::erase_if(spares_, [&](const Spare& spare) { return frame_ - spare.since > kSpareFrames; });
}

void SideTargets::release(Textures& textures)
{
    for (NS::SharedPtr<MTL::Texture>& texture : textures.texture)
        if (texture)
            spares_.push_back({std::move(texture), frame_});
    textures = {};
}

SideTargets::Target SideTargets::begin(uint64_t layer, uint64_t effect, size_t side, const SidePass& pass,
                                       bool float16, uint32_t width, uint32_t height)
{
    Textures& textures = targets_[key(layer, effect, side)];
    width = std::max(1u, (width ? width : width_) / pass.divisor);
    height = std::max(1u, (height ? height : height_) / pass.divisor);
    if (textures.texture[0] &&
        (textures.texture[0]->width() != width || textures.texture[0]->height() != height ||
         textures.texture[0]->pixelFormat() != formatOf(float16)))
        release(textures);
    textures.layer = layer;
    if (!textures.texture[0]) {
        textures.texture[0] = take(width, height, float16);
        if (pass.persistent)
            textures.texture[1] = take(width, height, float16);
    }
    if (!pass.persistent)
        return {textures.texture[0].get(), nullptr, false};

    const Target target{textures.texture[textures.written ^ 1].get(), textures.texture[textures.written].get(),
                        textures.fresh};
    textures.written ^= 1;
    textures.fresh = false;
    return target;
}

MTL::Texture* SideTargets::output(uint64_t layer, uint64_t effect, size_t side) const
{
    auto it = targets_.find(key(layer, effect, side));
    return it == targets_.end() ? nullptr : it->second.texture[it->second.written].get();
}

void SideTargets::restart(uint64_t layer, uint64_t effect)
{
    for (auto& [k, textures] : targets_)
        if (k == key(layer, effect, std::get<1>(k)))
            textures.fresh = true;
}

NS::SharedPtr<MTL::Texture> SideTargets::take(uint32_t width, uint32_t height, bool float16)
{
    const auto format = formatOf(float16);
    auto fits = [&](const Spare& spare) {
        return spare.texture->pixelFormat() == format && spare.texture->width() == width &&
               spare.texture->height() == height;
    };
    if (auto it = std::find_if(spares_.rbegin(), spares_.rend(), fits); it != spares_.rend()) {
        NS::SharedPtr<MTL::Texture> texture = std::move(it->texture);
        spares_.erase(std::next(it).base());
        return texture;
    }
    auto desc = NS::RetainPtr(MTL::TextureDescriptor::texture2DDescriptor(format, width, height, false));
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModePrivate);
    return NS::TransferPtr(device_->newTexture(desc.get()));
}

} // namespace azl
