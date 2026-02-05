#include "Gui.h"
#include "imgui.h"
#include "Logger.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <filesystem>
#include <array>
#include <cctype>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Global state for async results
static std::mutex g_resultMutex;
static std::string g_pendingResult;
static std::string g_pendingHistoryLabel;
static std::string g_pendingPath;
static bool g_hasResult = false;
static bool g_pendingIsLiveSegment = false;

Gui::Gui(AudioRecorder& recorder, WhisperEngine& whisper, ModelManager& models, InputManager& input)
: recorder_(recorder), whisper_(whisper), models_(models), input_(input) {

// Professional Dark Theme
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.26f, 0.35f, 1.00f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.35f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);

    loadHistory();
    loadSettings();
    
    // Initialize live transcription state
    lastSoundTime_ = std::chrono::steady_clock::now();

    input_.setGlobalHotkey([this]() {
         hotkeyPressed_ = true;
    });

    if (settings_.hotkeySym != 0) {
        input_.setHotkeySym(settings_.hotkeySym);
    }

    input_.startInternalLoop();
}

Gui::~Gui() {
    input_.stopInternalLoop();
    saveHistory();
    saveSettings();
    cleanup(); // Cleanup temp recordings
    if (transcriptionThread_.joinable()) transcriptionThread_.join();
    if (downloadThread_.joinable()) downloadThread_.join();
    removeTrayIcon();
}

std::string Gui::openAudioFileDialog() {
    std::array<char, MAX_PATH> buffer{};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = "Audio Files\0*.wav;*.mp3;*.m4a;*.flac;*.ogg;*.opus;*.aac;*.wma;*.aiff;*.aif;*.aifc\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        return buffer.data();
    }
    return {};
}

std::string Gui::openFolderDialog() {
    std::array<char, MAX_PATH> buffer{};
    BROWSEINFOA bi{};
    bi.lpszTitle = "Select Folder with Audio Files";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != nullptr) {
        if (SHGetPathFromIDListA(pidl, buffer.data())) {
            CoTaskMemFree(pidl);
            return buffer.data();
        }
        CoTaskMemFree(pidl);
    }
    return {};
}

void Gui::exportHistory(const std::string& format) {
    if (history_.empty()) {
        transcriptionStatus_ = "No history to export.";
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y%m%d_%H%M%S");
    std::string timestamp = ss.str();
    
    if (format == "txt") {
        std::string filename = "transcription_history_" + timestamp + ".txt";
        std::ofstream file(filename);
        if (file.is_open()) {
            for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
                if (settings_.showTimestamps) {
                    file << "[" << it->timestamp << "]" << std::endl;
                }
                file << it->text << std::endl << std::endl;
            }
            file.flush();
            if (file.good()) {
                transcriptionStatus_ = "Exported to " + filename;
            } else {
                transcriptionStatus_ = "Failed to write to " + filename;
            }
            file.close();
        } else {
            transcriptionStatus_ = "Failed to create " + filename;
        }
    } else if (format == "json") {
        std::string filename = "transcription_history_" + timestamp + ".json";
        std::ofstream file(filename);
        if (file.is_open()) {
            json j = json::array();
            for (const auto& item : history_) {
                j.push_back({
                    {"text", item.text}, 
                    {"timestamp", item.timestamp},
                    {"recordingPath", item.recordingPath}
                });
            }
            file << j.dump(4);
            file.flush();
            if (file.good()) {
                transcriptionStatus_ = "Exported to " + filename;
            } else {
                transcriptionStatus_ = "Failed to write to " + filename;
            }
            file.close();
        } else {
            transcriptionStatus_ = "Failed to create " + filename;
        }
    } else if (format == "srt") {
        std::string filename = "transcription_history_" + timestamp + ".srt";
        std::ofstream file(filename);
        if (file.is_open()) {
            int idx = 1;
            for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
                file << idx++ << std::endl;
                // Note: SRT format uses placeholder timestamps. Enable "Include Timestamps in Transcription" for accurate timing.
                file << "00:00:00,000 --> 00:00:05,000" << std::endl;
                file << it->text << std::endl << std::endl;
            }
            file.flush();
            if (file.good()) {
                transcriptionStatus_ = "Exported to " + filename;
            } else {
                transcriptionStatus_ = "Failed to write to " + filename;
            }
            file.close();
        } else {
            transcriptionStatus_ = "Failed to create " + filename;
        }
    }
}

void Gui::render(SDL_Window* window) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::Begin("Whisper Studio", nullptr, window_flags);
    ImGui::PopStyleVar();
    
    // Check if model is loaded/selected and show warning if not
    bool modelReady = whisper_.isModelLoaded();
    if (!modelReady) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
        ImGui::TextWrapped("Please download and select a model under Settings before using transcription.");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // Begin scrollable content area for the entire window content
    ImGui::BeginChild("MainScrollArea", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // Top controls section - hotkey rebind, push-to-talk, auto-transcribe
    std::string hotkeyName = getHotkeyName();
    if (input_.isLearning()) {
        ImGui::Button("Press any key to bind...", ImVec2(200, 0));
    } else {
        std::string rebindLabel = "Rebind Hotkey (" + hotkeyName + ")";
        if (ImGui::Button(rebindLabel.c_str(), ImVec2(200, 0))) {
            input_.stopInternalLoop();
            input_.startLearning();
            input_.startInternalLoop();
        }
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Push-to-Talk", &settings_.pushToTalk);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, hold the hotkey or click and hold the record button to record.");
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Auto-Transcribe", &settings_.autoTranscribe);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically transcribe after recording stops.");
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Minimize to Tray")) {
        toggleWindowVisibility(window);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Minimize to system tray. Use the hotkey or tray icon to restore.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderControlPanel(window);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderStatusPanel();

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transcription History")) {
        renderHistoryPanel();
    }

    if (ImGui::CollapsingHeader("Settings")) {
        renderSettingsPanel();
    }
    
    ImGui::EndChild(); // End MainScrollArea

    ImGui::End();
}

