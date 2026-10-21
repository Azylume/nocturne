#include "platform/thermal.hpp"

#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>

namespace azl {

namespace {

// AppleSMC's user client: selector 2 takes and returns this struct.
struct SmcKeyData {
    uint32_t key;
    struct {
        uint8_t major, minor, build, reserved;
        uint16_t release;
    } vers;
    struct {
        uint16_t version, length;
        uint32_t cpuLimit, gpuLimit, memLimit;
    } plimit;
    struct {
        uint32_t dataSize;
        uint32_t dataType;
        uint8_t dataAttributes;
    } info;
    uint8_t result;
    uint8_t status;
    uint8_t command;
    uint32_t data32;
    uint8_t bytes[32];
};

static_assert(sizeof(SmcKeyData) == 80);

constexpr uint8_t kReadInfo = 9;
constexpr uint8_t kReadBytes = 5;

constexpr uint32_t fourcc(const char (&s)[5])
{
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 |
           uint32_t(uint8_t(s[3]));
}

bool smcCall(io_connect_t connection, const SmcKeyData& in, SmcKeyData& out)
{
    size_t size = sizeof(SmcKeyData);
    return IOConnectCallStructMethod(connection, 2, &in, sizeof(SmcKeyData), &out, &size) == kIOReturnSuccess &&
           out.result == 0;
}

std::optional<double> smcTemperature(io_connect_t connection, uint32_t key)
{
    SmcKeyData in{}, info{}, value{};
    in.key = key;
    in.command = kReadInfo;
    if (!smcCall(connection, in, info))
        return std::nullopt;
    in.info.dataSize = info.info.dataSize;
    in.command = kReadBytes;
    if (!smcCall(connection, in, value))
        return std::nullopt;
    if (info.info.dataType == fourcc("sp78") && info.info.dataSize == 2)
        return int16_t(uint16_t(value.bytes[0]) << 8 | value.bytes[1]) / 256.0;
    if (info.info.dataType == fourcc("flt ") && info.info.dataSize == 4) {
        float f = 0.0f;
        std::memcpy(&f, value.bytes, sizeof f);
        return double(f);
    }
    return std::nullopt;
}

// Intel CPU proximity and die sensors, then Apple silicon CPU cores; the first one the SMC has.
std::optional<double> cpuTemperature()
{
    static constexpr uint32_t kKeys[] = {fourcc("TC0P"), fourcc("TC0D"), fourcc("TC0E"), fourcc("TC0F"),
                                         fourcc("Tp09"), fourcc("Tp01"), fourcc("Tp05")};
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!service)
        return std::nullopt;
    io_connect_t connection = 0;
    const kern_return_t opened = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (opened != KERN_SUCCESS)
        return std::nullopt;
    std::optional<double> celsius;
    for (uint32_t key : kKeys)
        if ((celsius = smcTemperature(connection, key)) && *celsius > 0.0)
            break;
    IOServiceClose(connection);
    return celsius && *celsius > 0.0 ? celsius : std::nullopt;
}

std::string pmsetTherm()
{
    FILE* pipe = popen("/usr/bin/pmset -g therm 2>/dev/null", "r");
    if (!pipe)
        return {};
    std::string out;
    char buffer[256];
    while (size_t n = std::fread(buffer, 1, sizeof buffer, pipe))
        out.append(buffer, n);
    pclose(pipe);
    return out;
}

std::string thermalState()
{
    switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal:
        return "nominal";
    case NSProcessInfoThermalStateFair:
        return "fair";
    case NSProcessInfoThermalStateSerious:
        return "serious";
    case NSProcessInfoThermalStateCritical:
        return "critical";
    }
    return {};
}

} // namespace

std::optional<double> parseCpuSpeedLimit(const std::string& pmset)
{
    std::smatch match;
    if (std::regex_search(pmset, match, std::regex(R"(CPU_Speed_Limit\s*=\s*(\d+))")))
        return std::stod(match[1].str());
    if (pmset.find("No CPU power status has been recorded") != std::string::npos)
        return 100.0;
    return std::nullopt;
}

Thermal readThermal()
{
    @autoreleasepool {
        return {parseCpuSpeedLimit(pmsetTherm()), cpuTemperature(), thermalState()};
    }
}

} // namespace azl
