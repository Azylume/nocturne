#pragma once

#include <functional>
#include <optional>
#include <string>

namespace azl {

// The machine's thermal state as the system reports it to a user process; a field it does not report is empty.
struct Thermal {
    // Percent of full CPU speed allowed now (CPU_Speed_Limit of `pmset -g therm`); 100 while macOS has recorded no
    // limit, which on Apple silicon is always. On Windows the lowest processor clock limit against its maximum.
    std::optional<double> cpuSpeedLimit;
    // The CPU die or proximity sensor in degrees Celsius, read from the SMC. On Windows the hottest ACPI thermal zone
    // WMI reports, which many machines do not.
    std::optional<double> temperatureC;
    // NSProcessInfo's thermal state: "nominal", "fair", "serious" or "critical". Windows has none to report: empty, or
    // "unknown" when neither field above could be read.
    std::string state;
};

using ThermalReader = std::function<Thermal()>;

// Takes a few milliseconds (it runs pmset, or asks WMI on Windows): never on a render or present thread.
Thermal readThermal();

// The CPU speed limit in `pmset -g therm` output. macOS only.
std::optional<double> parseCpuSpeedLimit(const std::string& pmset);

} // namespace azl
