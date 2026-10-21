#include "media/hap_texture.hpp"

#import <Metal/Metal.h>

namespace azl {

MTL::Texture* newHapTexture(MTL::Device* device, const HapImage& image, int width, int height)
{
    MTLPixelFormat format = MTLPixelFormatBC3_RGBA;
    NSUInteger blockBytes = 16;
    if (image.texture == HapTexture::BC1)
        format = MTLPixelFormatBC1_RGBA, blockBytes = 8;
    if (image.texture == HapTexture::BC4)
        format = MTLPixelFormatBC4_RUnorm, blockBytes = 8;
    if (image.blocks.size() != hapTextureBytes(image.texture, width, height))
        return nullptr;
    id<MTLDevice> metal = (__bridge id<MTLDevice>)device;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:NSUInteger(width)
                                                                                         height:NSUInteger(height)
                                                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = metal.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    id<MTLTexture> texture = [metal newTextureWithDescriptor:descriptor];
    if (!texture)
        return nullptr;
    [texture replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(width), NSUInteger(height))
               mipmapLevel:0
                 withBytes:image.blocks.data()
               bytesPerRow:NSUInteger((width + 3) / 4) * blockBytes];
    return (__bridge_retained MTL::Texture*)texture;
}

} // namespace azl
