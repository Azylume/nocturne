#pragma once

#include "gpu/gpu.hpp"
#include "media/decoded_picture.hpp"
#include "media/hap.hpp"

#include <array>
#include <memory>
#include <vector>

namespace azl {

gpu::Format textureFormat(PlaneFormat format);

// The picture's planes as sampled textures, one per plane in the plane's own format; null textures when the device
// cannot make one. `mipmapped` gives 8-bit planes a full mip chain, each level the 2x2 mean of the one below (text).
// Decode and text threads, never the render thread: uploading may wait for the GPU.
std::array<gpu::Texture, 2> uploadPicture(const gpu::Device& device, const DecodedPicture& picture,
                                          bool mipmapped = false);

// One decoded HAP image as it is (BC1, BC3 or BC4); the GPU decompresses it when sampled. Null when its size does not
// match `width` x `height` or the device cannot make it.
gpu::Texture uploadHapImage(const gpu::Device& device, const HapImage& image, int width, int height);

// The textures of one decoder's HAP frames, reused once no frame holds them: a frame keeps the shared_ptr it was given,
// and the GPU reads the texture until the frame is gone. Decode thread only, never the render thread.
class HapTexturePool {
public:
    explicit HapTexturePool(gpu::Device device) : device_(std::move(device)) {}

    // As uploadHapImage, into a texture no frame holds any more when there is one.
    std::shared_ptr<const gpu::Texture> upload(const HapImage& image, int width, int height);
    size_t size() const { return textures_.size(); }

private:
    gpu::Device device_;
    std::vector<std::shared_ptr<gpu::Texture>> textures_;
};

} // namespace azl
