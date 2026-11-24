// SPDX-License-Identifier: GPL-3.0-only
#include "Window.h"

#include <algorithm>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include "fluvi/ui/Ui.h"
#include "resource.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace fluvi::app {

namespace {
constexpr wchar_t kClassName[] = L"FluviMainWindow";
constexpr UINT kWmDestroyWindow = WM_APP + 1;
constexpr UINT kWmMinimize = WM_APP + 2;
constexpr UINT kWmToggleMaximize = WM_APP + 3;
constexpr COLORREF kBed = RGB(0x0A, 0x22, 0x28);   // theme --bed

uint64_t Pack(uint32_t w, uint32_t h, bool minimized) {
    return uint64_t(w & 0xFFFFFF) | (uint64_t(h & 0xFFFFFF) << 24) | (uint64_t(minimized) << 48);
}

// Borderless fullscreen (gpu::SwapChain::SetFullscreen) removes WS_OVERLAPPEDWINDOW, including the thick frame, but
// keeps WS_MAXIMIZE if the window was maximized before.
bool IsBorderlessFullscreen(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) == 0; }

bool IsButtonHit(ui::WindowHit hit) {
    return hit == ui::WindowHit::Minimize || hit == ui::WindowHit::Maximize || hit == ui::WindowHit::Close;
}
bool IsButtonCode(WPARAM code) { return code == HTMINBUTTON || code == HTMAXBUTTON || code == HTCLOSE; }

unsigned MonitorDpi(HMONITOR monitor) {
    UINT x = 96, y = 96;
    if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x) return x;
    return 96;
}

// Maximized client area: the monitor's work area. A taskbar set to auto-hide leaves the work area equal to the monitor,
// so one pixel on its edge stays free for the mouse to reveal it.
RECT MaximizedClientRect(HWND hwnd) {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return {};
    RECT r = mi.rcWork;
    for (const UINT edge : {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM}) {
        APPBARDATA abd{sizeof(abd)};
        abd.rc = mi.rcMonitor;
        abd.uEdge = edge;
        if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &abd)) continue;
        if (edge == ABE_LEFT) r.left += 1;
        else if (edge == ABE_TOP) r.top += 1;
        else if (edge == ABE_RIGHT) r.right -= 1;
        else r.bottom -= 1;
    }
    return r;
}
}

Window::~Window() { Stop(); }

