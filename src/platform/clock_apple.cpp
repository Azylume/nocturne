#include "platform/clock.hpp"

#include <mach/mach_time.h>

namespace azl {

double hostSeconds()
{
    static const double secondsPerTick = [] {
        mach_timebase_info_data_t base;
        mach_timebase_info(&base);
        return double(base.numer) / base.denom * 1e-9;
    }();
    return double(mach_absolute_time()) * secondsPerTick;
}

} // namespace azl
