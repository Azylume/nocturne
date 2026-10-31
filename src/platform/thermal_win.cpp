#include "platform/thermal.hpp"

#include <windows.h>

#include <powerbase.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

namespace azl {

namespace {

using Microsoft::WRL::ComPtr;

// PROCESSOR_POWER_INFORMATION: documented, but in no SDK header.
struct ProcessorPower {
    ULONG number;
    ULONG maxMhz;
    ULONG currentMhz;
    ULONG mhzLimit;
    ULONG maxIdleState;
    ULONG currentIdleState;
};

// The lowest clock limit any processor has now, in percent of its maximum: Windows' power and thermal throttling.
std::optional<double> cpuSpeedLimit()
{
    std::vector<ProcessorPower> processors(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, processors.data(),
                               ULONG(processors.size() * sizeof(ProcessorPower))) != 0)
        return std::nullopt;
    std::optional<double> lowest;
    for (const ProcessorPower& p : processors)
        if (p.maxMhz > 0)
            lowest = std::min({lowest.value_or(100.0), 100.0, 100.0 * p.mhzLimit / p.maxMhz});
    return lowest;
}

// COM on this thread for one reading; a thread that already has it keeps it as it was.
class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope()
    {
        if (SUCCEEDED(result_))
            CoUninitialize();
    }

private:
    HRESULT result_;
};

// The hottest of a WMI class's thermal zones, from a property in tenths of a kelvin; none when WMI has no such zone or
// refuses this user.
std::optional<double> hottestZone(const wchar_t* space, const wchar_t* query, const wchar_t* property)
{
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))))
        return std::nullopt;
    ComPtr<IWbemServices> services;
    BSTR spaceName = SysAllocString(space);
    const HRESULT connected = locator->ConnectServer(spaceName, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(spaceName);
    if (FAILED(connected))
        return std::nullopt;
    CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    BSTR language = SysAllocString(L"WQL");
    BSTR text = SysAllocString(query);
    ComPtr<IEnumWbemClassObject> rows;
    const HRESULT asked = services->ExecQuery(language, text, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                              nullptr, &rows);
    SysFreeString(language);
    SysFreeString(text);
    if (FAILED(asked))
        return std::nullopt;
    std::optional<double> hottest;
    ComPtr<IWbemClassObject> row;
    ULONG returned = 0;
    while (rows->Next(2000, 1, &row, &returned) == WBEM_S_NO_ERROR && returned == 1) {
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(row->Get(property, 0, &value, nullptr, nullptr)) && (value.vt == VT_I4 || value.vt == VT_UI4)) {
            const double celsius = double(value.ulVal) / 10.0 - 273.15;
            if (celsius > 0.0 && celsius < 150.0)
                hottest = std::max(hottest.value_or(celsius), celsius);
        }
        VariantClear(&value);
        row.Reset();
    }
    return hottest;
}

// The ACPI thermal zones: the performance counters any user may read, then the WMI class that needs an administrator.
std::optional<double> cpuTemperature()
{
    ComScope com;
    if (std::optional<double> zone = hottestZone(
            L"ROOT\\CIMV2",
            L"SELECT HighPrecisionTemperature FROM Win32_PerfFormattedData_Counters_ThermalZoneInformation",
            L"HighPrecisionTemperature"))
        return zone;
    return hottestZone(L"ROOT\\WMI", L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature",
                       L"CurrentTemperature");
}

} // namespace

Thermal readThermal()
{
    Thermal thermal{cpuSpeedLimit(), cpuTemperature(), {}};
    if (!thermal.cpuSpeedLimit && !thermal.temperatureC)
        thermal.state = "unknown";
    return thermal;
}

} // namespace azl