void Gui::updateLogic(SDL_Window* window) {
    // Handle Hotkey for push-to-talk mode
    static bool hotkeyHeldPrevFrame = false;
    bool hotkeyHeldThisFrame = input_.isHotkeyHeld();
    
    if (settings_.pushToTalk) {
        // Push-to-talk with hotkey - works even when hidden
        if (hotkeyHeldThisFrame && !hotkeyHeldPrevFrame) {
            // Hotkey just pressed
            if (!recorder_.isRecording()) {
                startRecordingRequest_ = true;
            }
        } else if (!hotkeyHeldThisFrame && hotkeyHeldPrevFrame) {
            // Hotkey just released
            if (recorder_.isRecording()) {
                stopRecordingRequest_ = true;
            }
        }
    }
    hotkeyHeldPrevFrame = hotkeyHeldThisFrame;

    // Handle Hotkey press (toggle recording - works from tray too)
    if (hotkeyPressed_) {
        hotkeyPressed_ = false;
        if (!settings_.pushToTalk) {
            // Toggle recording in non-push-to-talk mode (works even when hidden)
            if (recorder_.isRecording()) {
                stopRecordingRequest_ = true;
            } else {
                startRecordingRequest_ = true;
            }
        }
        // Note: hotkey no longer brings up window from tray - only starts/stops recording
    }

    // Handle async requests
    if (startRecordingRequest_) {
        startRecordingRequest_ = false;
        if (!recorder_.isRecording()) {
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss, tsStream;
            ss << std::put_time(std::localtime(&in_time_t), "%d-%m-%Y_%H-%M-%S");
            tsStream << std::put_time(std::localtime(&in_time_t), "%d-%m-%Y %H:%M:%S");
            currentRecordingPath_ = "recording_" + ss.str() + ".wav";
            currentRecordingTimestamp_ = tsStream.str();
            
            // Initialize live transcription session if enabled
            if (settings_.liveTranscription) {
                liveSessionTimestamp_ = currentRecordingTimestamp_;
                liveSegmentCounter_ = 0;
                accumulatedLiveText_.clear();
                hadSoundInSegment_ = false;
                lastSoundTime_ = std::chrono::steady_clock::now();
            }

            if (recorder_.startRecording(settings_.selectedDevice, currentRecordingPath_, false)) {
                tempRecordings_.push_back(currentRecordingPath_);
                transcriptionStatus_ = "Recording...";
                LOG_INFO("Recording started: " + currentRecordingPath_);
            } else {
                transcriptionStatus_ = "Error: Could not start recording.";
                LOG_ERROR("Failed to start recording");
            }
        }
    }
    
    // Live transcription: check for silence and auto-transcribe segments
    if (settings_.liveTranscription && recorder_.isRecording()) {
        float amplitude = recorder_.getRecentPeakAmplitude();
        
        // Track if we've had meaningful sound in this segment
        if (amplitude > settings_.noiseFloor) {
            hadSoundInSegment_ = true;
            lastSoundTime_ = std::chrono::steady_clock::now();
        }
        
        // Check silence duration
        float silenceDuration = recorder_.getSilenceDuration(settings_.silenceThreshold);
        
        if (silenceDuration >= settings_.silenceDuration && hadSoundInSegment_) {
            // We have silence after speech - transcribe this segment
            std::string segmentPath = currentRecordingPath_;
            
            // Create new file for next segment
            liveSegmentCounter_++;
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::localtime(&in_time_t), "%d-%m-%Y_%H-%M-%S");
            std::string newPath = "recording_" + ss.str() + "_seg" + std::to_string(liveSegmentCounter_) + ".wav";
            
            if (recorder_.resetToNewFile(newPath)) {
                currentRecordingPath_ = newPath;
                tempRecordings_.push_back(newPath);
                hadSoundInSegment_ = false;
                
                // Queue the previous segment for transcription (with noise check)
                if (!AudioRecorder::isAudioSilent(segmentPath, settings_.noiseFloor)) {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    transcriptionQueue_.push({segmentPath, liveSessionTimestamp_, true});
                    
                    // Start processing if not already
                    if (!isTranscribing_) {
                        transcriptionStatus_ = "Live: Transcribing segment...";
                        isTranscribing_ = true;
                        
                        if (transcriptionThread_.joinable()) transcriptionThread_.join();
                        transcriptionThread_ = std::thread([this]() {
                            processTranscriptionQueue();
                        });
                    }
                }
            }
        }
    }

    if (stopRecordingRequest_) {
        stopRecordingRequest_ = false;
        if (recorder_.isRecording()) {
            recorder_.stopRecording();
            LOG_INFO("Recording stopped: " + currentRecordingPath_);

            if (settings_.autoTranscribe) {
                // Check if audio is silent (noise filtering)
                bool isSilent = AudioRecorder::isAudioSilent(currentRecordingPath_, settings_.noiseFloor);
                
                if (isSilent) {
                    transcriptionStatus_ = "Skipped: Audio was silent.";
                    LOG_INFO("Skipped transcription - audio was silent");
                } else {
                    // Add to queue
                    {
                        std::lock_guard<std::mutex> lock(queueMutex_);
                        bool isLiveSegment = settings_.liveTranscription && !liveSessionTimestamp_.empty();
                        std::string label = isLiveSegment ? liveSessionTimestamp_ : currentRecordingTimestamp_;
                        transcriptionQueue_.push({currentRecordingPath_, label, isLiveSegment});
                        LOG_INFO("Queued for transcription: " + currentRecordingPath_);
                    }
                    
                    // Start processing if not already
                    if (!isTranscribing_) {
                        transcriptionStatus_ = "Transcribing...";
                        isTranscribing_ = true;
                        
                        if (transcriptionThread_.joinable()) transcriptionThread_.join();
                        transcriptionThread_ = std::thread([this]() {
                            processTranscriptionQueue();
                        });
                    } else {
                        transcriptionStatus_ = "Queued for transcription...";
                    }
                }
            } else {
                transcriptionStatus_ = "Recording Saved. Ready to transcribe.";
            }
            
            // For live transcription, finalize the session by adding accumulated text to history
            if (settings_.liveTranscription && !accumulatedLiveText_.empty()) {
                // The accumulated text will be added as results come in
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        if (g_hasResult) {
            // For live transcription, accumulate text into a single history entry
            if (g_pendingIsLiveSegment) {
                bool found = false;
                for (auto& item : history_) {
                    if (item.timestamp == g_pendingHistoryLabel) {
                        if (!item.text.empty() && !g_pendingResult.empty()) {
                            char lastChar = item.text.back();
                            if (!std::isspace(static_cast<unsigned char>(lastChar)) &&
                                !std::isspace(static_cast<unsigned char>(g_pendingResult.front()))) {
                                item.text += " ";
                            }
                        }
                        item.text += g_pendingResult;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    addToHistory(g_pendingResult, g_pendingHistoryLabel, g_pendingPath);
                } else {
                    saveHistory();
                }
            } else {
                addToHistory(g_pendingResult, g_pendingHistoryLabel, g_pendingPath);
            }
            
            if (settings_.autoPaste && g_pendingResult.find("Error:") == std::string::npos) {
                input_.autoPaste(g_pendingResult);
            }
            g_pendingResult.clear();
            g_pendingHistoryLabel.clear();
            g_pendingPath.clear();
            g_hasResult = false;
            g_pendingIsLiveSegment = false;
            
            // Check if more items in queue
            {
                std::lock_guard<std::mutex> qlock(queueMutex_);
                if (transcriptionQueue_.empty()) {
                    isTranscribing_ = false;
                    transcriptionStatus_ = recorder_.isRecording() ? "Recording..." : "Idle";
                    // Apply any pending settings now that transcription is done
                    applyPendingSettings();
                }
            }
        }
    }
}

void Gui::toggleWindowVisibility(SDL_Window* window) {
    if (isHidden_) {
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
        isHidden_ = false;
        removeTrayIcon();
    } else {
        SDL_HideWindow(window);
        isHidden_ = true;
        initTrayIcon(window);
    }
}

void Gui::renderControlPanel(SDL_Window* window) {
    float amplitude = recorder_.getAmplitude();
    ImGui::Text("Microphone Level:");
    ImGui::ProgressBar(amplitude * 5.0f, ImVec2(-1, 0));

    ImGui::Spacing();

    bool isRecording = recorder_.isRecording();
    std::string hotkeyName = getHotkeyName();
    std::string btnLabel = isRecording ? 
        ("Stop Recording (" + hotkeyName + ")") : 
        ("Start Recording (" + hotkeyName + ")");

    if (isRecording) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    }

    bool btnClicked = ImGui::Button(btnLabel.c_str(), ImVec2(-1, 60));
    bool btnHeld = ImGui::IsItemActive();
    ImGui::PopStyleColor(2);

    if (settings_.pushToTalk) {
        // Push-to-talk with mouse button on the UI button
        if (btnHeld && !isRecording) startRecordingRequest_ = true;
        else if (!btnHeld && isRecording && !input_.isHotkeyHeld()) stopRecordingRequest_ = true;
    } else {
        if (btnClicked) {
            if (isRecording) stopRecordingRequest_ = true;
            else startRecordingRequest_ = true;
        }
    }

    ImGui::Spacing();

    if (ImGui::Button("Transcribe Audio File...", ImVec2(-1, 0))) {
        std::string selectedPath = openAudioFileDialog();
        if (!selectedPath.empty()) {
            std::string label = fs::path(selectedPath).filename().string();
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                transcriptionQueue_.push({selectedPath, label, false});
            }

            if (!isTranscribing_) {
                transcriptionStatus_ = "Transcribing file...";
                isTranscribing_ = true;

                if (transcriptionThread_.joinable()) transcriptionThread_.join();
                transcriptionThread_ = std::thread([this]() {
                    processTranscriptionQueue();
                });
            } else {
                transcriptionStatus_ = "Queued file for transcription...";
            }
        }
    }

    if (ImGui::Button("Transcribe Folder...", ImVec2(-1, 0))) {
        std::string selectedFolder = openFolderDialog();
        if (!selectedFolder.empty()) {
            if (!fs::exists(selectedFolder) || !fs::is_directory(selectedFolder)) {
                transcriptionStatus_ = "Selected path is not a valid directory.";
            } else {
                // Queue all audio files in the folder
                int fileCount = 0;
                const std::vector<std::string> audioExtensions = {".wav", ".mp3", ".m4a", ".flac", ".ogg", ".opus", ".aac", ".wma", ".aiff", ".aif", ".aifc"};
                
                try {
                    for (const auto& entry : fs::directory_iterator(selectedFolder)) {
                        if (entry.is_regular_file()) {
                            std::string ext = entry.path().extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            
                            if (std::find(audioExtensions.begin(), audioExtensions.end(), ext) != audioExtensions.end()) {
                                std::string label = entry.path().filename().string();
                                {
                                    std::lock_guard<std::mutex> lock(queueMutex_);
                                    transcriptionQueue_.push({entry.path().string(), label, false});
                                }
                                fileCount++;
                            }
                        }
                    }
                    
                    if (fileCount > 0) {
                        transcriptionStatus_ = "Queued " + std::to_string(fileCount) + " files for transcription...";
                        
                        if (!isTranscribing_) {
                            isTranscribing_ = true;
                            if (transcriptionThread_.joinable()) transcriptionThread_.join();
                            transcriptionThread_ = std::thread([this]() {
                                processTranscriptionQueue();
                            });
                        }
                    } else {
                        transcriptionStatus_ = "No audio files found in folder.";
                    }
                } catch (const std::exception& e) {
                    transcriptionStatus_ = std::string("Error reading folder: ") + e.what();
                }
            }
        }
    }

    ImGui::Spacing();

    if (ImGui::Button("Open Recordings & Transcriptions Folder", ImVec2(-1, 0))) {
        ShellExecuteA(NULL, "open", ".", NULL, NULL, SW_SHOWDEFAULT);
    }

    if (ImGui::Button("Delete Temp Recordings & Transcriptions", ImVec2(-1, 0))) {
        // Delete recording files
        for (const auto& entry : fs::directory_iterator(".")) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                // Delete temp recordings
                if (filename.find("recording_") == 0 && filename.find(".wav") != std::string::npos) {
                    try {
                        fs::remove(entry.path());
                    } catch (...) {}
                }
                // Delete transcription export files
                if (filename.find("transcription_history_") == 0) {
                    if (filename.find(".txt") != std::string::npos ||
                        filename.find(".json") != std::string::npos ||
                        filename.find(".srt") != std::string::npos) {
                        try {
                            fs::remove(entry.path());
                        } catch (...) {}
                    }
                }
            }
        }
        transcriptionStatus_ = "Deleted temp recordings and transcription files.";
    }
}

