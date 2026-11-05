#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Global hotkey (F1-F24) on a background thread, plus auto-paste.
class InputManager {
public:
    ~InputManager() { stop(); }

    // onEvent runs on the input thread whenever the hotkey is pressed or released (use it to wake the UI).
    void start(std::function<void()> onEvent);
    void stop();
    void setHotkey(unsigned vk);
    unsigned hotkey() const { return hotkey_; }

    void startLearning() { learning_ = true; } // the next F-key pressed becomes the hotkey
    bool isLearning() const { return learning_; }
    bool isHotkeyHeld() const { return held_; }
    bool consumePress() { return pressed_.exchange(false); } // true once per hotkey press

    // Puts UTF-8 text on the clipboard and sends Ctrl+V to the focused window. Call from the main thread.
    static void paste(const std::string& text);

private:
    void run();

    std::thread thread_;
    std::function<void()> onEvent_;
    std::atomic<bool> running_{false}, learning_{false}, held_{false}, pressed_{false};
    std::atomic<unsigned> hotkey_{0x78}; // VK_F9
};
