#include "platform/clock.hpp"

#include <windows.h>

namespace azl {

double hostSeconds()
{
    static const double secondsPerTick = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1.0 / double(frequency.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) * secondsPerTick;
}

} // namespace azl