void Gui::renderStatusPanel() {
    ImGui::Text("State: %s", transcriptionStatus_.c_str());
    
    // Show CUDA/GPU status
    ImGui::SameLine();
    ImGui::Text(" | ");
    ImGui::SameLine();
#if WHISPERGUI_CUDA_ENABLED
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
    ImGui::Text("GPU: CUDA Enabled");
    ImGui::PopStyleColor();
#else
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.2f, 1.0f));
    ImGui::Text("GPU: CPU Only");
    ImGui::PopStyleColor();
#endif

    if (isTranscribing_) {
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime() * 0.2f, ImVec2(-1, 0.0f), "Processing...");
    }

    {
        auto& progress = models_.getDownloadProgress();
        if (progress.isDownloading) {
            ImGui::Separator();
            ImGui::Text("Downloading: %s", progress.currentModel.c_str());
            
            // Calculate elapsed time
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - progress.startTime).count();
            int elapsedMin = static_cast<int>(elapsed / 60);
            int elapsedSec = static_cast<int>(elapsed % 60);
            
            // Calculate progress and ETA
            double downloaded = progress.bytesDownloaded.load();
            double total = progress.totalBytes.load();
            double speed = progress.downloadSpeed.load();
            
            float progressFrac = 0.0f;
            if (total > 0) {
                progressFrac = static_cast<float>(downloaded / total);
            }
            
            // Format sizes
            char downloadedStr[32], totalStr[32], speedStr[32];
            if (downloaded >= 1024 * 1024) {
                snprintf(downloadedStr, sizeof(downloadedStr), "%.1f MB", downloaded / (1024 * 1024));
            } else {
                snprintf(downloadedStr, sizeof(downloadedStr), "%.0f KB", downloaded / 1024);
            }
            if (total >= 1024 * 1024) {
                snprintf(totalStr, sizeof(totalStr), "%.1f MB", total / (1024 * 1024));
            } else if (total > 0) {
                snprintf(totalStr, sizeof(totalStr), "%.0f KB", total / 1024);
            } else {
                snprintf(totalStr, sizeof(totalStr), "???");
            }
            if (speed >= 1024 * 1024) {
                snprintf(speedStr, sizeof(speedStr), "%.1f MB/s", speed / (1024 * 1024));
            } else if (speed > 0) {
                snprintf(speedStr, sizeof(speedStr), "%.0f KB/s", speed / 1024);
            } else {
                snprintf(speedStr, sizeof(speedStr), "calculating...");
            }
            
            // Calculate ETA
            std::string etaStr = "calculating...";
            if (speed > 0 && total > 0) {
                double remaining = total - downloaded;
                int etaSec = static_cast<int>(remaining / speed);
                int etaMin = etaSec / 60;
                etaSec = etaSec % 60;
                char etaBuf[32];
                snprintf(etaBuf, sizeof(etaBuf), "%d:%02d", etaMin, etaSec);
                etaStr = etaBuf;
            }
            
            // Show detailed progress
            ImGui::Text("Elapsed: %d:%02d | ETA: %s | Speed: %s", elapsedMin, elapsedSec, etaStr.c_str(), speedStr);
            
            // Progress bar with size info
            char overlayText[128];
            snprintf(overlayText, sizeof(overlayText), "%s / %s", downloadedStr, totalStr);
            
            if (total > 0) {
                ImGui::ProgressBar(progressFrac, ImVec2(-1, 0.0f), overlayText);
            } else {
                ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime() * 0.3f, ImVec2(-1, 0.0f), "Downloading...");
            }
        }
    }
}

