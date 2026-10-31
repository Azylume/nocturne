#pragma once

#include "output/ycbcr_pass.hpp"

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.hpp>

#include <map>
#include <memory>
#include <vector>

namespace azl {

// Writes a BGRA picture straight into an IOSurface-backed pixel buffer in the encoder's own Y'CbCr layout (BT.709,
// video range): one compute pass reads the picture once and writes the planes, so neither a copy nor the encoder's
// own format conversion runs.
//
// Chroma is subsampled the way this machine's VideoToolbox conversion does it, measured once at construction, so the
// encoder gets the picture it would have made itself. Samples keep the format's precision ('420v' and '2vuy' 8 bits, 'x420' 10
// for HEVC Main10, 'v216' 16 for ProRes), as the encoder's own conversion from BGRA does.
class YCbCrConverter {
public:
    // Throws std::runtime_error for a format it does not write.
    YCbCrConverter(MTL::Device* device, OSType format);
    ~YCbCrConverter();

    static bool writes(OSType format);
    OSType format() const { return format_; }

    // Wraps `target`'s planes as Metal textures, once per surface. False when it is not an IOSurface of this format
    // whose width is a multiple of 4 and whose height is even.
    bool prepare(CVPixelBufferRef target);
    // Render thread: encodes the conversion of `source` (the size of `target`) on `cmd`. False, with nothing encoded,
    // when `prepare` fails.
    bool encode(MTL::CommandBuffer* cmd, MTL::Texture* source, CVPixelBufferRef target);

private:
    MTL::Device* device_;
    OSType format_;
    std::unique_ptr<YCbCrPass> pass_;
    std::map<IOSurfaceRef, std::vector<gpu::Texture>> planes_;
};

} // namespace azl
