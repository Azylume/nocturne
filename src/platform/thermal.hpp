#pragma once

#include <functional>
#include <optional>
#include <string>

namespace azl {

// The machine's thermal state as macOS reports it to a user process; a field it does not report is empty.
struct Thermal {
    // Percent of full CPU speed allowed now (CPU_Speed_Limit of `pmset -g therm`); 100 while macOS has recorded no
    // limit, which on Apple silicon is always.
    std::optional<double> cpuSpeedLimit;
    // The CPU die or proximity sensor in degrees Celsius, read from the SMC.
    std::optional<double> temperatureC;
    // NSProcessInfo's thermal state: "nominal", "fair", "serious" or "critical".
    std::string state;
};

using ThermalReader = std::function<Thermal()>;

// Takes a few milliseconds (it runs pmset): never on a render or present thread.
Thermal readThermal();

// The CPU speed limit in `pmset -g therm` output.
std::optional<double> parseCpuSpeedLimit(const std::string& pmset);

} // namespace azl
