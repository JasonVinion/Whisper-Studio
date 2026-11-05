#include "AudioRecorder.h"
#include "Gui.h"
#include "InputManager.h"
#include "ModelManager.h"
#include "WhisperEngine.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <objbase.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Keep settings, history, logs, models and recordings next to the exe, however it was launched.
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) SetCurrentDirectoryW(std::filesystem::path(exe).parent_path().c_str());
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // for the folder picker

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_ERROR(std::string("SDL_Init failed: ") + SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Whisper Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED) : nullptr;
    if (!renderer) {
        LOG_ERROR(std::string("Could not create window: ") + SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // the layout is fixed; don't write imgui.ini
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE); // tray icon messages

    {
        AudioRecorder recorder;
        WhisperEngine whisper;
        ModelManager models;
        InputManager input;
        Gui gui(window, recorder, whisper, models, input);

        int activeFrames = 0; // keep drawing briefly after input so ImGui can settle hover/animations
        for (bool done = false; !done;) {
            const bool visible = !(SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
            // Idle: sleep until an event arrives (hotkeys post one) instead of redrawing at full speed.
            if (!visible || (activeFrames == 0 && !gui.isBusy())) SDL_WaitEventTimeout(nullptr, gui.isBusy() ? 15 : 250);

            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                activeFrames = 30;
                ImGui_ImplSDL2_ProcessEvent(&e);
                if (e.type == SDL_QUIT ||
                    (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == SDL_GetWindowID(window)))
                    done = true;
                else if (e.type == SDL_SYSWMEVENT && e.syswm.msg->msg.win.msg == Gui::kTrayMessage)
                    gui.onTrayMessage(e.syswm.msg->msg.win.lParam);
            }
            gui.update();
            if (!visible || gui.isHidden()) continue;

            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            gui.render();
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 0x1A, 0x1A, 0x21, 0xFF);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            if (activeFrames > 0) --activeFrames;
        }
    } // subsystems shut down here, before SDL and ImGui

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    CoUninitialize();
    return 0;
}
