#pragma once

namespace azl {

// Seconds on the host clock that GPU and audio timestamps use (mach_absolute_time on Apple, QueryPerformanceCounter on
// Windows). Monotonic, any thread, never blocks.
double hostSeconds();

} // namespace azl