bool Window::Start(const wchar_t* title, const WindowSettings& placement, std::string* error) {
    auto result = created_.get_future();
    thread_ = std::thread([this, title = std::wstring(title), placement] { Run(title.c_str(), placement); });
    const std::string failure = result.get();
    if (!failure.empty()) {
        if (error) *error = failure;
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void Window::Stop() {
    if (!thread_.joinable()) return;
    if (HWND h = hwnd_.load()) PostMessageW(h, kWmDestroyWindow, 0, 0);
    thread_.join();
}

void Window::SetUi(ui::Ui* ui) {
    std::lock_guard lock(uiMutex_);
    ui_ = ui;
}

WindowSize Window::Size() const {
    const uint64_t p = sizePacked_.load();
    WindowSize s;
    s.width = static_cast<uint32_t>(p & 0xFFFFFF);
    s.height = static_cast<uint32_t>((p >> 24) & 0xFFFFFF);
    s.minimized = ((p >> 48) & 1) != 0;
    s.serial = sizeSerial_.load();
    return s;
}

void Window::Minimize() {
    if (HWND h = hwnd_.load()) PostMessageW(h, kWmMinimize, 0, 0);
}

void Window::ToggleMaximize() {
    if (HWND h = hwnd_.load()) PostMessageW(h, kWmToggleMaximize, 0, 0);
}

bool Window::TakePlacement(WindowSettings& out) {
    std::lock_guard lock(placementMutex_);
    if (!placementDirty_) return false;
    placementDirty_ = false;
    out = normal_;
    return true;
}

WindowSettings Window::Placement() const {
    std::lock_guard lock(placementMutex_);
    return normal_;
}

// Window thread. Keeps the normal (restored) rect: while maximized, minimized or fullscreen it stays what it was.
void Window::TrackPlacement(HWND hwnd) {
    if (IsBorderlessFullscreen(hwnd) || IsIconic(hwnd)) return;
    const bool zoomed = IsZoomed(hwnd) != FALSE;
    RECT r{};
    if (!zoomed) GetWindowRect(hwnd, &r);
    std::lock_guard lock(placementMutex_);
    if (!zoomed) {
        normal_.x = r.left;
        normal_.y = r.top;
        normal_.width = static_cast<uint32_t>(std::max<LONG>(0, r.right - r.left));
        normal_.height = static_cast<uint32_t>(std::max<LONG>(0, r.bottom - r.top));
        normal_.hasPlacement = normal_.width > 0 && normal_.height > 0;
    }
    normal_.maximized = zoomed;
    if (!inSizeMove_) placementDirty_ = true;
}

void Window::Run(const wchar_t* title, WindowSettings placement) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    HBRUSH bed = CreateSolidBrush(kBed);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::Proc;
    wc.hInstance = inst;
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_FLUVI), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_FLUVI), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = bed;   // painted until the first frame is presented: no white flash before D3D is up
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        created_.set_value("RegisterClassEx failed");
        DeleteObject(bed);
        return;
    }

    // Monitor: the one the saved placement overlaps, else the primary one.
    HMONITOR monitor = nullptr;
    if (placement.hasPlacement) {
        const RECT saved{placement.x, placement.y, placement.x + static_cast<LONG>(placement.width), placement.y + static_cast<LONG>(placement.height)};
        monitor = MonitorFromRect(&saved, MONITOR_DEFAULTTONULL);
        if (!monitor) placement.hasPlacement = false;   // that monitor is gone
    }
    if (!monitor) monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const StartRect start = ComputeStartRect(placement, {mi.rcWork, MonitorDpi(monitor)});

    // WS_OVERLAPPEDWINDOW stays (snap, minimize/maximize animations, shadow, system menu); WM_NCCALCSIZE gives the
    // caption and frame to the client area.
    const DWORD style = WS_OVERLAPPEDWINDOW;
    HWND hwnd = CreateWindowExW(0, kClassName, title, style, start.x, start.y, start.width, start.height, nullptr, nullptr, inst, this);
    if (!hwnd) {
        created_.set_value("CreateWindowEx failed");
        UnregisterClassW(kClassName, inst);
        DeleteObject(bed);
        return;
    }
    hwnd_ = hwnd;
    dpi_ = GetDpiForWindow(hwnd);

    const BOOL dark = TRUE;   // reserve: the first frames before the UI draws are dark too
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const MARGINS shadow{1, 1, 1, 1};   // keeps the DWM shadow on a window without a non-client area
    DwmExtendFrameIntoClientArea(hwnd, &shadow);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT c{};
    GetClientRect(hwnd, &c);
    sizePacked_ = Pack(static_cast<uint32_t>(c.right), static_cast<uint32_t>(c.bottom), false);
    sizeSerial_ = 1;
    {
        // The restore rect, also when the window opens maximized.
        RECT w{};
        GetWindowRect(hwnd, &w);
        std::lock_guard lock(placementMutex_);
        normal_.hasPlacement = true;
        normal_.x = w.left;
        normal_.y = w.top;
        normal_.width = static_cast<uint32_t>(w.right - w.left);
        normal_.height = static_cast<uint32_t>(w.bottom - w.top);
        normal_.maximized = start.maximized;
        placementDirty_ = false;
    }
    ShowWindow(hwnd, start.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd);
    created_.set_value({});

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    hwnd_ = nullptr;
    UnregisterClassW(kClassName, inst);
    DeleteObject(bed);
}

LRESULT CALLBACK Window::Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Handle(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Window::ForwardToUi(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool screenCoords) {
    if (screenCoords) {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        lParam = MAKELPARAM(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y));
    }
    std::lock_guard lock(uiMutex_);
    if (ui_) ui_->HandleMessage(hwnd, msg, wParam, lParam);
}

// WM_NCHITTEST: the resize borders (none when maximized or fullscreen) come from the window, the rest from the UI.
LRESULT Window::OnHitTest(HWND hwnd, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd, &pt);
    ui::WindowHit hit = ui::WindowHit::Client;
    {
        std::lock_guard lock(uiMutex_);
        if (ui_) hit = ui_->HitTest(pt.x, pt.y);
    }
    if (!IsZoomed(hwnd) && !IsBorderlessFullscreen(hwnd)) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = GetDpiForWindow(hwnd);
        const int b = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const bool button = IsButtonHit(hit);   // the buttons sit in the top-right corner: they win over its border
        const bool left = pt.x < b, bottom = pt.y >= client.bottom - b;
        const bool right = !button && pt.x >= client.right - b, top = !button && pt.y < b;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    switch (hit) {
    case ui::WindowHit::Caption: return HTCAPTION;
    case ui::WindowHit::Minimize: return HTMINBUTTON;
    case ui::WindowHit::Maximize: return HTMAXBUTTON;   // keeps the Win11 Snap Layouts flyout
    case ui::WindowHit::Close: return HTCLOSE;
    default: return HTCLIENT;
    }
}

