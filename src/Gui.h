#pragma once
#include "AudioRecorder.h"
#include "ModelManager.h"
#include "Util.h"
#include <SDL.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>
#include <chrono>

class WhisperEngine;
class InputManager;

class Gui {
public:
    static constexpr UINT kTrayMessage = WM_APP + 1;

    Gui(SDL_Window* window, AudioRecorder& recorder, WhisperEngine& whisper, ModelManager& models, InputManager& input);
    ~Gui();

    void update(); // per-loop logic; also runs while hidden in the tray
    void render(); // call between ImGui::NewFrame() and ImGui::Render()
    void onTrayMessage(LPARAM event);
    bool isHidden() const { return hidden_; }
    bool isBusy() const; // something on screen is changing, so keep redrawing every frame

private:
    using Model = ModelManager::Model;

    struct Settings { // field names are the settings.json keys
        int selectedModel = -1, selectedDevice = 0;
        bool autoPaste = false, autoTranscribe = true, showTimestamps = true, pushToTalk = false;
        unsigned long hotkeySym = 0; // 0 = default (F9)
        bool liveTranscription = false;
        float silenceThreshold = 0.02f, silenceDuration = 1.5f, noiseFloor = 0.005f;
        std::string language = "en";
        bool translate = false, printTimestamps = false, speakerDiarization = false;
        std::string selectedSegmentationModel, selectedEmbeddingModel;
        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Settings, selectedModel, selectedDevice, autoPaste, autoTranscribe,
                                                    showTimestamps, pushToTalk, hotkeySym, liveTranscription,
                                                    silenceThreshold, silenceDuration, noiseFloor, language, translate,
                                                    printTimestamps, speakerDiarization, selectedSegmentationModel,
                                                    selectedEmbeddingModel)
    };
    struct HistoryItem {
        std::string text, timestamp, recordingPath;
        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(HistoryItem, text, timestamp, recordingPath)
    };
    struct Result { std::string text, label, path; bool live; };

    // Recording and transcription
    void startRecording();
    void stopRecording();
    void toggleRecording();
    void queueRecording(const std::string& path, const AudioRecorder::Stats& stats);
    void enqueue(std::string path, std::string label, bool live);
    void addResult(const Result& r);

    // Models and options
    void selectModel(int index);
    void download(std::vector<const Model*> models);
    void refreshModels();
    void applyOptions();
    void setLive(bool on);
    void setDiarization(bool on);
    bool diarizationReady() const;
    std::string& speakerSelection(ModelManager::Kind kind);
    void notify(std::string message); // status update from a background thread

    // Window and tray
    HWND hwnd() const;
    void show();
    void hide();
    void trayMenu();

    // Persistence
    void saveSettings();
    void saveHistory();
    void exportHistory(const char* ext);

    // Drawing
    void renderControls();
    void renderStatus();
    void renderHistory();
    void renderSettings();
    void renderSpeakerModels(ModelManager::Kind kind, const char* title, const char* comboLabel);
    std::string hotkeyName() const;

    SDL_Window* window_;
    AudioRecorder& recorder_;
    WhisperEngine& whisper_;
    ModelManager& models_;
    InputManager& input_;

    Settings settings_;
    std::vector<HistoryItem> history_;
    std::vector<std::string> devices_, tempRecordings_;
    std::vector<char> whisperAvailable_, speakerAvailable_; // cached file checks, refreshed after downloads
    std::string status_ = "Idle", editBuffer_;
    std::string recordingPath_, recordingStem_, recordingLabel_, liveLabel_;
    int editing_ = -1, liveSegment_ = 0;
    bool hidden_ = false, hadSound_ = false, hotkeyWasHeld_ = false, trayIcon_ = false;
    NOTIFYICONDATAA nid_{};
    HICON icon_ = nullptr;

    // Written by background threads:
    std::mutex mutex_; // guards results_ and asyncStatus_
    std::vector<Result> results_;
    std::string asyncStatus_;
    std::atomic<const Model*> downloading_{nullptr};
    std::atomic<int> downloadedWhisper_{-1};
    std::atomic<bool> modelsChanged_{false};
    std::chrono::steady_clock::time_point downloadStart_;

    // Declared last so they are destroyed (joined) first.
    Worker worker_;     // model loading and transcription, in order
    Worker downloader_; // model downloads
};
