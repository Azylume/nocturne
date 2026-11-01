#include <doctest.h>

// macOS reads its thermal state in the engine library (platform/thermal.mm); machine_test.cpp covers it.
#ifdef _WIN32
#include "platform/thermal.hpp"

TEST_CASE("platform: the thermal reading has a plausible speed limit and temperature, or says it is unknown")
{
    const azl::Thermal thermal = azl::readThermal();
    MESSAGE("speed limit " << thermal.cpuSpeedLimit.value_or(-1.0) << " %, temperature "
                           << thermal.temperatureC.value_or(-1.0) << " C, state '" << thermal.state << "'");
    if (thermal.cpuSpeedLimit) {
        CHECK(*thermal.cpuSpeedLimit > 0.0);
        CHECK(*thermal.cpuSpeedLimit <= 100.0);
    }
    if (thermal.temperatureC) {
        CHECK(*thermal.temperatureC > 0.0);
        CHECK(*thermal.temperatureC < 150.0);
    }
    CHECK((thermal.state == "unknown") == (!thermal.cpuSpeedLimit && !thermal.temperatureC));
}
#endif