void Gui::renderHistoryPanel() {
    if (ImGui::Button("Clear Log")) {
        history_.clear();
        editingIndex_ = -1;  // Reset editing state
        saveHistory();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show Timestamps", &settings_.showTimestamps);
    
    ImGui::SameLine();
    if (ImGui::Button("Export TXT")) {
        exportHistory("txt");
    }
    ImGui::SameLine();
    if (ImGui::Button("Export JSON")) {
        exportHistory("json");
    }
    ImGui::SameLine();
    if (ImGui::Button("Export SRT")) {
        exportHistory("srt");
    }

    ImGui::Separator();

    // Child region with vertical scrollbar (AlwaysVerticalScrollbar flag)
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, 300), true, 
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
    
    // Calculate index from reverse iterator position
    int itemIndex = static_cast<int>(history_.size()) - 1;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it, --itemIndex) {
        if (settings_.showTimestamps) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::Text("[%s]", it->timestamp.c_str());
            ImGui::PopStyleColor();
        }

        // Check if this item is being edited
        if (editingIndex_ == itemIndex) {
            // Editing mode - show multiline input
            ImGui::PushItemWidth(-1);
            
            // Use a unique ID for the input
            std::string inputId = "##edit_" + std::to_string(itemIndex);
            
            // Ensure buffer is properly sized before use
            // Pre-allocate sufficient space for editing (minimum 4KB)
            constexpr size_t kEditBufferSize = 4096;
            if (editBuffer_.size() < kEditBufferSize) {
                editBuffer_.resize(kEditBufferSize, '\0');
            }
            
            // Show multiline text input
            if (ImGui::InputTextMultiline(inputId.c_str(), &editBuffer_[0], editBuffer_.size(),
                ImVec2(-1, 100), ImGuiInputTextFlags_AllowTabInput)) {
                // No need to resize - buffer maintains fixed size during editing
            }
            
            ImGui::PopItemWidth();
            
            // Save and Cancel buttons
            std::string saveId = "Save##" + std::to_string(itemIndex);
            std::string cancelId = "Cancel##" + std::to_string(itemIndex);
            
            if (ImGui::Button(saveId.c_str())) {
                // Bounds check before accessing history
                if (itemIndex >= 0 && itemIndex < static_cast<int>(history_.size())) {
                    // Trim to actual string length (up to null terminator)
                    history_[itemIndex].text = std::string(editBuffer_.c_str());
                    saveHistory();
                }
                editingIndex_ = -1;
                editBuffer_.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button(cancelId.c_str())) {
                editingIndex_ = -1;
                editBuffer_.clear();
            }
        } else {
            // Display mode - show text wrapped
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(it->text.c_str());
            ImGui::PopTextWrapPos();

            // Copy button
            std::string copyId = std::string("Copy##") + it->timestamp + "_" + it->recordingPath;
            if (ImGui::Button(copyId.c_str())) {
                ImGui::SetClipboardText(it->text.c_str());
            }
            
            // Edit button
            ImGui::SameLine();
            std::string editId = std::string("Edit##") + it->timestamp + "_" + it->recordingPath;
            if (ImGui::Button(editId.c_str())) {
                editingIndex_ = itemIndex;
                // Initialize buffer with existing text and pre-allocate space
                constexpr size_t kEditBufferSize = 4096;
                editBuffer_.assign(kEditBufferSize, '\0');
                // Copy text into buffer (safely truncate if too long)
                size_t copyLen = (std::min)(it->text.size(), kEditBufferSize - 1);
                std::copy(it->text.begin(), it->text.begin() + copyLen, editBuffer_.begin());
            }
        }

        ImGui::Separator();
    }
    ImGui::EndChild();
}

