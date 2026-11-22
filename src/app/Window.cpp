// SPDX-License-Identifier: GPL-3.0-only
#include "Window.h"

#include <algorithm>
#include <windowsx.h>

#include "fluvi/ui/Ui.h"
#include "resource.h"

namespace fluvi::app {

namespace {
constexpr wchar_t kClassName[] = L"FluviMainWindow";
constexpr UINT kWmDestroyWindow = WM_APP + 1;

uint64_t Pack(uint32_t w, uint32_t h, bool minimized) {
    return uint64_t(w & 0xFFFFFF) | (uint64_t(h & 0xFFFFFF) << 24) | (uint64_t(minimized) << 48);
}
}

Window::~Window() { Stop(); }

bool Window::Start(const wchar_t* title, int clientWidth, int clientHeight, std::string* error) {
    auto result = created_.get_future();
    thread_ = std::thread([this, title = std::wstring(title), clientWidth, clientHeight] { Run(title.c_str(), clientWidth, clientHeight); });
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

void Window::Run(const wchar_t* title, int clientWidth, int clientHeight) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::Proc;
    wc.hInstance = inst;
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_FLUVI), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_FLUVI), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // the render loop paints everything
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        created_.set_value("RegisterClassEx falhou");
        return;
    }

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r{0, 0, clientWidth, clientHeight};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForSystem());
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - w) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2);
    HWND hwnd = CreateWindowExW(0, kClassName, title, style, x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd) {
        created_.set_value("CreateWindowEx falhou");
        return;
    }
    hwnd_ = hwnd;
    RECT c{};
    GetClientRect(hwnd, &c);
    sizePacked_ = Pack(static_cast<uint32_t>(c.right), static_cast<uint32_t>(c.bottom), false);
    sizeSerial_ = 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    created_.set_value({});

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    hwnd_ = nullptr;
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

LRESULT Window::Handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE: {
        const bool minimized = wParam == SIZE_MINIMIZED;
        if (!minimized) sizePacked_ = Pack(LOWORD(lParam), HIWORD(lParam), false);
        else sizePacked_ = (sizePacked_.load() & 0xFFFFFFFFFFFFull) | (1ull << 48);
        ++sizeSerial_;
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = {640, 360};
        return 0;
    }
    case WM_DPICHANGED: {   // keep the physical size the system suggests
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: closeRequested_ = true; return 0;   // the render loop shuts down in order, then Stop()
    case kWmDestroyWindow: DestroyWindow(hwnd); return 0;
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
    default: break;
    }

    {
        std::lock_guard lock(uiMutex_);
        if (ui_ && ui_->HandleMessage(hwnd, msg, wParam, lParam)) return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace fluvi::app
