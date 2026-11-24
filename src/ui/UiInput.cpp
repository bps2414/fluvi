// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: window messages queued on the window thread and replayed on the render thread, hotkeys, the focus
// keeper for D-pad use, and the title-bar rectangles behind HitTest().
#include "UiImpl.h"

#include <algorithm>
#include <array>
#include <format>

#include "fluvi/core/Log.h"
#include "RmlUi_Platform_Win32_Fluvi.h"

namespace fluvi::ui {
namespace {

// windowsx.h's GET_X/Y_LPARAM, without its macros that clash with RmlUi's Element API (GetFirstChild...).
int MouseX(LPARAM lp) { return static_cast<int>(static_cast<short>(LOWORD(lp))); }
int MouseY(LPARAM lp) { return static_cast<int>(static_cast<short>(HIWORD(lp))); }

bool FirstPress(LPARAM lParam) { return (lParam & (1 << 30)) == 0; }

// Settings nav items, indexed by set.section.
constexpr std::array<const char*, 7> kNavIds = {"nav-video", "nav-smooth", "nav-pad", "nav-audio", "nav-keys", "nav-stats", "nav-about"};

bool Inside(Rml::Element* e, const char* cls) {
    for (; e; e = e->GetParentNode())
        if (e->IsClassSet(cls)) return true;
    return false;
}

Rml::Rectanglef BorderRect(Rml::Element* e) {
    if (!e) return {};
    return Rml::Rectanglef::FromPositionSize(e->GetAbsoluteOffset(Rml::BoxArea::Border), e->GetBox().GetSize(Rml::BoxArea::Border));
}

} // namespace

void UiImpl::Enqueue(const InputEvent& e) {
    std::lock_guard lock(mutex_);
    if (pending_.input.size() < 512) pending_.input.push_back(e);
}

bool UiImpl::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool wants = WantsInput();
    const Screen screen = screen_.load();
    if (!hwnd) hwnd = hwnd_;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const auto vk = static_cast<uint32_t>(wParam);
        // Controller buttons arrive as keys sent from the render thread, not from the window's own thread.
        const bool pad = hwnd && GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId();
        auto hotkey = [&](Hotkey key) {
            if (FirstPress(lParam)) Enqueue({InputEvent::Kind::Hotkey, static_cast<int>(key), 0, 0, 0.0f, pad});
            return true;
        };
        if (screen == Screen::Stream && !dialog_.load()) {
            if (vk == hkOverlay_.load()) return hotkey(Hotkey::ToggleOverlay);
            if (vk == hkMode_.load()) return hotkey(Hotkey::ToggleMode);
            if (vk == hkHud_.load()) return hotkey(Hotkey::CycleHud);
        }
#ifndef NDEBUG
        if (vk == VK_F5) return hotkey(Hotkey::Reload);
#endif
        if (!wants || msg == WM_SYSKEYDOWN) return false;
        if (vk == VK_ESCAPE) return hotkey(Hotkey::Back);
        Enqueue({InputEvent::Kind::KeyDown, RmlWin32Fluvi::ConvertKey(static_cast<int>(vk)), 0, RmlWin32Fluvi::GetKeyModifierState(), 0.0f, pad});
        return true;
    }
    case WM_KEYUP:
        if (!wants) return false;
        Enqueue({InputEvent::Kind::KeyUp, RmlWin32Fluvi::ConvertKey(static_cast<int>(wParam)), 0, RmlWin32Fluvi::GetKeyModifierState()});
        return true;
    case WM_CHAR:
        if (!wants) return false;
        if (wParam >= 32 && wParam != 127) Enqueue({InputEvent::Kind::Char, static_cast<int>(wParam)});
        return true;
    case WM_MOUSEMOVE:
        if (!wants) return false;
        Enqueue({InputEvent::Kind::MouseMove, MouseX(lParam), MouseY(lParam), RmlWin32Fluvi::GetKeyModifierState()});
        return true;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        if (!wants) return false;
        const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1 : 2;
        Enqueue({InputEvent::Kind::MouseMove, MouseX(lParam), MouseY(lParam), RmlWin32Fluvi::GetKeyModifierState()});
        Enqueue({down ? InputEvent::Kind::MouseDown : InputEvent::Kind::MouseUp, button, 0, RmlWin32Fluvi::GetKeyModifierState()});
        return true;
    }
    case WM_MOUSEWHEEL: {
        if (!wants) return false;
        InputEvent e{InputEvent::Kind::Wheel};
        e.wheel = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
        e.mods = RmlWin32Fluvi::GetKeyModifierState();
        Enqueue(e);
        return true;
    }
    case WM_MOUSELEAVE:
        Enqueue({InputEvent::Kind::Leave});
        return false;
    default: return false;
    }
}

WindowHit UiImpl::HitTest(int x, int y) const {
    std::lock_guard lock(chromeMutex_);
    if (!chromeVisible_ || !chromeRects_.valid) return WindowHit::Client;
    const Rml::Vector2f p(static_cast<float>(x), static_cast<float>(y));
    if (chromeRects_.close.Contains(p)) return WindowHit::Close;
    if (chromeRects_.max.Contains(p)) return WindowHit::Maximize;
    if (chromeRects_.min.Contains(p)) return WindowHit::Minimize;
    if (chromeRects_.bar.Contains(p)) return WindowHit::Caption;
    return WindowHit::Client;
}

