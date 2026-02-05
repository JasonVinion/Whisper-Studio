#include "InputManager.h"
#include <iostream>
#include <chrono>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

InputManager::InputManager() {
}

InputManager::~InputManager() {
    stopInternalLoop();
}

void InputManager::setGlobalHotkey(std::function<void()> callback) {
    hotkeyCallback_ = callback;
}

void InputManager::setHotkeySym(unsigned long sym) {
    // Restart loop to apply new grab
    stopInternalLoop();
    currentHotkeySym_ = sym;
    startInternalLoop();
}

void InputManager::startLearning() {
    learning_ = true;
}

void InputManager::startInternalLoop() {
    if (running_) return;
    running_ = true;
    inputThread_ = std::thread(&InputManager::runLoop, this);
}

void InputManager::stopInternalLoop() {
    running_ = false;
    if (inputThread_.joinable()) {
        inputThread_.join();
    }
}

void InputManager::runLoop() {
    // Register hotkey (F9 = VK_F9 = 0x78)
    // Convert XK_ keysym to Windows VK if needed
    UINT vk = static_cast<UINT>(currentHotkeySym_);
    
    // Register the hotkey
    if (!RegisterHotKey(NULL, 1, 0, vk)) {
        std::cerr << "InputManager: Failed to register hotkey" << std::endl;
    }

    MSG msg;
    while (running_) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) {
                if (learning_) {
                    // In learning mode, we'd capture the key
                    learning_ = false;
                } else {
                    if (hotkeyCallback_) {
                        hotkeyCallback_();
                    }
                }
            }
        } else {
            // Check for learning mode - poll keyboard state
            if (learning_) {
                for (int vkCode = VK_F1; vkCode <= VK_F24; ++vkCode) {
                    if (GetAsyncKeyState(vkCode) & 0x8000) {
                        currentHotkeySym_ = vkCode;
                        learning_ = false;
                        // Re-register with new key
                        UnregisterHotKey(NULL, 1);
                        RegisterHotKey(NULL, 1, 0, vkCode);
                        break;
                    }
                }
                // Also check letter keys and number keys
                for (int vkCode = 'A'; vkCode <= 'Z'; ++vkCode) {
                    if (GetAsyncKeyState(vkCode) & 0x8000) {
                        currentHotkeySym_ = vkCode;
                        learning_ = false;
                        UnregisterHotKey(NULL, 1);
                        RegisterHotKey(NULL, 1, 0, vkCode);
                        break;
                    }
                }
            }
            
            // Track if hotkey is currently held (for push-to-talk)
            hotkeyHeld_ = (GetAsyncKeyState(static_cast<int>(currentHotkeySym_)) & 0x8000) != 0;
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    UnregisterHotKey(NULL, 1);
}

void InputManager::autoPaste(const std::string& text) {
    // Use Windows clipboard and SendInput to simulate Ctrl+V
    if (text.empty()) return;

    // Set clipboard text
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (hMem) {
        char* pMem = static_cast<char*>(GlobalLock(hMem));
        if (pMem) {
            memcpy(pMem, text.c_str(), text.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
    }
    CloseClipboard();

    // Simulate Ctrl+V
    INPUT inputs[4] = {};
    
    // Press Ctrl
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    
    // Press V
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    
    // Release V
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    
    // Release Ctrl
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(4, inputs, sizeof(INPUT));
}