void Gui::renderSettingsPanel() {
    if (ImGui::Button("Save Settings")) {
        saveSettings();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Settings are also saved on exit)");

    ImGui::Separator();

    ImGui::Text("Audio Input");
    auto devices = recorder_.getInputDevices();
    if (ImGui::BeginCombo("Device", settings_.selectedDevice >= 0 && settings_.selectedDevice < static_cast<int>(devices.size()) ? devices[settings_.selectedDevice].name.c_str() : "Select Device")) {
        for (size_t i = 0; i < devices.size(); ++i) {
            bool isSelected = (settings_.selectedDevice == static_cast<int>(i));
            if (ImGui::Selectable(devices[i].name.c_str(), isSelected)) {
                settings_.selectedDevice = static_cast<int>(i);
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    ImGui::Text("Whisper Model");
    auto models = models_.getAvailableModels();
    std::string previewValue = (settings_.selectedModel >= 0 && settings_.selectedModel < static_cast<int>(models.size())) ? models[settings_.selectedModel].name : "Select Model";

    // Show warning if transcribing and settings changes will be deferred
    bool transcribingNow = isTranscribing_.load();
    if (transcribingNow && pendingSettings_.hasAny()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.2f, 1.0f));
        ImGui::TextWrapped("Settings will be applied after transcription completes.");
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginCombo("Model", previewValue.c_str())) {
        for (size_t i = 0; i < models.size(); ++i) {
            bool isSelected = (settings_.selectedModel == static_cast<int>(i));
            bool available = models_.isModelAvailable(models[i].name);
            std::string label = models[i].name + (available ? "" : " [Click to Download]");

            if (ImGui::Selectable(label.c_str(), isSelected)) {
                if (!available) {
                     auto& progress = models_.getDownloadProgress();
                     if (!progress.isDownloading) {
                         if (downloadThread_.joinable()) downloadThread_.join();
                         downloadThread_ = std::thread([this, modelName = models[i].name]() {
                             bool success = models_.downloadModel(modelName);
                             if (success && !isTranscribing_.load()) {
                                 // Load model after download only if not transcribing
                                 whisper_.loadModel(models_.getModelPath(modelName));
                             }
                         });
                     }
                } else {
                    // Defer model loading if transcription is in progress
                    if (isTranscribing_.load()) {
                        pendingSettings_.hasPendingModel = true;
                        pendingSettings_.pendingModel = static_cast<int>(i);
                        LOG_INFO("Deferred model change - transcription in progress");
                    } else {
                        settings_.selectedModel = static_cast<int>(i);
                        whisper_.loadModel(models_.getModelPath(models[i].name));
                    }
                }
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    
    if (!whisper_.isModelLoaded()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
        ImGui::TextWrapped("No model loaded. Please select a model above to download or load.");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::Text("Whisper Settings");
    
    // Language selection
    const char* languages[] = {"auto", "en", "es", "fr", "de", "it", "pt", "ru", "zh", "ja", "ko"};
    const char* languageNames[] = {"Auto-detect", "English", "Spanish", "French", "German", "Italian", "Portuguese", "Russian", "Chinese", "Japanese", "Korean"};
    constexpr int numLanguages = sizeof(languages) / sizeof(languages[0]);
    int currentLang = 0;
    for (int i = 0; i < numLanguages; i++) {
        if (settings_.language == languages[i]) {
            currentLang = i;
            break;
        }
    }
    
    if (ImGui::BeginCombo("Language", languageNames[currentLang])) {
        for (int i = 0; i < numLanguages; i++) {
            bool isSelected = (currentLang == i);
            if (ImGui::Selectable(languageNames[i], isSelected)) {
                // Defer language change if transcription is in progress
                if (isTranscribing_.load()) {
                    pendingSettings_.hasPendingLanguage = true;
                    pendingSettings_.pendingLanguage = languages[i];
                    LOG_INFO("Deferred language change - transcription in progress");
                } else {
                    settings_.language = languages[i];
                    whisper_.setLanguage(settings_.language);
                }
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Select the language for transcription or use auto-detect.");
    }
    
    if (ImGui::Checkbox("Translate to English", &settings_.translate)) {
        // Defer translate change if transcription is in progress
        if (isTranscribing_.load()) {
            pendingSettings_.hasPendingTranslate = true;
            pendingSettings_.pendingTranslate = settings_.translate;
            // Revert the checkbox state - it will be applied later
            settings_.translate = !settings_.translate;
            LOG_INFO("Deferred translate change - transcription in progress");
        } else {
            whisper_.setTranslate(settings_.translate);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Translate non-English audio to English during transcription.");
    }
    
    if (ImGui::Checkbox("Include Timestamps in Transcription", &settings_.printTimestamps)) {
        // Defer timestamp change if transcription is in progress
        if (isTranscribing_.load()) {
            pendingSettings_.hasPendingTimestamps = true;
            pendingSettings_.pendingTimestamps = settings_.printTimestamps;
            // Revert the checkbox state - it will be applied later
            settings_.printTimestamps = !settings_.printTimestamps;
            LOG_INFO("Deferred timestamp change - transcription in progress");
        } else {
            whisper_.setPrintTimestamps(settings_.printTimestamps);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add timestamps to each segment in the transcribed text.");
    }
    
    auto forceSpeakerDiarizationSetting = [&](bool enabled) {
        if (settings_.speakerDiarization == enabled) {
            return;
        }
        settings_.speakerDiarization = enabled;
        if (isTranscribing_.load()) {
            pendingSettings_.hasPendingDiarization = true;
            pendingSettings_.pendingDiarization = enabled;
            LOG_INFO("Deferred diarization change - transcription in progress");
        } else {
            whisper_.setSpeakerDiarization(settings_.speakerDiarization);
        }
    };

    if (settings_.liveTranscription && settings_.speakerDiarization) {
        forceSpeakerDiarizationSetting(false);
    }

    bool disableSpeakerDiarization = settings_.liveTranscription;
    if (disableSpeakerDiarization) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("Speaker Identification", &settings_.speakerDiarization)) {
        // Defer diarization change if transcription is in progress
        if (isTranscribing_.load()) {
            pendingSettings_.hasPendingDiarization = true;
            pendingSettings_.pendingDiarization = settings_.speakerDiarization;
            // Revert the checkbox state - it will be applied later
            settings_.speakerDiarization = !settings_.speakerDiarization;
            LOG_INFO("Deferred diarization change - transcription in progress");
        } else {
            whisper_.setSpeakerDiarization(settings_.speakerDiarization);
        }
        if (settings_.speakerDiarization) {
            settings_.liveTranscription = false;
        }
    }
    if (disableSpeakerDiarization) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered()) {
#if WHISPERGUI_HAS_SHERPA_ONNX
        ImGui::SetTooltip("Neural speaker diarization powered by sherpa-onnx (10k+ stars).\nOutput will include 'Speaker 1:', 'Speaker 2:', etc.\n\nRequires diarization models - see README for download instructions.");
#else
        ImGui::SetTooltip("Speaker diarization using audio energy heuristics.\nOutput will include 'Speaker 1:', 'Speaker 2:', etc.\n\nNote: Build with -DWHISPERGUI_USE_SHERPA_ONNX=ON\nfor production-grade neural diarization.");
#endif
    }
    
    // Show diarization status
    if (settings_.speakerDiarization) {
        ImGui::SameLine();
        if (whisper_.isSpeakerDiarizationReady()) {
#if WHISPERGUI_HAS_SHERPA_ONNX
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "(Neural - Ready)");
#else
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.0f, 1.0f), "(Heuristic)");
#endif
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "(Models not loaded)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Download diarization models below or place them in models/speaker-diarization/");
            }
        }
    }
    
    // Speaker Diarization Model Download Section
    ImGui::Separator();
    ImGui::Text("Speaker Diarization Models");
#if WHISPERGUI_HAS_SHERPA_ONNX
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Required for neural speaker identification");
#else
    ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.2f, 1.0f), "Build with --sherpa flag to enable neural diarization");
#endif
    
    if (ImGui::Button("Download All Diarization Models")) {
        auto allModels = models_.getAllSpeakerModels();
        for (const auto& model : allModels) {
            if (!models_.isSpeakerModelAvailable(model.name)) {
                auto& progress = models_.getDownloadProgress();
                if (!progress.isDownloading) {
                    if (downloadThread_.joinable()) downloadThread_.join();
                    downloadThread_ = std::thread([this, modelName = model.name]() {
                        LOG_INFO("Download All: Starting " + modelName);
                        models_.downloadSpeakerModel(modelName);
                    });
                }
            }
        }
    }

    // Segmentation Models Section
    ImGui::Spacing();
    ImGui::Text("1. Voice Segmentation Model:");
    auto segmentationModels = models_.getSegmentationModels();
    
    std::string segPreview = settings_.selectedSegmentationModel.empty() ? "Select Segmentation Model" : settings_.selectedSegmentationModel;
    if (ImGui::BeginCombo("Segmentation", segPreview.c_str())) {
        for (const auto& model : segmentationModels) {
            bool available = models_.isSpeakerModelAvailable(model.name);
            bool isSelected = (settings_.selectedSegmentationModel == model.name);
            std::string label = model.name + (available ? "" : " [Not Downloaded]");
            
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                if (available) {
                    // Defer model change if transcription is in progress
                    if (isTranscribing_.load()) {
                        pendingSettings_.hasPendingDiarizationModels = true;
                        pendingSettings_.pendingSegmentationModel = model.name;
                        if (pendingSettings_.pendingEmbeddingModel.empty()) {
                            pendingSettings_.pendingEmbeddingModel = settings_.selectedEmbeddingModel;
                        }
                        LOG_INFO("Deferred segmentation model change - transcription in progress");
                    } else {
                        settings_.selectedSegmentationModel = model.name;
                        LOG_INFO("User selected segmentation: " + model.name);
                        
                        // Auto-reinitialize if embedding is ready
                        if (!settings_.selectedEmbeddingModel.empty() && 
                            models_.isSpeakerModelAvailable(settings_.selectedEmbeddingModel)) {
                            std::string segPath = models_.getActualModelFilePath(model.name);
                            std::string embPath = models_.getActualModelFilePath(settings_.selectedEmbeddingModel);
                            whisper_.initializeSpeakerDiarization(segPath, embPath);
                        }
                    }
                }
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    
    for (size_t i = 0; i < segmentationModels.size(); ++i) {
        const auto& model = segmentationModels[i];
        bool available = models_.isSpeakerModelAvailable(model.name);
        ImGui::PushID(static_cast<int>(i * 100));
        if (available) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
            ImGui::BulletText("%s [Downloaded]", model.name.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::BulletText("%s", model.name.c_str());
            ImGui::SameLine();
            auto& progress = models_.getDownloadProgress();
            if (progress.isDownloading && progress.currentModel == model.name) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "(Downloading...)");
            } else {
                if (ImGui::SmallButton("Download")) {
                    if (!progress.isDownloading) {
                        if (downloadThread_.joinable()) downloadThread_.join();
                        downloadThread_ = std::thread([this, modelName = model.name]() {
                            models_.downloadSpeakerModel(modelName);
                        });
                    }
                }
            }
        }
        ImGui::PopID();
    }
    
    // Embedding Models Section
    ImGui::Spacing();
    ImGui::Text("2. Speaker Identification Model:");
    auto embeddingModels = models_.getEmbeddingModels();
    
    std::string embPreview = settings_.selectedEmbeddingModel.empty() ? "Select Embedding Model" : settings_.selectedEmbeddingModel;
    if (ImGui::BeginCombo("Embedding", embPreview.c_str())) {
        for (const auto& model : embeddingModels) {
            bool available = models_.isSpeakerModelAvailable(model.name);
            bool isSelected = (settings_.selectedEmbeddingModel == model.name);
            std::string label = model.name + (available ? "" : " [Not Downloaded]");
            
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                if (available) {
                    // Defer model change if transcription is in progress
                    if (isTranscribing_.load()) {
                        pendingSettings_.hasPendingDiarizationModels = true;
                        pendingSettings_.pendingEmbeddingModel = model.name;
                        if (pendingSettings_.pendingSegmentationModel.empty()) {
                            pendingSettings_.pendingSegmentationModel = settings_.selectedSegmentationModel;
                        }
                        LOG_INFO("Deferred embedding model change - transcription in progress");
                    } else {
                        settings_.selectedEmbeddingModel = model.name;
                        LOG_INFO("User selected embedding: " + model.name);
                        
                        if (!settings_.selectedSegmentationModel.empty() && 
                            models_.isSpeakerModelAvailable(settings_.selectedSegmentationModel)) {
                            std::string segPath = models_.getActualModelFilePath(settings_.selectedSegmentationModel);
                            std::string embPath = models_.getActualModelFilePath(model.name);
                            whisper_.initializeSpeakerDiarization(segPath, embPath);
                        }
                    }
                }
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    
    for (size_t i = 0; i < embeddingModels.size(); ++i) {
        const auto& model = embeddingModels[i];
        bool available = models_.isSpeakerModelAvailable(model.name);
        ImGui::PushID(static_cast<int>(i * 100 + 50));
        if (available) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
            ImGui::BulletText("%s [Downloaded]", model.name.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::BulletText("%s", model.name.c_str());
            ImGui::SameLine();
            auto& progress = models_.getDownloadProgress();
            if (progress.isDownloading && progress.currentModel == model.name) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "(Downloading...)");
            } else {
                if (ImGui::SmallButton("Download")) {
                    if (!progress.isDownloading) {
                        if (downloadThread_.joinable()) downloadThread_.join();
                        downloadThread_ = std::thread([this, modelName = model.name]() {
                            models_.downloadSpeakerModel(modelName);
                        });
                    }
                }
            }
        }
        ImGui::PopID();
    }
    
    ImGui::Separator();
    ImGui::Text("Automation");
    ImGui::Checkbox("Auto-paste text after transcription", &settings_.autoPaste);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Simulates keyboard typing to paste text into the currently focused window.");
    }
    
    ImGui::Separator();
    ImGui::Text("Live Transcription Mode");
    // TODO: Need to research sherpa continuity across files before implementing to see if possible.
    bool disableLiveTranscription = settings_.speakerDiarization;
    if (disableLiveTranscription) {
        ImGui::BeginDisabled();
    }
    bool liveTranscriptionChanged = ImGui::Checkbox("Enable Live Transcription", &settings_.liveTranscription);
    if (disableLiveTranscription) {
        ImGui::EndDisabled();
    }
    if (liveTranscriptionChanged && settings_.liveTranscription) {
        forceSpeakerDiarizationSetting(false);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically transcribe audio segments when speech pauses are detected.\nAllows for continuous dictation without manually stopping recording.");
    }
    
    if (settings_.liveTranscription) {
        ImGui::Indent();
        
        ImGui::SliderFloat("Silence Threshold", &settings_.silenceThreshold, 0.005f, 0.1f, "%.3f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Audio amplitude below this level is considered silence.\nCurrent mic level: %.3f", recorder_.getRecentPeakAmplitude());
        }
        
        ImGui::SliderFloat("Silence Duration (sec)", &settings_.silenceDuration, 0.5f, 5.0f, "%.1f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How long to wait in silence before triggering auto-transcription.");
        }
        
        ImGui::SliderFloat("Noise Floor", &settings_.noiseFloor, 0.001f, 0.05f, "%.3f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Minimum amplitude to consider as actual speech.\nClips below this are skipped.");
        }
        
        ImGui::Unindent();
    }
}

