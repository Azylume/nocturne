#include "platform/win_monitors.hpp"

#include <windows.h>

#include <dbt.h>
#include <dxgi.h>
#include <setupapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <thread>
#include <utility>

namespace azl::win {

namespace {

constexpr GUID kMonitorInterface = {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
constexpr UINT_PTR kSettleTimer = 1;
constexpr UINT kSettleMs = 250;

std::string utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(size_t(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string descriptorText(std::span<const uint8_t> text)
{
    std::string out(text.begin(), text.end());
    out = out.substr(0, out.find('\n'));
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// The EDID block Windows keeps for the monitor behind a monitor interface path; empty when there is none.
std::vector<uint8_t> readEdid(const wchar_t* devicePath)
{
    HDEVINFO set = SetupDiGetClassDevsW(&kMonitorInterface, nullptr, nullptr, DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return {};
    std::vector<uint8_t> edid;
    SP_DEVICE_INTERFACE_DATA iface{.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA)};
    SP_DEVINFO_DATA device{.cbSize = sizeof(SP_DEVINFO_DATA)};
    DWORD detailSize = 0;
    if (SetupDiOpenDeviceInterfaceW(set, devicePath, 0, &iface)) {
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &detailSize, nullptr);
        std::vector<uint8_t> detail(std::max<DWORD>(detailSize, sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)));
        auto* data = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail.data());
        data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface, data, DWORD(detail.size()), nullptr, &device)) {
            HKEY key = SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key != INVALID_HANDLE_VALUE) {
                DWORD size = 0;
                if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size) {
                    edid.resize(size);
                    if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, edid.data(), &size) != ERROR_SUCCESS)
                        edid.clear();
                }
                RegCloseKey(key);
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return edid;
}

DISPLAYCONFIG_TARGET_DEVICE_NAME targetName(const DISPLAYCONFIG_PATH_INFO& path)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = path.targetInfo.adapterId;
    name.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS)
        return {};
    return name;
}

std::wstring sourceName(const DISPLAYCONFIG_PATH_INFO& path)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = path.sourceInfo.adapterId;
    name.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS)
        return {};
    return name.viewGdiDeviceName;
}

// The monitor's own numbers from its EDID; Windows' copy of the vendor and model where the block cannot be read.
Edid identify(const DISPLAYCONFIG_TARGET_DEVICE_NAME& target)
{
    if (std::optional<Edid> edid = parseEdid(readEdid(target.monitorDevicePath)))
        return *edid;
    Edid fallback;
    if (target.flags.edidIdsValid) {
        fallback.vendor = uint32_t(_byteswap_ushort(target.edidManufactureId));
        fallback.model = target.edidProductCodeId;
    }
    return fallback;
}

bool builtIn(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology)
{
    return technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS;
}

// The monitor's own name, Windows' name for it, or what it is.
std::string nameOf(const Edid& edid, const DISPLAYCONFIG_TARGET_DEVICE_NAME& target, const std::wstring& gdiName)
{
    if (!edid.name.empty())
        return edid.name;
    if (std::string friendly = utf8(target.monitorFriendlyDeviceName); !friendly.empty())
        return friendly;
    return builtIn(target.outputTechnology) ? "Built-in Display" : utf8(gdiName.c_str());
}

bool readPaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    for (;;) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return false;
        paths.resize(pathCount);
        modes.resize(modeCount);
        const LONG result =
            QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        paths.resize(pathCount);
        modes.resize(modeCount);
        return result == ERROR_SUCCESS;
    }
}

Monitor monitorOf(const DISPLAYCONFIG_PATH_INFO& path, const std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    const DISPLAYCONFIG_TARGET_DEVICE_NAME target = targetName(path);
    const Edid edid = identify(target);
    Monitor monitor;
    monitor.gdiName = sourceName(path);
    DisplayInfo& info = monitor.info;
    info.id = displayIdentity(edid.vendor, edid.model, edid.serial, path.targetInfo.id);
    info.name = nameOf(edid, target, monitor.gdiName);
    info.vendor = edid.vendor;
    info.model = edid.model;
    info.serial = edid.serial;
    info.unit = path.targetInfo.id;
    const DISPLAYCONFIG_RATIONAL rate = path.targetInfo.refreshRate;
    if (rate.Numerator && rate.Denominator)
        info.refreshHz = double(rate.Numerator) / double(rate.Denominator);
    const UINT32 index = path.sourceInfo.modeInfoIdx;
    if (index < modes.size() && modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        const DISPLAYCONFIG_SOURCE_MODE& mode = modes[index].sourceMode;
        info.width = mode.width;
        info.height = mode.height;
        info.x = mode.position.x;
        info.y = mode.position.y;
    }
    return monitor;
}

bool before(const Monitor& a, const Monitor& b)
{
    const bool aPrimary = a.info.x == 0 && a.info.y == 0;
    const bool bPrimary = b.info.x == 0 && b.info.y == 0;
    if (aPrimary != bPrimary)
        return aPrimary;
    return std::pair(a.info.x, a.info.y) < std::pair(b.info.x, b.info.y);
}

const wchar_t* registerClass(const wchar_t* name, WNDPROC procedure)
{
    WNDCLASSEXW cls{.cbSize = sizeof(WNDCLASSEXW)};
    cls.lpfnWndProc = procedure;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = name;
    RegisterClassExW(&cls);
    return name;
}

} // namespace

