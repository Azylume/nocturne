#pragma once

#include "media/hap.hpp"

#include <Metal/Metal.hpp>

namespace azl {

// A Metal texture holding one decoded HAP image as it is (BC1, BC3 or BC4); the GPU decompresses it when sampled.
// The caller owns the texture: release it with CFRelease (or release() where metal-cpp is linked). Null when the
// device cannot make it.
MTL::Texture* newHapTexture(MTL::Device* device, const HapImage& image, int width, int height);

} // namespace azl
