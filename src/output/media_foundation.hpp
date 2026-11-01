#pragma once

#include "media/ffmpeg_util.hpp"

#include <string_view>

namespace azl::mf {

// Whether Windows lists a Media Foundation hardware encoder for the codec (H.264, HEVC or AV1), the same search FFmpeg's
// *_mf encoders make with hw_encoding on. Asked of Windows once per codec per process; always false off Windows.
bool hasHardwareEncoder(AVCodecID codec);

// Whether an encoder is worth opening. FFmpeg's *_mf encoders with hw_encoding on, where Windows lists no hardware
// encoder, fail to open and shut Media Foundation and COM down once more than they started them; Media Foundation
// encoders already running then crash when they close. So such an encoder is never opened there.
inline bool worthOpening(std::string_view encoder, bool hardware, bool hardwareEncoderListed)
{
    return !hardware || hardwareEncoderListed || !encoder.ends_with("_mf");
}

} // namespace azl::mf
