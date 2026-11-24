// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app: the window thread. It owns the HWND and the Win32 message pump and never touches the GPU or the
// network, so the render loop is never blocked by a drag of the title bar or a modal loop. The render thread polls the
// small set of facts it needs (size, close request, F11, placement) and the window forwards input to the UI as it arrives.
//
// The title bar is drawn by the UI: WM_NCCALCSIZE gives the whole window to the client area, WM_NCHITTEST asks
// ui::Ui::HitTest what is under the cursor (caption, the three buttons) and the window computes the resize borders
// itself. Windows reports the mouse over the buttons as non-client messages; those are converted to client messages
// for the UI.
#pragma once
#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

#include "WindowGeometry.h"
#include "fluvi/common/Settings.h"

namespace fluvi::ui { class Ui; }

namespace fluvi::app {

struct WindowSize {
    uint32_t width = 0, height = 0;   // client area, pixels
    bool minimized = false;
    uint64_t serial = 0;              // changes on every WM_SIZE
};

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Creates the window on a new thread (restored from `placement` or default size, centered, shown) and returns once
    // it exists. False with `error` on failure.
    bool Start(const wchar_t* title, const WindowSettings& placement, std::string* error);
    // Destroys the window and joins the thread. Call after the swapchain is gone.
    void Stop();

    HWND hwnd() const { return hwnd_.load(); }

    // Messages go to `ui` (HandleMessage, HitTest) until it is set to nullptr. Setting nullptr waits for a call in flight.
    void SetUi(ui::Ui* ui);
    void SetFullscreenKey(uint32_t vk) { fullscreenKey_ = vk; }

    WindowSize Size() const;
    float DpiScale() const { return static_cast<float>(dpi_.load()) / 96.0f; }
    bool Maximized() const { return maximized_.load(); }   // windowed and maximized
    bool Active() const { return active_.load(); }
    bool TakeCloseRequest() { return closeRequested_.exchange(false); }
    bool TakeFullscreenToggle() { return fullscreenToggle_.exchange(false); }

    // Title bar buttons; any thread (the work is posted to the window thread).
    void Minimize();
    void ToggleMaximize();

    // The render loop calls this after its first present: the class background brush (no white flash before D3D) stops
    // being painted over the swapchain.
    void MarkRendered() { rendered_ = true; }

    // Last windowed placement (the normal rect, in desktop pixels). TakePlacement returns true when it changed since the
    // last call and no move/resize drag is in progress.
    bool TakePlacement(WindowSettings& out);
    WindowSettings Placement() const;

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Run(const wchar_t* title, WindowSettings placement);
    LRESULT OnHitTest(HWND hwnd, LPARAM lParam);
    void TrackPlacement(HWND hwnd);
    // Window thread: hands a mouse message to the UI with client coordinates taken from a screen-coordinate lParam.
    void ForwardToUi(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool screenCoords);

    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> closeRequested_{false}, fullscreenToggle_{false};
    std::atomic<uint32_t> fullscreenKey_{VK_F11};
    std::atomic<uint64_t> sizePacked_{0}, sizeSerial_{0};   // width | height << 24 | minimized << 48
    std::atomic<unsigned> dpi_{96};
    std::atomic<bool> maximized_{false}, active_{true}, rendered_{false};
    std::mutex uiMutex_;
    ui::Ui* ui_ = nullptr;
    bool trackingMouse_ = false, trackingNcMouse_ = false, ncCapture_ = false, inSizeMove_ = false;   // window thread only

    mutable std::mutex placementMutex_;
    WindowSettings normal_;
    bool placementDirty_ = false;

    std::promise<std::string> created_;   // "" = created, otherwise the error
};

} // namespace fluvi::app