void UiImpl::Replay(const InputEvent& e) {
    using K = InputEvent::Kind;
    const bool press = e.kind == K::KeyDown || e.kind == K::MouseDown || (e.kind == K::Hotkey && static_cast<Hotkey>(e.a) == Hotkey::Back);
    if (press && shown_ == Screen::Splash) splashSkip_ = true;
    if (e.kind == K::KeyDown || e.kind == K::Hotkey || e.kind == K::MouseDown) {
        const bool pad = e.kind != K::MouseDown && e.pad;
        if (pad != padInput_) {
            padInput_ = pad;
            SetDocClass("pad", pad);
        }
    }
    switch (e.kind) {
    case K::MouseMove: pointerInput_ = true; context_->ProcessMouseMove(e.a, e.b, e.mods); break;
    case K::MouseDown: context_->ProcessMouseButtonDown(e.a, e.mods); break;
    case K::MouseUp: context_->ProcessMouseButtonUp(e.a, e.mods); break;
    case K::Wheel: context_->ProcessMouseWheel(Rml::Vector2f(0.0f, e.wheel), e.mods); break;
    case K::KeyDown: pointerInput_ = false; context_->ProcessKeyDown(static_cast<Rml::Input::KeyIdentifier>(e.a), e.mods); break;
    case K::KeyUp: context_->ProcessKeyUp(static_cast<Rml::Input::KeyIdentifier>(e.a), e.mods); break;
    case K::Char: context_->ProcessTextInput(static_cast<Rml::Character>(e.a)); break;
    case K::Leave: context_->ProcessMouseLeave(); break;
    case K::Hotkey: OnHotkey(static_cast<Hotkey>(e.a)); break;
    }
}

void UiImpl::OnHotkey(Hotkey key) {
    const Screen screen = screen_.load();
    switch (key) {
    case Hotkey::ToggleOverlay:
        if (screen == Screen::Stream) overlay_.store(!overlay_.load());
        break;
    case Hotkey::ToggleMode:
        SetMode(mode_ == StreamMode::Smooth ? StreamMode::Play : StreamMode::Smooth);
        break;
    case Hotkey::CycleHud:
        CycleHud();
        break;
    case Hotkey::Reload:
        ReloadDocuments();
        break;
    case Hotkey::Back:
        pointerInput_ = false;
        // Innermost first: the dialog, the account menu, then the screen.
        if (ui_.confirm_quit) {
            ui_.confirm_quit = false;
            dialog_.store(false);
            model_.DirtyVariable("ui");
            break;
        }
        if (cons_.menu) {
            cons_.menu = false;
            model_.DirtyVariable("cons");
            wantFocus_ = "account-chip";
            break;
        }
        if (screen == Screen::Settings) {
            // From a row, back to its section in the nav; from the nav, out of settings.
            if (Inside(context_->GetFocusElement(), "list")) {
                wantFocus_ = kNavIds[static_cast<size_t>(std::clamp(set_.section, 0, 6))];
            } else {
                screen_.store(returnTo_.load());
                transition_.store(true);
            }
        } else if (screen == Screen::Stream && overlay_.load()) {
            overlay_.store(false);
        } else if (screen == Screen::Connecting && cb_.cancelConnect) {
            cb_.cancelConnect();
        }
        break;
    }
}

void UiImpl::KeepFocus(bool changed) {
    Rml::ElementDocument* doc = Doc(shown_);
    if (!doc || !doc->IsVisible()) return;
    if (!wantFocus_.empty()) {
        // Requested targets exist by now: the data views were updated by the context update just before.
        if (Rml::Element* e = doc->GetElementById(wantFocus_)) e->Focus(true);
        wantFocus_.clear();
        return;
    }
    // Keep something focused for D-pad/keyboard use: autofocus targets often appear only after data arrives (data-if,
    // data-for), after the document was shown.
    if (pointerInput_ || !changed) return;
    Rml::Element* focus = context_->GetFocusElement();
    if (focus && focus != doc && focus->GetOwnerDocument() == doc && focus->IsVisible(true)) return;
    // The console shelf starts on the console played last; elsewhere the first [autofocus] that is shown (several
    // screens mark one per state and hide the others with data-if).
    if (shown_ == Screen::Consoles && cons_.list.size() > 1) {
        size_t index = 0;
        for (size_t i = 0; i < consoles_.size(); ++i)
            if (consoles_[i].id == lastPlayedId_) index = i;
        if (Rml::Element* tile = doc->GetElementById(std::format("console-{}", index)); tile && tile->Focus(true)) return;
    }
    Rml::ElementList candidates;
    doc->QuerySelectorAll(candidates, "[autofocus]");
    for (Rml::Element* e : candidates)
        if (e->IsVisible(true) && e->Focus(true)) return;
}

void UiImpl::CacheChrome() {
    ChromeRects rects;
    Rml::ElementDocument* doc = phase_ == Phase::ToBed && leaving_ ? Doc(*leaving_) : Doc(shown_);
    if (doc && chrome_.visible)
        if (Rml::Element* bar = doc->GetElementById("chrome-bar")) {
            rects.valid = true;
            rects.bar = BorderRect(bar);
            rects.min = BorderRect(doc->GetElementById("chrome-min"));
            rects.max = BorderRect(doc->GetElementById("chrome-max"));
            rects.close = BorderRect(doc->GetElementById("chrome-close"));
        }
    std::lock_guard lock(chromeMutex_);
    chromeRects_ = rects;
}

} // namespace fluvi::ui
