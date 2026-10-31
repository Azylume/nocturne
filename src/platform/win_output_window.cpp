#include "platform/win_output_window.hpp"

#include <windows.h>

#include <string>

namespace azl::win {

namespace {

constexpr DWORD kTitledStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

std::wstring wide(const std::string& text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1)
        return {};
    std::wstring out(size_t(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

bool held(int key)
{
    return (GetKeyState(key) & 0x8000) != 0;
}

} // namespace

struct OutputWindow::Impl {
    HWND window = nullptr;
    bool hideCursor = false;
    std::function<void(const WindowKey&)> onKey;
    std::function<void(void*, int, int)> paint;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!impl)
            return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_SETCURSOR:
            if (!impl->hideCursor || LOWORD(lParam) != HTCLIENT)
                break;
            SetCursor(nullptr);
            return TRUE;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            impl->key(unsigned(wParam), lParam);
            if (message == WM_SYSKEYDOWN && wParam == VK_F4)
                return 0;
            break;
        case WM_CLOSE:
            return 0;
        case WM_PAINT:
            if (!impl->paint)
                break;
            impl->paintNow();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void key(unsigned virtualKey, LPARAM lParam)
    {
        const bool repeat = (lParam & (LPARAM(1) << 30)) != 0;
        if (repeat || !onKey)
            return;
        onKey({.virtualKey = virtualKey,
               .alt = held(VK_MENU),
               .control = held(VK_CONTROL),
               .shift = held(VK_SHIFT),
               .windows = held(VK_LWIN) || held(VK_RWIN)});
    }

    void paintNow()
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        paint(dc, int(client.right), int(client.bottom));
        EndPaint(window, &ps);
    }

    static const wchar_t* windowClass()
    {
        static const wchar_t* name = [] {
            WNDCLASSEXW cls{.cbSize = sizeof(WNDCLASSEXW)};
            cls.lpfnWndProc = &procedure;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            cls.lpszClassName = L"NocturneOutput";
            RegisterClassExW(&cls);
            return cls.lpszClassName;
        }();
        return name;
    }
};

OutputWindow::OutputWindow(Style style, Area area, const std::string& title, bool hideCursor,
                           std::function<void(const WindowKey&)> onKey)
    : impl_(std::make_unique<Impl>())
{
    impl_->hideCursor = hideCursor;
    impl_->onKey = std::move(onKey);
    const bool titled = style == Style::Titled;
    const DWORD exStyle = titled ? 0 : WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    const DWORD windowStyle = titled ? kTitledStyle : WS_POPUP;
    HWND window = CreateWindowExW(exStyle, Impl::windowClass(), wide(title).c_str(), windowStyle, area.x, area.y,
                                  area.width, area.height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window)
        return;
    impl_->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_.get()));
    if (!titled) {
        SetWindowPos(window, HWND_TOPMOST, area.x, area.y, area.width, area.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return;
    }
    RECT frame{area.x, area.y, area.x + area.width, area.y + area.height};
    AdjustWindowRectExForDpi(&frame, windowStyle, FALSE, exStyle, GetDpiForWindow(window));
    EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    SetWindowPos(window, HWND_BOTTOM, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

OutputWindow::~OutputWindow()
{
    if (impl_->window)
        DestroyWindow(impl_->window);
}

void* OutputWindow::handle() const
{
    return impl_->window;
}

Area OutputWindow::clientArea() const
{
    RECT client{};
    GetClientRect(impl_->window, &client);
    POINT origin{0, 0};
    ClientToScreen(impl_->window, &origin);
    return {origin.x, origin.y, int(client.right - client.left), int(client.bottom - client.top)};
}

void* OutputWindow::monitor() const
{
    return MonitorFromWindow(impl_->window, MONITOR_DEFAULTTONEAREST);
}

void OutputWindow::onPaint(std::function<void(void* dc, int width, int height)> paint)
{
    impl_->paint = std::move(paint);
    InvalidateRect(impl_->window, nullptr, TRUE);
}

} // namespace azl::win