void Gui::loadHistory() {
    std::ifstream file("history.json");
    if (file.is_open()) {
        try {
            json j;
            file >> j;
            for (const auto& item : j) {
                HistoryItem hi;
                hi.text = item.value("text", "");
                hi.timestamp = item.value("timestamp", "");
                hi.recordingPath = item.value("recordingPath", "");
                history_.push_back(hi);
            }
        } catch (...) {}
    }
}

void Gui::saveHistory() {
    std::ofstream file("history.json");
    if (file.is_open()) {
        json j = json::array();
        for (const auto& item : history_) {
            j.push_back({
                {"text", item.text}, 
                {"timestamp", item.timestamp},
                {"recordingPath", item.recordingPath}
            });
        }
        file << j.dump(4);
    }
}

void Gui::addToHistory(const std::string& text, const std::string& recordingTimestamp, const std::string& recordingPath) {
    HistoryItem item;
    item.text = text;
    item.timestamp = recordingTimestamp;
    item.recordingPath = recordingPath;
    history_.push_back(item);
    saveHistory();
}

void Gui::loadSettings() {
    std::ifstream file("settings.json");
    if (file.is_open()) {
        try {
            json j;
            file >> j;
            settings_.selectedModel = j.value("selectedModel", -1);
            settings_.selectedDevice = j.value("selectedDevice", 0);
            settings_.autoPaste = j.value("autoPaste", false);
            settings_.autoTranscribe = j.value("autoTranscribe", true);
            settings_.showTimestamps = j.value("showTimestamps", true);
            settings_.pushToTalk = j.value("pushToTalk", false);
            settings_.hotkeySym = j.value("hotkeySym", 0ul);
            settings_.liveTranscription = j.value("liveTranscription", false);
            settings_.silenceThreshold = j.value("silenceThreshold", 0.02f);
            settings_.silenceDuration = j.value("silenceDuration", 1.5f);
            settings_.noiseFloor = j.value("noiseFloor", 0.005f);
            settings_.language = j.value("language", "en");
            settings_.translate = j.value("translate", false);
            settings_.printTimestamps = j.value("printTimestamps", false);
            settings_.speakerDiarization = j.value("speakerDiarization", false);
            settings_.selectedSegmentationModel = j.value("selectedSegmentationModel", "");
            settings_.selectedEmbeddingModel = j.value("selectedEmbeddingModel", "");
            LOG_INFO("Settings loaded");
        } catch (...) {
            LOG_WARNING("Failed to parse settings.json");
        }
    }

    // Auto-select models if none selected and something is available
    if (settings_.selectedSegmentationModel.empty()) {
        auto segModels = models_.getSegmentationModels();
        for (const auto& m : segModels) {
            if (models_.isSpeakerModelAvailable(m.name)) {
                settings_.selectedSegmentationModel = m.name;
                LOG_INFO("Auto-selected segmentation model: " + m.name);
                break;
            }
        }
    }
    if (settings_.selectedEmbeddingModel.empty()) {
        auto embModels = models_.getEmbeddingModels();
        for (const auto& m : embModels) {
            if (models_.isSpeakerModelAvailable(m.name)) {
                settings_.selectedEmbeddingModel = m.name;
                LOG_INFO("Auto-selected embedding model: " + m.name);
                break;
            }
        }
    }

    // Auto load whisper model
    if (settings_.selectedModel >= 0) {
        auto models = models_.getAvailableModels();
        if (settings_.selectedModel < static_cast<int>(models.size())) {
            std::string name = models[settings_.selectedModel].name;
            if (models_.isModelAvailable(name)) {
                LOG_INFO("Loading whisper model: " + name);
                whisper_.loadModel(models_.getModelPath(name));
            }
        }
    }
    
    // Apply whisper settings
    whisper_.setLanguage(settings_.language);
    whisper_.setTranslate(settings_.translate);
    whisper_.setPrintTimestamps(settings_.printTimestamps);
    whisper_.setSpeakerDiarization(settings_.speakerDiarization);
    
    // Auto-initialize speaker diarization if models are selected and available
    if (!settings_.selectedSegmentationModel.empty() && !settings_.selectedEmbeddingModel.empty()) {
        if (models_.isSpeakerModelAvailable(settings_.selectedSegmentationModel) &&
            models_.isSpeakerModelAvailable(settings_.selectedEmbeddingModel)) {
            std::string segPath = models_.getActualModelFilePath(settings_.selectedSegmentationModel);
            std::string embPath = models_.getActualModelFilePath(settings_.selectedEmbeddingModel);
            LOG_INFO("Auto-initializing speaker diarization");
            if (whisper_.initializeSpeakerDiarization(segPath, embPath)) {
                LOG_INFO("Speaker diarization initialized successfully");
            } else {
                LOG_ERROR("Failed to initialize speaker diarization during load");
            }
        }
    }
}

