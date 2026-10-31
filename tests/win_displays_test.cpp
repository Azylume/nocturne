#include <doctest.h>

#include "platform/main_loop.hpp"
#include "platform/win_monitors.hpp"
#include "platform/win_output_window.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// An EDID block for vendor "APP" (0x0610), product 0xa050, serial 0x01020304, named "Test Panel".
std::vector<uint8_t> testEdid()
{
    std::vector<uint8_t> edid(128, 0);
    const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::copy(std::begin(header), std::end(header), edid.begin());
    edid[8] = 0x06;
    edid[9] = 0x10;
    edid[10] = 0x50;
    edid[11] = 0xa0;
    edid[12] = 0x04;
    edid[13] = 0x03;
    edid[14] = 0x02;
    edid[15] = 0x01;
    const uint8_t name[] = {0, 0, 0, 0xfc, 0, 'T', 'e', 's', 't', ' ', 'P', 'a', 'n', 'e', 'l', '\n', ' ', ' '};
    std::copy(std::begin(name), std::end(name), edid.begin() + 72);
    return edid;
}

// Pumps the main loop for `duration`.
void pump(azl::MainLoop& loop, std::chrono::milliseconds duration)
{
    loop.after(duration, [&loop] { loop.quit(); });
    loop.run();
}

azl::win::Area areaOf(const azl::DisplayInfo& d)
{
    return {d.x, d.y, int(d.width), int(d.height)};
}

bool same(const azl::win::Area& a, const azl::win::Area& b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// The monitors of this session; the window cases need a desktop session (win.sh gui), not an SSH one.
std::vector<azl::win::Monitor> desktopMonitors()
{
    azl::win::usePhysicalPixels();
    std::vector<azl::win::Monitor> monitors = azl::win::activeMonitors();
    if (monitors.empty())
        MESSAGE("no monitor in this session: run in the desktop session");
    return monitors;
}

// Whether `window` is above `other` in the z-order. Explorer's own overlays (Shell_SystemDim while the screen dims) may
// sit above both, so this compares the two directly instead of hit-testing the screen.
bool above(HWND window, HWND other)
{
    for (HWND next = GetWindow(window, GW_HWNDNEXT); next; next = GetWindow(next, GW_HWNDNEXT))
        if (next == other)
            return true;
    return false;
}

} // namespace

TEST_CASE("displays: an EDID block gives the vendor, model, serial and name macOS reports")
{
    const std::optional<azl::win::Edid> edid = azl::win::parseEdid(testEdid());
    REQUIRE(edid);
    CHECK(edid->vendor == 0x610);
    CHECK(edid->model == 0xa050);
    CHECK(edid->serial == 0x01020304);
    CHECK(edid->name == "Test Panel");
    CHECK(azl::win::displayIdentity(edid->vendor, edid->model, edid->serial, 7) == "610-a050-1020304");
    CHECK(azl::win::displayIdentity(edid->vendor, edid->model, 0, 7) == "610-a050-u7");

    std::vector<uint8_t> notEdid = testEdid();
    notEdid[1] = 0;
    CHECK_FALSE(azl::win::parseEdid(notEdid));
    CHECK_FALSE(azl::win::parseEdid(std::vector<uint8_t>(64, 0)));
}

TEST_CASE("displays: the monitors are listed with stable, distinct ids, the primary first")
{
    const std::vector<azl::win::Monitor> monitors = desktopMonitors();
    if (monitors.empty())
        return;
    std::set<std::string> ids;
    for (const azl::win::Monitor& m : monitors) {
        const azl::DisplayInfo& d = m.info;
        MESSAGE(d.id << " \"" << d.name << "\" " << d.width << "x" << d.height << " at " << d.x << "," << d.y << ", "
                     << d.refreshHz << " Hz" << (d.mirrorOf.empty() ? "" : ", mirrors " + d.mirrorOf));
        CHECK(ids.insert(d.id).second);
        CHECK_FALSE(d.name.empty());
        CHECK(d.width > 0);
        CHECK(d.height > 0);
        CHECK(d.refreshHz > 0.0);
        CHECK(azl::win::monitorHandle(m.gdiName) != nullptr);
    }
    CHECK(monitors.front().info.x == 0);
    CHECK(monitors.front().info.y == 0);
    const std::vector<azl::win::Monitor> again = azl::win::activeMonitors();
    REQUIRE(again.size() == monitors.size());
    for (size_t i = 0; i < monitors.size(); ++i)
        CHECK(again[i].info.id == monitors[i].info.id);
}