LRESULT Window::Handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        // The client area is the whole window: no caption, no frame. Maximized, Windows would extend the window past the
        // monitor by the frame thickness, so the client area is the work area instead.
        if (wParam && IsZoomed(hwnd) && !IsBorderlessFullscreen(hwnd))
            reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0] = MaximizedClientRect(hwnd);
        return 0;
    case WM_NCHITTEST: return OnHitTest(hwnd, lParam);
    case WM_SIZE: {
        const bool minimized = wParam == SIZE_MINIMIZED;
        if (!minimized) sizePacked_ = Pack(LOWORD(lParam), HIWORD(lParam), false);
        else sizePacked_ = (sizePacked_.load() & 0xFFFFFFFFFFFFull) | (1ull << 48);
        ++sizeSerial_;
        maximized_ = wParam == SIZE_MAXIMIZED && !IsBorderlessFullscreen(hwnd);
        TrackPlacement(hwnd);
        return 0;
    }
    case WM_MOVE: TrackPlacement(hwnd); break;
    case WM_ENTERSIZEMOVE: inSizeMove_ = true; break;
    case WM_EXITSIZEMOVE:
        inSizeMove_ = false;
        TrackPlacement(hwnd);
        break;
    case WM_ACTIVATE: active_ = LOWORD(wParam) != WA_INACTIVE; break;
    case WM_GETMINMAXINFO: {
        // Minimum 1120x630 dp at the window's current DPI (the client area is the whole window).
        const SIZE min = MinWindowSize(GetDpiForWindow(hwnd));
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {min.cx, min.cy};
        return 0;
    }
    case WM_DPICHANGED: {   // keep the physical size the system suggests
        dpi_ = HIWORD(wParam);
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND:
        // Before the first present the class brush (--bed) covers the window; afterwards the swapchain owns every pixel.
        if (rendered_.load()) return 1;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_CLOSE: closeRequested_ = true; return 0;   // the render loop shuts down in order, then Stop()
    case kWmDestroyWindow: DestroyWindow(hwnd); return 0;
    case kWmMinimize: ShowWindow(hwnd, SW_MINIMIZE); return 0;
    case kWmToggleMaximize:
        if (!IsBorderlessFullscreen(hwnd)) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            std::lock_guard lock(uiMutex_);
            // Menus open: the arrow. The stream with no menu: no cursor in the way of the game.
            SetCursor(!ui_ || ui_->WantsInput() ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == fullscreenKey_.load() && (lParam & (1 << 30)) == 0) {
            fullscreenToggle_ = true;
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (!trackingMouse_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
            trackingMouse_ = TrackMouseEvent(&t) != FALSE;
        }
        break;
    case WM_MOUSELEAVE: trackingMouse_ = false; break;

    // The mouse over the title bar (caption and the three buttons) arrives as non-client messages. The UI draws the bar,
    // so it gets them as the client messages it already understands, and DefWindowProc never acts on the buttons.
    case WM_NCMOUSEMOVE:
        if (!trackingNcMouse_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            trackingNcMouse_ = TrackMouseEvent(&t) != FALSE;
        }
        if (IsButtonCode(wParam)) {
            ForwardToUi(hwnd, WM_MOUSEMOVE, 0, lParam, true);
            return 0;
        }
        if (wParam == HTCAPTION) ForwardToUi(hwnd, WM_MOUSEMOVE, 0, lParam, true);   // hover leaves the buttons
        else ForwardToUi(hwnd, WM_MOUSELEAVE, 0, 0, false);                           // a resize border: nothing is hovered
        break;
    case WM_NCMOUSELEAVE:
        trackingNcMouse_ = false;
        ForwardToUi(hwnd, WM_MOUSELEAVE, 0, 0, false);
        return 0;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (IsButtonCode(wParam)) {
            // Capture, so the release reaches the UI (as a client message) even if the pointer leaves the button.
            SetCapture(hwnd);
            ncCapture_ = true;
            ForwardToUi(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam, true);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (IsButtonCode(wParam)) {
            if (ncCapture_) {
                ncCapture_ = false;
                ReleaseCapture();
            }
            ForwardToUi(hwnd, WM_LBUTTONUP, 0, lParam, true);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (ncCapture_) {
            ncCapture_ = false;
            ReleaseCapture();
        }
        break;
    case WM_CAPTURECHANGED: ncCapture_ = false; break;
    default: break;
    }

    {
        std::lock_guard lock(uiMutex_);
        if (ui_ && ui_->HandleMessage(hwnd, msg, wParam, lParam)) return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace fluvi::app
