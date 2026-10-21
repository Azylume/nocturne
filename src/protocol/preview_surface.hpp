#pragma once

#include <CoreFoundation/CoreFoundation.h>

namespace azl {

// Every shared surface a preview names carries this value (a CFNumber): the frame index the picture in it is of, -1
// while it is being painted. A reader takes the picture only when the value is the same before and after its copy.
inline CFStringRef previewFrameKey()
{
    return CFSTR("nocturnePreviewFrame");
}

} // namespace azl