TEST_CASE("displays: a borderless output window covers its monitor, taskbar included, without taking focus")
{
    const std::vector<azl::win::Monitor> monitors = desktopMonitors();
    if (monitors.empty())
        return;
    azl::MainLoop loop;
    const azl::DisplayInfo& d = monitors.front().info;
    HWND before = GetForegroundWindow();
    azl::win::OutputWindow window(azl::win::OutputWindow::Style::Borderless, areaOf(d), "Nocturne test", true, nullptr);
    REQUIRE(window.handle());
    pump(loop, 300ms);
    const HWND hwnd = static_cast<HWND>(window.handle());
    CHECK(same(window.clientArea(), areaOf(d)));
    CHECK(window.monitor() == azl::win::monitorHandle(monitors.front().gdiName));
    CHECK((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    CHECK(IsWindowVisible(hwnd));
    CHECK(above(hwnd, FindWindowW(L"Shell_TrayWnd", nullptr)));
    CHECK(GetForegroundWindow() == before);
}

TEST_CASE("displays: a titled output window has the client area asked for and stays behind other windows")
{
    const std::vector<azl::win::Monitor> monitors = desktopMonitors();
    if (monitors.empty())
        return;
    azl::MainLoop loop;
    const azl::DisplayInfo& d = monitors.front().info;
    const azl::win::Area area{d.x + 100, d.y + 120, 960, 540};
    azl::win::OutputWindow window(azl::win::OutputWindow::Style::Titled, area, "Nocturne window-1", false, nullptr);
    REQUIRE(window.handle());
    pump(loop, 200ms);
    const HWND hwnd = static_cast<HWND>(window.handle());
    CHECK(same(window.clientArea(), area));
    CHECK((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0);
    CHECK((GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION);
    CHECK(GetForegroundWindow() != hwnd);
    wchar_t title[64] = {};
    GetWindowTextW(hwnd, title, 64);
    CHECK(std::wstring(title) == L"Nocturne window-1");
}

TEST_CASE("displays: an output window reports keys, hides the cursor over itself and refuses to close")
{
    const std::vector<azl::win::Monitor> monitors = desktopMonitors();
    if (monitors.empty())
        return;
    azl::MainLoop loop;
    const azl::DisplayInfo& d = monitors.front().info;
    std::vector<azl::win::WindowKey> keys;
    azl::win::OutputWindow window(azl::win::OutputWindow::Style::Titled, {d.x + 40, d.y + 60, 320, 180}, "keys", true,
                                  [&keys](const azl::win::WindowKey& key) { keys.push_back(key); });
    const HWND hwnd = static_cast<HWND>(window.handle());
    REQUIRE(hwnd);
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 1 | (LPARAM(1) << 30));
    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_F4, 1 | (LPARAM(1) << 29));
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    pump(loop, 100ms);
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].virtualKey == VK_ESCAPE);
    CHECK(keys[1].virtualKey == VK_F4);
    CHECK(IsWindow(hwnd));

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    CHECK(SendMessageW(hwnd, WM_SETCURSOR, WPARAM(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) == TRUE);
    CHECK(GetCursor() == nullptr);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

TEST_CASE("displays: a burst of display changes makes one change call")
{
    azl::MainLoop loop;
    int changes = 0;
    azl::win::DisplayWatcher watcher([&changes] { ++changes; });
    watcher.simulateChange();
    loop.after(100ms, [&watcher] { watcher.simulateChange(); });
    pump(loop, 200ms);
    CHECK(changes == 0);
    pump(loop, 400ms);
    CHECK(changes == 1);
}

TEST_CASE("displays: a monitor's refreshes pace the present thread at its refresh rate")
{
    const std::vector<azl::win::Monitor> monitors = desktopMonitors();
    if (monitors.empty())
        return;
    const azl::DisplayInfo& d = monitors.front().info;
    azl::win::VblankWaiter waiter(azl::win::monitorHandle(monitors.front().gdiName), d.refreshHz);
    CHECK(waiter.hardware());
    const int refreshes = int(d.refreshHz);
    std::vector<double> intervals;
    auto last = waiter.wait();
    for (int i = 0; i < refreshes; ++i) {
        const auto now = waiter.wait();
        intervals.push_back(std::chrono::duration<double>(now - last).count());
        last = now;
    }
    std::sort(intervals.begin(), intervals.end());
    const double median = intervals[intervals.size() / 2];
    MESSAGE("median interval " << median * 1000.0 << " ms, max " << intervals.back() * 1000.0 << " ms over "
                               << refreshes << " refreshes at " << d.refreshHz << " Hz");
    CHECK(median == doctest::Approx(1.0 / d.refreshHz).epsilon(0.05));
}

TEST_CASE("main loop: posted work runs on the loop's thread in order, delayed work after its delay")
{
    azl::MainLoop loop;
    CHECK(azl::MainLoop::current() == &loop);
    std::vector<int> order;
    const auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration delayed{};
    std::thread other([&] {
        for (int i = 0; i < 100; ++i)
            loop.post([&order, i] { order.push_back(i); });
        loop.after(50ms, [&] {
            delayed = std::chrono::steady_clock::now() - start;
            loop.quit();
        });
    });
    loop.run();
    other.join();
    REQUIRE(order.size() == 100);
    CHECK(std::is_sorted(order.begin(), order.end()));
    CHECK(delayed >= 45ms);
    CHECK(loop.onLoopThread());
    bool off = true;
    std::thread([&] { off = loop.onLoopThread(); }).join();
    CHECK_FALSE(off);
}
