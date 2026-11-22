// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app: the window thread. It owns the HWND and the Win32 message pump and never touches the GPU or the
// network, so the render loop is never blocked by a drag of the title bar or a modal loop. The render thread polls the
// small set of facts it needs (size, close request, F11) and the window forwards input to the UI as it arrives.
#pragma once
#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

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

    // Creates the window on a new thread (windowed, centred, `clientWidth` x `clientHeight`, shown) and returns once
    // it exists. False with `error` on failure.
    bool Start(const wchar_t* title, int clientWidth, int clientHeight, std::string* error);
    // Destroys the window and joins the thread. Call after the swapchain is gone.
    void Stop();

    HWND hwnd() const { return hwnd_.load(); }

    // Messages go to `ui` (HandleMessage) until it is set to nullptr. Setting nullptr waits for a call in flight.
    void SetUi(ui::Ui* ui);
    void SetFullscreenKey(uint32_t vk) { fullscreenKey_ = vk; }

    WindowSize Size() const;
    bool TakeCloseRequest() { return closeRequested_.exchange(false); }
    bool TakeFullscreenToggle() { return fullscreenToggle_.exchange(false); }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Run(const wchar_t* title, int clientWidth, int clientHeight);

    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> closeRequested_{false}, fullscreenToggle_{false};
    std::atomic<uint32_t> fullscreenKey_{VK_F11};
    std::atomic<uint64_t> sizePacked_{0}, sizeSerial_{0};   // width | height << 24 | minimized << 48
    std::mutex uiMutex_;
    ui::Ui* ui_ = nullptr;
    bool trackingMouse_ = false;

    std::promise<std::string> created_;   // "" = created, otherwise the error
};

} // namespace fluvi::app
