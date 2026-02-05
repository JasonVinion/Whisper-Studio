#pragma once
#include <functional>
#include <thread>
#include <atomic>
#include <string>

class InputManager {
public:
    InputManager();
    ~InputManager();

    // Sets the global hotkey (currently supports simple keys like F2-F12, or modifiers)
    // We will use a simple implementation that monitors global input or registers a hotkey.
    void setGlobalHotkey(std::function<void()> callback);
    void setHotkeySym(unsigned long sym);
    unsigned long getHotkeySym() const { return currentHotkeySym_; }

    // Pastes text into the currently focused window
    void autoPaste(const std::string& text);

    void startInternalLoop();
    void stopInternalLoop();
    void startLearning(); // Captures next key
    bool isLearning() const { return learning_; }
    bool isHotkeyHeld() const { return hotkeyHeld_; }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> learning_{false};
    std::atomic<bool> hotkeyHeld_{false};
    std::thread inputThread_;
    std::function<void()> hotkeyCallback_;

    // Windows: F9 default (VK_F9)
    unsigned long currentHotkeySym_ = 0x78;

    void runLoop();
};
