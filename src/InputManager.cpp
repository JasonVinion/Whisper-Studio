#include "InputManager.h"
#include "Util.h"
#include <SDL.h>

void InputManager::start(std::function<void()> onEvent) {
    if (running_) return;
    onEvent_ = std::move(onEvent);
    running_ = true;
    thread_ = std::thread(&InputManager::run, this);
}

void InputManager::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void InputManager::setHotkey(unsigned vk) {
    const bool restart = running_;
    stop();
    hotkey_ = vk;
    if (restart) start(onEvent_);
}

// RegisterHotKey delivers WM_HOTKEY to the thread that registered it, so registration and the message
// loop both live here. The key state is polled for push-to-talk.
void InputManager::run() {
    unsigned vk = hotkey_;
    auto reg = [&] {
        UnregisterHotKey(nullptr, 1);
        if (!RegisterHotKey(nullptr, 1, MOD_NOREPEAT, vk)) LOG_ERROR("Failed to register hotkey " + std::to_string(vk));
    };
    reg();
    while (running_) {
        bool wake = false;
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
            if (msg.message == WM_HOTKEY && !learning_) pressed_ = wake = true;

        if (learning_) {
            for (unsigned k = VK_F1; k <= VK_F24; ++k) {
                if (!(GetAsyncKeyState(k) & 0x8000)) continue;
                while (running_ && (GetAsyncKeyState(k) & 0x8000)) Sleep(10); // wait for release
                hotkey_ = vk = k;
                reg();
                learning_ = false;
                break;
            }
        }
        const bool held = !learning_ && (GetAsyncKeyState(vk) & 0x8000);
        if (held != held_) {
            held_ = held;
            wake = true;
        }
        if (wake && onEvent_) onEvent_();
        Sleep(10);
    }
    UnregisterHotKey(nullptr, 1);
}

void InputManager::paste(const std::string& text) {
    // SDL converts UTF-8 to CF_UNICODETEXT and owns the clipboard through its window.
    if (text.empty() || SDL_SetClipboardText(text.c_str()) != 0) return;
    INPUT in[4] = {};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[3].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = in[2].ki.wVk = 'V';
    in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}
