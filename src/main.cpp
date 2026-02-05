#include "Gui.h"
#include "AudioRecorder.h"
#include "WhisperEngine.h"
#include "ModelManager.h"
#include "InputManager.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <chrono>

// Helper function to set window icon
static void SetWindowIcon(SDL_Window* window) {
    // Get the native Windows handle
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        HWND hwnd = wmInfo.info.win.window;
        
        // Try to load from resource ID 1 or string "IDI_ICON1"
        HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
        if (!hIcon) {
            hIcon = LoadIcon(GetModuleHandle(NULL), "IDI_ICON1");
        }
        
        // Fallback to file from resources folder
        if (!hIcon) {
            hIcon = (HICON)LoadImageA(NULL, "resources/app.ico", IMAGE_ICON,
                                         0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        }
        if (!hIcon) {
            // Fallback to app.ico in current directory
            hIcon = (HICON)LoadImageA(NULL, "app.ico", IMAGE_ICON,
                                       0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        }
        
        if (hIcon) {
            // Set both small and big icons
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        }
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Hide console window on Windows (already handled by WIN32 subsystem)
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    int argc = 0;
    char** argv = nullptr;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    // Window flags
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("Whisper Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);

    if (window == nullptr) {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    // Setup SDL_Renderer instance
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr) {
        SDL_Log("Error creating SDL_Renderer!");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    
    // Set window icon
    SetWindowIcon(window);

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking

    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

// Enable system window message events for tray icon support
SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);

// Initialize subsystems
    AudioRecorder recorder;
    WhisperEngine whisper;
    ModelManager models;
    InputManager input;

    Gui gui(recorder, whisper, models, input);

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                done = true;
            // Handle tray icon messages
            if (event.type == SDL_SYSWMEVENT) {
                SDL_SysWMmsg* wmMsg = event.syswm.msg;
                if (wmMsg->msg.win.msg == WM_USER + 1) {
                    // Tray icon clicked
                    if (LOWORD(wmMsg->msg.win.lParam) == WM_LBUTTONUP ||
                        LOWORD(wmMsg->msg.win.lParam) == WM_LBUTTONDBLCLK) {
                        gui.showFromTray(window);
                    } else if (LOWORD(wmMsg->msg.win.lParam) == WM_RBUTTONUP) {
                        // Right-click - show context menu
                        gui.showTrayContextMenu(window);
                    }
                }
            }
        }

        gui.updateLogic(window);

        if (!gui.isWindowHidden()) {
            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();

            gui.render(window);

            ImGui::Render();
            // Use dark background matching the theme
            SDL_SetRenderDrawColor(renderer, 0x1A, 0x1A, 0x21, 0xFF);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // Cleanup temp recordings before exit
    gui.cleanup();

    // Cleanup
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