void Gui::saveSettings() {
    std::ofstream file("settings.json");
    if (file.is_open()) {
        json j;
        j["selectedModel"] = settings_.selectedModel;
        j["selectedDevice"] = settings_.selectedDevice;
        j["autoPaste"] = settings_.autoPaste;
        j["autoTranscribe"] = settings_.autoTranscribe;
        j["showTimestamps"] = settings_.showTimestamps;
        j["pushToTalk"] = settings_.pushToTalk;
        j["hotkeySym"] = input_.getHotkeySym();
        j["liveTranscription"] = settings_.liveTranscription;
        j["silenceThreshold"] = settings_.silenceThreshold;
        j["silenceDuration"] = settings_.silenceDuration;
        j["noiseFloor"] = settings_.noiseFloor;
        j["language"] = settings_.language;
        j["translate"] = settings_.translate;
        j["printTimestamps"] = settings_.printTimestamps;
        j["speakerDiarization"] = settings_.speakerDiarization;
        j["selectedSegmentationModel"] = settings_.selectedSegmentationModel;
        j["selectedEmbeddingModel"] = settings_.selectedEmbeddingModel;
        file << j.dump(4);
        LOG_INFO("Settings saved");
    }
}

void Gui::applyPendingSettings() {
    if (!pendingSettings_.hasAny()) return;
    
    LOG_INFO("Applying pending settings");
    
    if (pendingSettings_.hasPendingModel) {
        auto models = models_.getAvailableModels();
        if (pendingSettings_.pendingModel >= 0 && pendingSettings_.pendingModel < static_cast<int>(models.size())) {
            settings_.selectedModel = pendingSettings_.pendingModel;
            std::string name = models[settings_.selectedModel].name;
            if (models_.isModelAvailable(name)) {
                LOG_INFO("Applying pending model: " + name);
                whisper_.loadModel(models_.getModelPath(name));
            }
        }
    }
    
    if (pendingSettings_.hasPendingLanguage) {
        settings_.language = pendingSettings_.pendingLanguage;
        whisper_.setLanguage(settings_.language);
        LOG_INFO("Applying pending language: " + settings_.language);
    }
    
    if (pendingSettings_.hasPendingTranslate) {
        settings_.translate = pendingSettings_.pendingTranslate;
        whisper_.setTranslate(settings_.translate);
    }
    
    if (pendingSettings_.hasPendingTimestamps) {
        settings_.printTimestamps = pendingSettings_.pendingTimestamps;
        whisper_.setPrintTimestamps(settings_.printTimestamps);
    }
    
    if (pendingSettings_.hasPendingDiarization) {
        settings_.speakerDiarization = pendingSettings_.pendingDiarization;
        whisper_.setSpeakerDiarization(settings_.speakerDiarization);
    }
    
    if (pendingSettings_.hasPendingDiarizationModels) {
        settings_.selectedSegmentationModel = pendingSettings_.pendingSegmentationModel;
        settings_.selectedEmbeddingModel = pendingSettings_.pendingEmbeddingModel;
        
        if (!settings_.selectedSegmentationModel.empty() && !settings_.selectedEmbeddingModel.empty()) {
            if (models_.isSpeakerModelAvailable(settings_.selectedSegmentationModel) &&
                models_.isSpeakerModelAvailable(settings_.selectedEmbeddingModel)) {
                std::string segPath = models_.getActualModelFilePath(settings_.selectedSegmentationModel);
                std::string embPath = models_.getActualModelFilePath(settings_.selectedEmbeddingModel);
                LOG_INFO("Applying pending diarization models");
                whisper_.initializeSpeakerDiarization(segPath, embPath);
            }
        }
    }
    
    pendingSettings_.clear();
    saveSettings();
}

std::string Gui::getHotkeyName() const {
    unsigned long sym = input_.getHotkeySym();
    // Windows virtual key codes
    if (sym >= 0x70 && sym <= 0x87) {
        return "F" + std::to_string(sym - 0x70 + 1);
    }
    switch (sym) {
        case 0x08: return "Backspace";
        case 0x09: return "Tab";
        case 0x0D: return "Enter";
        case 0x1B: return "Escape";
        case 0x20: return "Space";
        case 0x2D: return "Insert";
        case 0x2E: return "Delete";
        case 0x24: return "Home";
        case 0x23: return "End";
        case 0x21: return "PageUp";
        case 0x22: return "PageDown";
        default:
            if (sym >= 0x41 && sym <= 0x5A) {
                return std::string(1, static_cast<char>(sym));
            }
            if (sym >= 0x30 && sym <= 0x39) {
                return std::string(1, static_cast<char>(sym));
            }
            return "Key " + std::to_string(sym);
    }
}

void Gui::cleanup() {
    // Delete all temp recordings tracked during this session
    for (const auto& path : tempRecordings_) {
        try {
            if (fs::exists(path)) {
                fs::remove(path);
            }
        } catch (...) {}
    }
    tempRecordings_.clear();
    
    // Also delete the temp_recording.wav if it exists
    try {
        if (fs::exists("temp_recording.wav")) {
            fs::remove("temp_recording.wav");
        }
    } catch (...) {}
}

