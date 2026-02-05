#pragma once
#include "AudioRecorder.h"
#include "WhisperEngine.h"
#include "ModelManager.h"
#include "InputManager.h"
#include "Logger.h"
#include <SDL.h>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <memory>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

class Gui {
public:
    Gui(AudioRecorder& recorder, WhisperEngine& whisper, ModelManager& models, InputManager& input);
    ~Gui();

    void render(SDL_Window* window);
    void updateLogic(SDL_Window* window); // Called every frame to handle state updates

    void toggleWindowVisibility(SDL_Window* window);
    bool isWindowHidden() const { return isHidden_; }
    void cleanup(); // Cleanup temp recordings and resources
    
    void initTrayIcon(SDL_Window* window);
    void removeTrayIcon();
    void showFromTray(SDL_Window* window);
    void showTrayContextMenu(SDL_Window* window);
    HWND getHwnd(SDL_Window* window);

private:
    AudioRecorder& recorder_;
    WhisperEngine& whisper_;
    ModelManager& models_;
    InputManager& input_;
    
    // Settings
    struct Settings {
        int selectedModel = -1;
        int selectedDevice = 0;
        bool autoPaste = false;
        bool autoTranscribe = true;
        bool showTimestamps = true;
        bool pushToTalk = false;
        unsigned long hotkeySym = 0; // 0 means default/unchanged
        bool liveTranscription = false; // Live transcription mode
        float silenceThreshold = 0.02f; // Silence detection threshold for live mode
        float silenceDuration = 1.5f;   // Seconds of silence before auto-transcribe
        float noiseFloor = 0.005f;      // Minimum amplitude to consider as speech
        // Whisper-specific settings
        std::string language = "en";    // Language code (en, es, fr, etc., or "auto" for auto-detect)
        bool translate = false;          // Translate to English
        bool printTimestamps = false;    // Print timestamps in transcription
        bool speakerDiarization = false; // Enable speaker identification
        // Speaker diarization model selection
        std::string selectedSegmentationModel;  // Name of selected segmentation model
        std::string selectedEmbeddingModel;     // Name of selected embedding model
    } settings_;
    
    // Pending settings changes (applied after transcription completes)
    struct PendingSettings {
        bool hasPendingModel = false;
        int pendingModel = -1;
        bool hasPendingLanguage = false;
        std::string pendingLanguage;
        bool hasPendingTranslate = false;
        bool pendingTranslate = false;
        bool hasPendingTimestamps = false;
        bool pendingTimestamps = false;
        bool hasPendingDiarization = false;
        bool pendingDiarization = false;
        bool hasPendingDiarizationModels = false;
        std::string pendingSegmentationModel;
        std::string pendingEmbeddingModel;
        
        void clear() {
            hasPendingModel = false;
            hasPendingLanguage = false;
            hasPendingTranslate = false;
            hasPendingTimestamps = false;
            hasPendingDiarization = false;
            hasPendingDiarizationModels = false;
        }
        
        bool hasAny() const {
            return hasPendingModel || hasPendingLanguage || hasPendingTranslate ||
                   hasPendingTimestamps || hasPendingDiarization || hasPendingDiarizationModels;
        }
    } pendingSettings_;
    
    void applyPendingSettings(); // Apply pending settings when safe

    void loadSettings();
    void saveSettings();

    // History
    struct HistoryItem {
        std::string text;
        std::string timestamp;      // Display timestamp (when recorded)
        std::string recordingPath;  // Path to the recording file
    };
    std::vector<HistoryItem> history_;
    void loadHistory();
    void saveHistory();
    void addToHistory(const std::string& text, const std::string& recordingTimestamp, const std::string& recordingPath);
    void exportHistory(const std::string& format); // Export history to file (txt, json, srt)
    
    // Inline editing state
    int editingIndex_ = -1;          // Index of history item being edited (-1 = none)
    std::string editBuffer_;         // Buffer for editing text

    // UI helpers
    void renderControlPanel(SDL_Window* window);
    void renderStatusPanel();
    void renderHistoryPanel();
    void renderSettingsPanel();
    std::string getHotkeyName() const; // Get display name for current hotkey
    std::string openAudioFileDialog();
    std::string openFolderDialog(); // Open folder picker dialog

    // Async transcription with queue
    std::atomic<bool> isTranscribing_{false};
    std::string transcriptionStatus_ = "Idle";
    std::string lastTranscription_;
    
    struct TranscriptionJob {
        std::string audioPath;
        std::string historyLabel;
        bool isLiveSegment = false;
    };
    std::queue<TranscriptionJob> transcriptionQueue_;
    std::mutex queueMutex_;
    void processTranscriptionQueue(); // Process items in the transcription queue

    // Queued actions from other threads
    std::atomic<bool> startRecordingRequest_{false};
    std::atomic<bool> stopRecordingRequest_{false};
    std::atomic<bool> hotkeyPressed_{false};

    std::string currentRecordingPath_;
    std::string currentRecordingTimestamp_; // When recording started
    std::thread transcriptionThread_;
    std::thread downloadThread_;

    bool isHidden_ = false;
    std::vector<std::string> tempRecordings_; // Track temp files to cleanup
    
    // Download time tracking
    std::chrono::steady_clock::time_point downloadStartTime_;
    
    // Live transcription state
    std::chrono::steady_clock::time_point lastSoundTime_;
    bool hadSoundInSegment_ = false;
    int liveSegmentCounter_ = 0;
    std::string liveSessionTimestamp_; // Session timestamp for grouping live segments
    std::string accumulatedLiveText_;  // Accumulated text from live transcription session
    
    NOTIFYICONDATAA nid_{};
    bool trayIconCreated_ = false;
    HICON appIcon_ = NULL; // Application icon for tray
};
