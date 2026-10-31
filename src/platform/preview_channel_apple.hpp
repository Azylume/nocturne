#pragma once

#include <IOSurface/IOSurfaceRef.h>

#include <cstdint>

namespace azl {

// The IOSurfaces behind PreviewChannelWriter, for the engine's preview service, which paints into them on the GPU.

// A global BGRA surface, so the UI process finds it by id; null when the system refuses. The caller releases it.
IOSurfaceRef createPreviewSurface(uint32_t width, uint32_t height);
// Names the frame the surface holds, -1 while it is painted.
void markPreviewSurface(IOSurfaceRef surface, int64_t frame);

} // namespace azl