std::optional<Edid> parseEdid(std::span<const uint8_t> bytes)
{
    constexpr uint8_t kHeader[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (bytes.size() < 128 || !std::equal(std::begin(kHeader), std::end(kHeader), bytes.begin()))
        return std::nullopt;
    Edid edid;
    edid.vendor = uint32_t(bytes[8]) << 8 | bytes[9];
    edid.model = uint32_t(bytes[10]) | uint32_t(bytes[11]) << 8;
    edid.serial =
        uint32_t(bytes[12]) | uint32_t(bytes[13]) << 8 | uint32_t(bytes[14]) << 16 | uint32_t(bytes[15]) << 24;
    for (size_t at = 54; at + 18 <= 126; at += 18) {
        const std::span<const uint8_t> d = bytes.subspan(at, 18);
        if (d[0] == 0 && d[1] == 0 && d[3] == 0xfc)
            edid.name = descriptorText(d.subspan(5));
    }
    return edid;
}

std::string displayIdentity(uint32_t vendor, uint32_t model, uint32_t serial, uint32_t unit)
{
    char id[64];
    if (serial)
        std::snprintf(id, sizeof(id), "%x-%x-%x", vendor, model, serial);
    else
        std::snprintf(id, sizeof(id), "%x-%x-u%u", vendor, model, unit);
    return id;
}

std::vector<Monitor> activeMonitors()
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!readPaths(paths, modes))
        return {};
    std::vector<Monitor> own;
    std::vector<Monitor> mirrors;
    std::map<std::pair<uint64_t, UINT32>, std::string> sources;
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        Monitor monitor = monitorOf(path, modes);
        const LUID adapter = path.sourceInfo.adapterId;
        const auto source = std::pair(uint64_t(uint32_t(adapter.HighPart)) << 32 | adapter.LowPart, path.sourceInfo.id);
        const auto [shown, first] = sources.try_emplace(source, monitor.info.id);
        if (first) {
            own.push_back(std::move(monitor));
            continue;
        }
        monitor.info.mirrorOf = shown->second;
        mirrors.push_back(std::move(monitor));
    }
    std::stable_sort(own.begin(), own.end(), before);
    own.insert(own.end(), std::make_move_iterator(mirrors.begin()), std::make_move_iterator(mirrors.end()));
    return own;
}

void* monitorHandle(const std::wstring& gdiName)
{
    struct Search {
        const std::wstring& name;
        HMONITOR found = nullptr;
    } search{gdiName};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
            auto* s = reinterpret_cast<Search*>(context);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) && s->name == info.szDevice) {
                s->found = monitor;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool extendDesktop()
{
    return SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND) == ERROR_SUCCESS;
}

void usePhysicalPixels()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

struct DisplayWatcher::Impl {
    std::function<void()> changed;
    HWND window = nullptr;
    HDEVNOTIFY notification = nullptr;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (impl && impl->handle(message, wParam))
            return message == WM_DEVICECHANGE ? TRUE : 0;
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool handle(UINT message, WPARAM wParam)
    {
        switch (message) {
        case WM_DISPLAYCHANGE:
            SetTimer(window, kSettleTimer, kSettleMs, nullptr);
            return true;
        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE)
                SetTimer(window, kSettleTimer, kSettleMs, nullptr);
            return true;
        case WM_TIMER:
            if (wParam != kSettleTimer)
                return false;
            KillTimer(window, kSettleTimer);
            if (changed)
                changed();
            return true;
        default:
            return false;
        }
    }
};

DisplayWatcher::DisplayWatcher(std::function<void()> changed) : impl_(std::make_unique<Impl>())
{
    static const wchar_t* cls = registerClass(L"NocturneDisplayWatcher", &Impl::procedure);
    impl_->changed = std::move(changed);
    // A hidden top-level window: message-only windows get no WM_DISPLAYCHANGE broadcast.
    impl_->window = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    if (!impl_->window)
        return;
    SetWindowLongPtrW(impl_->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_.get()));
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kMonitorInterface;
    impl_->notification = RegisterDeviceNotificationW(impl_->window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

DisplayWatcher::~DisplayWatcher()
{
    if (impl_->notification)
        UnregisterDeviceNotification(impl_->notification);
    if (impl_->window)
        DestroyWindow(impl_->window);
}

void DisplayWatcher::simulateChange()
{
    SendMessageW(impl_->window, WM_DISPLAYCHANGE, 32, 0);
}

using Microsoft::WRL::ComPtr;

struct VblankWaiter::Impl {
    ComPtr<IDXGIOutput> output;
    std::chrono::steady_clock::duration period{};
    std::chrono::steady_clock::time_point next{};

    std::chrono::steady_clock::time_point tick()
    {
        const auto now = std::chrono::steady_clock::now();
        next = next + period > now ? next + period : now + period;
        std::this_thread::sleep_until(next);
        return next;
    }
};

namespace {

ComPtr<IDXGIOutput> outputShowing(HMONITOR monitor)
{
    ComPtr<IDXGIFactory1> factory;
    if (!monitor || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                return output;
        }
    }
    return nullptr;
}

} // namespace

VblankWaiter::VblankWaiter(void* monitor, double refreshHz) : impl_(std::make_unique<Impl>())
{
    impl_->output = outputShowing(static_cast<HMONITOR>(monitor));
    impl_->period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / (refreshHz > 0.0 ? refreshHz : 60.0)));
    impl_->next = std::chrono::steady_clock::now();
}

VblankWaiter::~VblankWaiter() = default;

std::chrono::steady_clock::time_point VblankWaiter::wait()
{
    if (impl_->output && SUCCEEDED(impl_->output->WaitForVBlank()))
        return std::chrono::steady_clock::now();
    impl_->output.Reset();
    return impl_->tick();
}

bool VblankWaiter::hardware() const
{
    return bool(impl_->output);
}

} // namespace azl::win
