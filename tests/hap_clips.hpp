#pragma once

#include "clips.hpp"
#include "hap_count.hpp"

namespace {

// The bottom right patch over the black composition: premultiplied by its alpha.
inline void checkHapPatches(const azl::Image& image, azl::HapCodec codec)
{
    checkColour(image, 240, 405, 200, 40, 40);
    if (hasAlpha(codec))
        checkColour(image, 720, 405, 20, 80, 40);
    else
        checkColour(image, 720, 405, 40, 160, 80);
}

} // namespace