void Gui::processTranscriptionQueue() {
while (true) {
    TranscriptionJob job;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (transcriptionQueue_.empty()) {
            return;
        }
        job = transcriptionQueue_.front();
        transcriptionQueue_.pop();
    }
        
    LOG_INFO("Starting transcription: " + job.audioPath);

    std::string text;

    // Ensure model is loaded
    if (!whisper_.isModelLoaded()) {
        auto models = models_.getAvailableModels();
        if (settings_.selectedModel >= 0 && settings_.selectedModel < static_cast<int>(models.size())) {
            LOG_INFO("Loading whisper model for transcription");
            whisper_.loadModel(models_.getModelPath(models[settings_.selectedModel].name));
        }
    }

    if (!whisper_.isModelLoaded()) {
        text = "Error: No Model Loaded";
        LOG_ERROR("Transcription failed - no model loaded");
    } else {
        auto startTime = std::chrono::steady_clock::now();
        text = whisper_.transcribeFile(job.audioPath);
        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

        if (text.find("Error:") == 0) {
            LOG_ERROR("Transcription failed: " + text);
        } else {
            LOG_INFO("Transcription completed in " + std::to_string(duration) + "ms");
        }
    }

        {
            std::lock_guard<std::mutex> lock(g_resultMutex);
            g_pendingResult = text;
            g_pendingHistoryLabel = job.historyLabel;
            g_pendingPath = job.audioPath;
            g_hasResult = true;
            g_pendingIsLiveSegment = job.isLiveSegment;
        }
        
        // Small delay to let the main thread process the result
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Wait until result is consumed before processing next
        while (true) {
            {
                std::lock_guard<std::mutex> lock(g_resultMutex);
                if (!g_hasResult) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

HWND Gui::getHwnd(SDL_Window* window) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        return wmInfo.info.win.window;
    }
    return NULL;
}

// Tray menu item IDs
#define TRAY_MENU_SHOW 1001
#define TRAY_MENU_START_RECORDING 1002
#define TRAY_MENU_STOP_RECORDING 1003
#define TRAY_MENU_AUTO_PASTE 1004
#define TRAY_MENU_LIVE_TRANSCRIPTION 1005
#define TRAY_MENU_PUSH_TO_TALK 1008
#define TRAY_MENU_SPEAKER_DIARIZATION 1009
#define TRAY_MENU_EXIT 1006

// Static Gui pointer for the window procedure
static Gui* g_trayGui = nullptr;
static SDL_Window* g_trayWindow = nullptr;

void Gui::initTrayIcon(SDL_Window* window) {
if (trayIconCreated_) return;
    
HWND hwnd = getHwnd(window);
if (!hwnd) return;
    
g_trayGui = this;
g_trayWindow = window;
    
// Load application icon
if (!appIcon_) {
    // Try to load from resource ID 1 (often IDI_ICON1) or string "IDI_ICON1"
    appIcon_ = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    if (!appIcon_) {
        appIcon_ = LoadIcon(GetModuleHandle(NULL), "IDI_ICON1");
    }
        
    // Fallback to resources/app.ico file
    if (!appIcon_) {
        appIcon_ = (HICON)LoadImageA(NULL, "resources/app.ico", IMAGE_ICON, 
                                      0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    // Fallback to app.ico in current directory
    if (!appIcon_) {
        appIcon_ = (HICON)LoadImageA(NULL, "app.ico", IMAGE_ICON,
                                      0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    // Last resort: use system default icon
    if (!appIcon_) {
        appIcon_ = LoadIcon(NULL, IDI_APPLICATION);
    }
}
    
    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(NOTIFYICONDATAA);
    nid_.hWnd = hwnd;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_USER + 1;
    nid_.hIcon = appIcon_;
    strcpy_s(nid_.szTip, "Whisper Studio - Right-click for options");
    
    Shell_NotifyIconA(NIM_ADD, &nid_);
    trayIconCreated_ = true;
}

void Gui::removeTrayIcon() {
    if (!trayIconCreated_) return;
    Shell_NotifyIconA(NIM_DELETE, &nid_);
    trayIconCreated_ = false;
    g_trayGui = nullptr;
    g_trayWindow = nullptr;
    // Note: We don't destroy appIcon_ here as it may be reused
}

void Gui::showTrayContextMenu(SDL_Window* window) {
    HWND hwnd = getHwnd(window);
    if (!hwnd) return;
    
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    
    // Add menu items
    AppendMenuA(hMenu, MF_STRING, TRAY_MENU_SHOW, "Open Whisper Studio");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    
    if (recorder_.isRecording()) {
        AppendMenuA(hMenu, MF_STRING, TRAY_MENU_STOP_RECORDING, "Stop Recording");
    } else {
        AppendMenuA(hMenu, MF_STRING, TRAY_MENU_START_RECORDING, "Start Recording");
    }
    
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, settings_.autoPaste ? MF_STRING | MF_CHECKED : MF_STRING, TRAY_MENU_AUTO_PASTE, "Auto-Paste");
    AppendMenuA(hMenu, settings_.liveTranscription ? MF_STRING | MF_CHECKED : MF_STRING, TRAY_MENU_LIVE_TRANSCRIPTION, "Live Transcription");

    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, settings_.speakerDiarization ? MF_STRING | MF_CHECKED : MF_STRING, TRAY_MENU_SPEAKER_DIARIZATION, "Speaker Identification");

    AppendMenuA(hMenu, settings_.pushToTalk ? MF_STRING | MF_CHECKED : MF_STRING, TRAY_MENU_PUSH_TO_TALK, "Push-to-Talk");

    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, TRAY_MENU_EXIT, "Exit");
    
    // Get cursor position
    POINT pt;
    GetCursorPos(&pt);
    
    // Required to make menu disappear when clicking outside
    SetForegroundWindow(hwnd);
    
    // Show menu
    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    
    // Process command
    switch (cmd) {
        case TRAY_MENU_SHOW:
            showFromTray(window);
            break;
        case TRAY_MENU_START_RECORDING:
            startRecordingRequest_ = true;
            break;
        case TRAY_MENU_STOP_RECORDING:
            stopRecordingRequest_ = true;
            break;
        case TRAY_MENU_AUTO_PASTE:
            settings_.autoPaste = !settings_.autoPaste;
            saveSettings();
            break;
        case TRAY_MENU_LIVE_TRANSCRIPTION:
            settings_.liveTranscription = !settings_.liveTranscription;
            saveSettings();
            break;
        case TRAY_MENU_PUSH_TO_TALK:
            settings_.pushToTalk = !settings_.pushToTalk;
            saveSettings();
            break;
        case TRAY_MENU_SPEAKER_DIARIZATION:
            settings_.speakerDiarization = !settings_.speakerDiarization;
            // Whisper engine update needed
            whisper_.setSpeakerDiarization(settings_.speakerDiarization);
            if (settings_.speakerDiarization) {
                settings_.liveTranscription = false;
            }
            saveSettings();
            break;
        case TRAY_MENU_EXIT:
            // Post quit event
            SDL_Event quitEvent;
            quitEvent.type = SDL_QUIT;
            SDL_PushEvent(&quitEvent);
            break;
    }
    
    DestroyMenu(hMenu);
    
    // Required for the menu to work properly
    PostMessage(hwnd, WM_NULL, 0, 0);
}

void Gui::showFromTray(SDL_Window* window) {
    removeTrayIcon();
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    isHidden_ = false;
}
