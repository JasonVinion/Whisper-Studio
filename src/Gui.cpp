#include "Gui.h"
#include "InputManager.h"
#include "WhisperEngine.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include <SDL_syswm.h>
#include <commdlg.h>
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
using json = nlohmann::json;
using Kind = ModelManager::Kind;
using Clock = std::chrono::steady_clock;

namespace {
const ImVec4 kGreen{0.2f, 0.8f, 0.2f, 1}, kYellow{0.8f, 0.8f, 0.2f, 1}, kOrange{1, 0.6f, 0.2f, 1}, kGrey{0.6f, 0.6f, 0.6f, 1};
const char* kAudioExts[] = {".wav", ".mp3", ".m4a", ".flac", ".ogg", ".opus", ".aac", ".wma", ".aiff", ".aif", ".aifc"};
const char* kLangCodes[] = {"auto", "en", "es", "fr", "de", "it", "pt", "ru", "zh", "ja", "ko"};
const char* kLangNames[] = {"Auto-detect", "English", "Spanish", "French", "German", "Italian",
                            "Portuguese", "Russian", "Chinese", "Japanese", "Korean"};

void tooltip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void warn(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void section() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

bool isError(const std::string& text) { return text.rfind("Error:", 0) == 0; }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string formatBytes(double bytes) {
    char s[32];
    if (bytes >= 1 << 20) std::snprintf(s, sizeof s, "%.1f MB", bytes / (1 << 20));
    else std::snprintf(s, sizeof s, "%.0f KB", bytes / 1024);
    return s;
}

// Writes to a temp file first so a crash mid-write can't wipe the existing file.
void writeJson(const char* path, const json& j) {
    const std::string tmp = std::string(path) + ".tmp";
    std::ofstream(tmp) << j.dump(4);
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) LOG_ERROR(std::string("Failed to save ") + path + ": " + ec.message());
}

template <class T> bool readJson(const char* path, T& out) {
    std::ifstream f(path);
    if (!f) return false;
    try {
        out = json::parse(f).get<T>();
        return true;
    } catch (const std::exception& e) {
        LOG_WARNING(std::string("Ignoring unreadable ") + path + ": " + e.what());
        return false;
    }
}

std::string openAudioFileDialog() {
    char buffer[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "Audio Files\0*.wav;*.mp3;*.m4a;*.flac;*.ogg;*.opus;*.aac;*.wma;*.aiff;*.aif;*.aifc\0All Files\0*.*\0";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? buffer : "";
}

std::string openFolderDialog() {
    char buffer[MAX_PATH] = {};
    BROWSEINFOA bi{};
    bi.lpszTitle = "Select Folder with Audio Files";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return "";
    bool ok = SHGetPathFromIDListA(pidl, buffer);
    CoTaskMemFree(pidl);
    return ok ? buffer : "";
}
} // namespace

Gui::Gui(SDL_Window* window, AudioRecorder& recorder, WhisperEngine& whisper, ModelManager& models, InputManager& input)
    : window_(window), recorder_(recorder), whisper_(whisper), models_(models), input_(input) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5;
    style.FrameRounding = style.ScrollbarRounding = style.GrabRounding = 4;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = {0.10f, 0.10f, 0.13f, 1};
    c[ImGuiCol_Header] = c[ImGuiCol_Button] = {0.20f, 0.20f, 0.25f, 1};
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered] = {0.26f, 0.26f, 0.35f, 1};
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive] = {0.30f, 0.30f, 0.40f, 1};
    c[ImGuiCol_FrameBg] = {0.15f, 0.15f, 0.18f, 1};
    c[ImGuiCol_TitleBgActive] = {0.15f, 0.15f, 0.20f, 1};

    icon_ = loadAppIcon();
    if (HWND h = hwnd()) {
        SendMessageA(h, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_));
        SendMessageA(h, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_));
    }

    readJson("history.json", history_);
    readJson("settings.json", settings_);
    if (settings_.hotkeySym) input_.setHotkey(static_cast<unsigned>(settings_.hotkeySym));
    input_.start([] { // wake the main loop so hotkeys respond immediately even while it idles
        SDL_Event e{};
        e.type = SDL_USEREVENT;
        SDL_PushEvent(&e);
    });

    devices_ = AudioRecorder::inputDevices();
    refreshModels();
    applyOptions();
    const int m = settings_.selectedModel;
    if (m >= 0 && m < static_cast<int>(whisperAvailable_.size()) && whisperAvailable_[m]) selectModel(m);
}

Gui::~Gui() {
    input_.stop();
    models_.cancel();
    whisper_.cancel();
    worker_.shutdown();
    downloader_.shutdown();
    recorder_.stop();
    saveSettings();
    saveHistory();
    std::error_code ec;
    for (const auto& path : tempRecordings_) fs::remove(path, ec);
    if (trayIcon_) Shell_NotifyIconA(NIM_DELETE, &nid_);
}

bool Gui::isBusy() const {
    return recorder_.isRecording() || worker_.pending() > 0 || downloading_.load() || input_.isLearning();
}

// ---------------------------------------------------------------------------------------------
// Logic
// ---------------------------------------------------------------------------------------------

void Gui::update() {
    // Push-to-talk follows the key; otherwise each press toggles recording. Both work from the tray.
    const bool held = input_.isHotkeyHeld();
    if (settings_.pushToTalk && held != hotkeyWasHeld_) {
        if (held) startRecording();
        else stopRecording();
    }
    hotkeyWasHeld_ = held;
    if (input_.consumePress() && !settings_.pushToTalk) toggleRecording();

    // Live mode: once speech is followed by enough silence, hand the segment off and keep recording.
    recorder_.setSilenceThreshold(settings_.silenceThreshold);
    if (settings_.liveTranscription && recorder_.isRecording()) {
        if (recorder_.peak() > settings_.noiseFloor) hadSound_ = true;
        if (hadSound_ && recorder_.silenceSeconds() >= settings_.silenceDuration) {
            if (liveLabel_.empty()) liveLabel_ = recordingLabel_;
            const std::string finished = recordingPath_;
            recordingPath_ = recordingStem_ + "_seg" + std::to_string(++liveSegment_) + ".wav";
            AudioRecorder::Stats stats;
            hadSound_ = false;
            if (recorder_.rollover(recordingPath_, stats)) {
                tempRecordings_.push_back(recordingPath_);
            } else {
                recorder_.stop();
                status_ = "Error: Could not create the next live segment file.";
            }
            queueRecording(finished, stats);
        }
    }

    std::vector<Result> results;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        results.swap(results_);
        if (!asyncStatus_.empty()) status_ = std::move(asyncStatus_);
        asyncStatus_.clear();
    }
    for (const auto& r : results) addResult(r);
    if (!results.empty()) saveHistory();

    if (modelsChanged_.exchange(false)) refreshModels();
    const int downloaded = downloadedWhisper_.exchange(-1);
    if (downloaded >= 0) selectModel(downloaded);
}

void Gui::addResult(const Result& r) {
    const bool failed = isError(r.text);
    status_ = failed ? r.text : "Idle";
    if (failed && r.live) return; // keep errors out of the running live transcript

    std::string paste = r.text;
    auto live = r.live ? std::find_if(history_.rbegin(), history_.rend(), [&](auto& h) { return h.timestamp == r.label; })
                       : history_.rend();
    if (live != history_.rend()) { // append to this live session's entry
        std::string& text = live->text;
        if (!text.empty() && !r.text.empty() && !std::isspace(static_cast<unsigned char>(text.back())) &&
            !std::isspace(static_cast<unsigned char>(r.text.front())))
            paste.insert(0, " ");
        text += paste;
    } else {
        history_.push_back({r.text, r.label, r.path});
    }
    if (settings_.autoPaste && !failed) InputManager::paste(paste);
}

void Gui::startRecording() {
    if (recorder_.isRecording()) return;
    recordingLabel_ = localTime("%d-%m-%Y %H:%M:%S");
    recordingStem_ = "recording_" + localTime("%d-%m-%Y_%H-%M-%S");
    recordingPath_ = recordingStem_ + ".wav";
    liveLabel_ = settings_.liveTranscription ? recordingLabel_ : "";
    liveSegment_ = 0;
    hadSound_ = false;
    const int dev = settings_.selectedDevice;
    const std::string device = dev >= 0 && dev < static_cast<int>(devices_.size()) ? devices_[dev] : "";
    recorder_.setSilenceThreshold(settings_.silenceThreshold);
    if (recorder_.start(device, recordingPath_)) {
        tempRecordings_.push_back(recordingPath_);
        LOG_INFO("Recording started: " + recordingPath_);
    } else {
        status_ = "Error: Could not start recording.";
    }
}

void Gui::stopRecording() {
    if (!recorder_.isRecording()) return;
    const auto stats = recorder_.stop();
    LOG_INFO("Recording stopped: " + recordingPath_);
    if (settings_.autoTranscribe || !liveLabel_.empty()) queueRecording(recordingPath_, stats);
    else status_ = "Recording saved.";
}

void Gui::toggleRecording() {
    if (recorder_.isRecording()) stopRecording();
    else startRecording();
}

void Gui::queueRecording(const std::string& path, const AudioRecorder::Stats& stats) {
    if (stats.avg < settings_.noiseFloor && stats.peak < settings_.noiseFloor * 3) {
        status_ = "Skipped: audio was silent.";
        return;
    }
    const bool live = !liveLabel_.empty();
    enqueue(path, live ? liveLabel_ : recordingLabel_, live);
}

void Gui::enqueue(std::string path, std::string label, bool live) {
    worker_.post([this, path = std::move(path), label = std::move(label), live] {
        LOG_INFO("Transcribing " + path);
        const auto start = Clock::now();
        std::string text = whisper_.transcribeFile(path);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        if (isError(text)) LOG_ERROR(path + ": " + text);
        else LOG_INFO("Transcribed in " + std::to_string(ms) + " ms");
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back({std::move(text), label, path, live});
    });
}

void Gui::notify(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    asyncStatus_ = std::move(message);
}

// Loading happens on the worker, after any transcriptions already queued, so the UI never blocks.
void Gui::selectModel(int index) {
    settings_.selectedModel = index;
    const Model& m = models_.whisperModels()[index];
    status_ = "Loading " + m.name + "...";
    worker_.post([this, &m] {
        notify(whisper_.loadModel(models_.path(m)) ? "Loaded " + m.name : "Error: Could not load " + m.name);
    });
}

void Gui::download(std::vector<const Model*> list) {
    if (list.empty()) return;
    if (downloading_) {
        status_ = "Wait for the current download to finish.";
        return;
    }
    downloading_ = list.front();
    downloadStart_ = Clock::now();
    downloader_.post([this, list] {
        for (const Model* m : list) {
            downloading_ = m;
            const bool ok = models_.download(*m);
            notify((ok ? "Downloaded " : "Error: Download failed: ") + m->name);
            if (ok && m->kind == Kind::Whisper) downloadedWhisper_ = static_cast<int>(m - models_.whisperModels().data());
            modelsChanged_ = true;
        }
        downloading_ = nullptr;
    });
}

void Gui::refreshModels() {
    auto check = [&](const std::vector<Model>& list, std::vector<char>& out) {
        out.clear();
        for (const auto& m : list) out.push_back(models_.isAvailable(m));
    };
    check(models_.whisperModels(), whisperAvailable_);
    check(models_.speakerModels(), speakerAvailable_);

    // Pick the first downloaded diarization model of each kind if none is chosen yet.
    bool changed = false;
    for (size_t i = 0; i < speakerAvailable_.size(); ++i) {
        const Model& m = models_.speakerModels()[i];
        std::string& selected = speakerSelection(m.kind);
        if (selected.empty() && speakerAvailable_[i]) {
            selected = m.name;
            changed = true;
        }
    }
    if (changed) applyOptions();
}

std::string& Gui::speakerSelection(Kind kind) {
    return kind == Kind::Segmentation ? settings_.selectedSegmentationModel : settings_.selectedEmbeddingModel;
}

bool Gui::diarizationReady() const {
    if (!WHISPERGUI_HAS_SHERPA_ONNX) return true; // the heuristic needs no models
    for (const std::string* name : {&settings_.selectedSegmentationModel, &settings_.selectedEmbeddingModel}) {
        const Model* m = models_.findSpeakerModel(*name);
        if (!m || !speakerAvailable_[m - models_.speakerModels().data()]) return false;
    }
    return true;
}

void Gui::applyOptions() {
    WhisperEngine::Options o;
    o.language = settings_.language;
    o.translate = settings_.translate;
    o.timestamps = settings_.printTimestamps;
    o.diarize = settings_.speakerDiarization;
    const Model* seg = models_.findSpeakerModel(settings_.selectedSegmentationModel);
    const Model* emb = models_.findSpeakerModel(settings_.selectedEmbeddingModel);
    if (seg && emb) {
        o.segmentationModel = models_.path(*seg);
        o.embeddingModel = models_.path(*emb);
    }
    whisper_.setOptions(std::move(o));
}

// Live transcription and speaker identification can't be combined (diarization needs the whole recording).
void Gui::setLive(bool on) {
    settings_.liveTranscription = on;
    if (on) settings_.speakerDiarization = false;
    applyOptions();
}

void Gui::setDiarization(bool on) {
    settings_.speakerDiarization = on;
    if (on) settings_.liveTranscription = false;
    applyOptions();
}

std::string Gui::hotkeyName() const {
    const unsigned vk = input_.hotkey();
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    return "Key " + std::to_string(vk);
}

// ---------------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------------

void Gui::saveSettings() {
    settings_.hotkeySym = input_.hotkey();
    writeJson("settings.json", settings_);
}

void Gui::saveHistory() { writeJson("history.json", history_); }

void Gui::exportHistory(const char* ext) {
    if (history_.empty()) {
        status_ = "No history to export.";
        return;
    }
    const std::string name = "transcription_history_" + localTime("%Y%m%d_%H%M%S") + "." + ext;
    std::ofstream f(name);
    if (!std::strcmp(ext, "json")) {
        f << json(history_).dump(4);
    } else {
        // SRT entries get consecutive 5-second slots: history items carry no timing of their own.
        auto srtTime = [](int s) {
            char t[16];
            std::snprintf(t, sizeof t, "%02d:%02d:%02d,000", s / 3600, s / 60 % 60, s % 60);
            return std::string(t);
        };
        int n = 0;
        for (auto it = history_.rbegin(); it != history_.rend(); ++it, ++n) {
            if (*ext == 's') f << n + 1 << '\n' << srtTime(n * 5) << " --> " << srtTime(n * 5 + 5) << '\n';
            else if (settings_.showTimestamps) f << '[' << it->timestamp << "]\n";
            f << it->text << "\n\n";
        }
    }
    f.flush();
    status_ = (f ? "Exported to " : "Error: Could not write ") + name;
}

// ---------------------------------------------------------------------------------------------
// Window and tray
// ---------------------------------------------------------------------------------------------

HWND Gui::hwnd() const {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    return SDL_GetWindowWMInfo(window_, &info) ? info.info.win.window : nullptr;
}

void Gui::hide() {
    if (!trayIcon_) {
        nid_ = {};
        nid_.cbSize = sizeof nid_;
        nid_.hWnd = hwnd();
        nid_.uID = 1;
        nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid_.uCallbackMessage = kTrayMessage;
        nid_.hIcon = icon_;
        std::snprintf(nid_.szTip, sizeof nid_.szTip, "Whisper Studio - Right-click for options");
        trayIcon_ = Shell_NotifyIconA(NIM_ADD, &nid_);
        if (!trayIcon_) return; // never hide the window without a way to bring it back
    }
    SDL_HideWindow(window_);
    hidden_ = true;
}

void Gui::show() {
    if (trayIcon_) Shell_NotifyIconA(NIM_DELETE, &nid_);
    trayIcon_ = false;
    SDL_ShowWindow(window_);
    SDL_RaiseWindow(window_);
    hidden_ = false;
}

void Gui::onTrayMessage(LPARAM event) {
    switch (LOWORD(event)) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK: show(); break;
    case WM_RBUTTONUP: trayMenu(); break;
    }
}

void Gui::trayMenu() {
    enum : UINT { Open = 1, Record, AutoPaste, Live, Diarize, PushToTalk, Exit };
    HWND h = hwnd();
    HMENU menu = CreatePopupMenu();
    if (!h || !menu) return;
    auto item = [&](UINT id, const char* text, bool checked = false) {
        AppendMenuA(menu, MF_STRING | (checked ? MF_CHECKED : 0), id, text);
    };
    auto separator = [&] { AppendMenuA(menu, MF_SEPARATOR, 0, nullptr); };
    item(Open, "Open Whisper Studio");
    separator();
    item(Record, recorder_.isRecording() ? "Stop Recording" : "Start Recording");
    separator();
    item(AutoPaste, "Auto-Paste", settings_.autoPaste);
    item(Live, "Live Transcription", settings_.liveTranscription);
    separator();
    item(Diarize, "Speaker Identification", settings_.speakerDiarization);
    item(PushToTalk, "Push-to-Talk", settings_.pushToTalk);
    separator();
    item(Exit, "Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(h); // so the menu closes when clicking elsewhere
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
    DestroyMenu(menu);
    PostMessageA(h, WM_NULL, 0, 0);

    switch (cmd) {
    case Open: show(); break;
    case Record: toggleRecording(); break;
    case AutoPaste: settings_.autoPaste = !settings_.autoPaste; break;
    case Live: setLive(!settings_.liveTranscription); break;
    case Diarize: setDiarization(!settings_.speakerDiarization); break;
    case PushToTalk: settings_.pushToTalk = !settings_.pushToTalk; break;
    case Exit: {
        SDL_Event e{};
        e.type = SDL_QUIT;
        SDL_PushEvent(&e);
        break;
    }
    }
    if (cmd >= AutoPaste && cmd <= PushToTalk) saveSettings();
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

void Gui::render() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 10});
    ImGui::Begin("Whisper Studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleVar();
    if (!whisper_.isModelLoaded()) {
        warn("Please download and select a model under Settings before using transcription.");
        ImGui::Separator();
    }
    ImGui::BeginChild("Main", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    renderControls();
    section();
    renderStatus();
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Transcription History")) renderHistory();
    if (ImGui::CollapsingHeader("Settings")) renderSettings();
    ImGui::EndChild();
    ImGui::End();
}

void Gui::renderControls() {
    const std::string key = hotkeyName();
    if (input_.isLearning()) ImGui::Button("Press F1-F24 to bind...###rebind", {200, 0});
    else if (ImGui::Button(("Rebind Hotkey (" + key + ")###rebind").c_str(), {200, 0})) input_.startLearning();
    ImGui::SameLine();
    ImGui::Checkbox("Push-to-Talk", &settings_.pushToTalk);
    tooltip("When enabled, hold the hotkey or the record button to record.");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-Transcribe", &settings_.autoTranscribe);
    tooltip("Automatically transcribe after recording stops.");
    ImGui::SameLine();
    if (ImGui::Button("Minimize to Tray")) hide();
    tooltip("Hide to the system tray. Click the tray icon to restore; the hotkey keeps working.");
    section();

    ImGui::TextUnformatted("Microphone Level:");
    ImGui::ProgressBar(recorder_.amplitude() * 5, {-1, 0});
    ImGui::Spacing();

    const bool rec = recorder_.isRecording();
    ImGui::PushStyleColor(ImGuiCol_Button, rec ? ImVec4{0.7f, 0.2f, 0.2f, 1} : ImVec4{0.2f, 0.5f, 0.2f, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rec ? ImVec4{0.8f, 0.2f, 0.2f, 1} : ImVec4{0.2f, 0.6f, 0.2f, 1});
    // Stable "###record" ID so holding the button survives its label changing.
    const bool clicked = ImGui::Button(((rec ? "Stop Recording (" : "Start Recording (") + key + ")###record").c_str(), {-1, 60});
    ImGui::PopStyleColor(2);
    if (!settings_.pushToTalk) {
        if (clicked) toggleRecording();
    } else if (ImGui::IsItemActivated()) {
        startRecording();
    } else if (ImGui::IsItemDeactivated()) {
        stopRecording();
    }
    ImGui::Spacing();

    if (ImGui::Button("Transcribe Audio File...", {-1, 0})) {
        const std::string path = openAudioFileDialog();
        if (!path.empty()) enqueue(path, fs::path(path).filename().string(), false);
    }
    if (ImGui::Button("Transcribe Folder...", {-1, 0})) {
        const std::string dir = openFolderDialog();
        if (!dir.empty()) {
            int count = 0;
            try {
                for (const auto& entry : fs::directory_iterator(dir)) {
                    const std::string ext = lower(entry.path().extension().string());
                    if (!entry.is_regular_file() || std::find(std::begin(kAudioExts), std::end(kAudioExts), ext) == std::end(kAudioExts))
                        continue;
                    enqueue(entry.path().string(), entry.path().filename().string(), false);
                    ++count;
                }
                status_ = count ? "Queued " + std::to_string(count) + " files." : "No audio files found in folder.";
            } catch (const std::exception& e) {
                status_ = std::string("Error reading folder: ") + e.what();
            }
        }
    }
    ImGui::Spacing();

    if (ImGui::Button("Open Recordings & Transcriptions Folder", {-1, 0}))
        ShellExecuteA(nullptr, "open", ".", nullptr, nullptr, SW_SHOWDEFAULT);
    ImGui::BeginDisabled(rec || worker_.pending() > 0); // don't delete files that are still in use
    if (ImGui::Button("Delete Temp Recordings & Transcriptions", {-1, 0})) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(".", ec)) {
            const std::string name = entry.path().filename().string(), ext = lower(entry.path().extension().string());
            if ((name.rfind("recording_", 0) == 0 && ext == ".wav") ||
                (name.rfind("transcription_history_", 0) == 0 && (ext == ".txt" || ext == ".json" || ext == ".srt")))
                fs::remove(entry.path(), ec);
        }
        tempRecordings_.clear();
        status_ = "Deleted temp recordings and transcription files.";
    }
    ImGui::EndDisabled();
}

void Gui::renderStatus() {
    const int jobs = worker_.pending();
    const std::string state = recorder_.isRecording() ? "Recording..."
                              : jobs                  ? "Working... (" + std::to_string(jobs) + " left)"
                                                      : status_;
    ImGui::Text("State: %s", state.c_str());
    ImGui::SameLine();
    ImGui::TextColored(WHISPERGUI_CUDA_ENABLED ? kGreen : kYellow,
                       WHISPERGUI_CUDA_ENABLED ? " | GPU: CUDA Enabled" : " | GPU: CPU Only");
    if (jobs) ImGui::ProgressBar(-0.2f * static_cast<float>(ImGui::GetTime()), {-1, 0}, "Processing...");

    const Model* m = downloading_;
    if (!m) return;
    const auto& p = models_.progress();
    const double done = p.done, total = p.total, speed = p.speed;
    const int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - downloadStart_).count());
    char eta[16] = "calculating...";
    if (speed > 0 && total > 0) {
        const int s = static_cast<int>((total - done) / speed);
        std::snprintf(eta, sizeof eta, "%d:%02d", s / 60, s % 60);
    }
    ImGui::Separator();
    ImGui::Text("Downloading: %s", m->name.c_str());
    ImGui::Text("Elapsed: %d:%02d | ETA: %s | Speed: %s", elapsed / 60, elapsed % 60, eta,
                speed > 0 ? (formatBytes(speed) + "/s").c_str() : "calculating...");
    if (total > 0)
        ImGui::ProgressBar(static_cast<float>(done / total), {-1, 0}, (formatBytes(done) + " / " + formatBytes(total)).c_str());
    else
        ImGui::ProgressBar(-0.3f * static_cast<float>(ImGui::GetTime()), {-1, 0}, "Downloading...");
}

void Gui::renderHistory() {
    if (ImGui::Button("Clear Log")) {
        history_.clear();
        editing_ = -1;
        saveHistory();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show Timestamps", &settings_.showTimestamps);
    static const char* exports[][2] = {{"Export TXT", "txt"}, {"Export JSON", "json"}, {"Export SRT", "srt"}};
    for (auto& e : exports) {
        ImGui::SameLine();
        if (ImGui::Button(e[0])) exportHistory(e[1]);
    }
    ImGui::Separator();

    ImGui::BeginChild("History", {0, 300}, ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (int i = static_cast<int>(history_.size()) - 1; i >= 0; --i) { // newest first
        HistoryItem& item = history_[i];
        ImGui::PushID(i);
        if (settings_.showTimestamps) ImGui::TextColored(kGrey, "[%s]", item.timestamp.c_str());
        if (editing_ == i) {
            ImGui::InputTextMultiline("##edit", &editBuffer_, {-1, 100}, ImGuiInputTextFlags_AllowTabInput);
            if (ImGui::Button("Save")) {
                item.text = editBuffer_;
                editing_ = -1;
                saveHistory();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) editing_ = -1;
        } else {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(item.text.c_str(), item.text.c_str() + item.text.size());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Copy")) ImGui::SetClipboardText(item.text.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Edit")) {
                editing_ = i;
                editBuffer_ = item.text;
            }
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void Gui::renderSpeakerModels(Kind kind, const char* title, const char* comboLabel) {
    std::string& selected = speakerSelection(kind);
    const auto& list = models_.speakerModels();
    ImGui::Spacing();
    ImGui::TextUnformatted(title);
    if (ImGui::BeginCombo(comboLabel, selected.empty() ? "Select Model" : selected.c_str())) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].kind != kind) continue;
            const bool have = speakerAvailable_[i];
            const std::string label = list[i].name + (have ? "" : " [Not Downloaded]");
            if (ImGui::Selectable(label.c_str(), selected == list[i].name, have ? 0 : ImGuiSelectableFlags_Disabled)) {
                selected = list[i].name;
                applyOptions();
            }
        }
        ImGui::EndCombo();
    }
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].kind != kind) continue;
        ImGui::PushID(static_cast<int>(i));
        if (speakerAvailable_[i]) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::BulletText("%s [Downloaded]", list[i].name.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::BulletText("%s", list[i].name.c_str());
            ImGui::SameLine();
            if (downloading_ == &list[i]) ImGui::TextColored(kYellow, "(Downloading...)");
            else if (ImGui::SmallButton("Download")) download({&list[i]});
        }
        ImGui::PopID();
    }
}

void Gui::renderSettings() {
    if (ImGui::Button("Save Settings")) saveSettings();
    ImGui::SameLine();
    ImGui::TextDisabled("(Settings are also saved on exit)");

    ImGui::SeparatorText("Audio Input");
    int& dev = settings_.selectedDevice;
    const bool validDevice = dev >= 0 && dev < static_cast<int>(devices_.size());
    if (ImGui::BeginCombo("Device", validDevice ? devices_[dev].c_str() : "Default Device")) {
        if (ImGui::IsWindowAppearing()) devices_ = AudioRecorder::inputDevices(); // re-scan only when opened
        for (int i = 0; i < static_cast<int>(devices_.size()); ++i)
            if (ImGui::Selectable(devices_[i].c_str(), dev == i)) dev = i;
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("Whisper Model");
    const auto& list = models_.whisperModels();
    const int sel = settings_.selectedModel;
    if (ImGui::BeginCombo("Model", sel >= 0 && sel < static_cast<int>(list.size()) ? list[sel].name.c_str() : "Select Model")) {
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const bool have = whisperAvailable_[i];
            const char* suffix = have ? "" : downloading_ == &list[i] ? " [Downloading...]" : " [Click to Download]";
            if (ImGui::Selectable((list[i].name + suffix).c_str(), sel == i)) {
                if (have) selectModel(i);
                else download({&list[i]});
            }
        }
        ImGui::EndCombo();
    }
    if (!whisper_.isModelLoaded()) warn("No model loaded. Pick one above to download or load it.");

    ImGui::SeparatorText("Whisper Settings");
    int lang = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kLangCodes); ++i)
        if (settings_.language == kLangCodes[i]) lang = i;
    if (ImGui::Combo("Language", &lang, kLangNames, IM_ARRAYSIZE(kLangNames))) {
        settings_.language = kLangCodes[lang];
        applyOptions();
    }
    tooltip("Select the language for transcription or use auto-detect.");
    if (ImGui::Checkbox("Translate to English", &settings_.translate)) applyOptions();
    tooltip("Translate non-English audio to English during transcription.");
    if (ImGui::Checkbox("Include Timestamps in Transcription", &settings_.printTimestamps)) applyOptions();
    tooltip("Add timestamps to each segment in the transcribed text.");
    bool diarize = settings_.speakerDiarization;
    if (ImGui::Checkbox("Speaker Identification", &diarize)) setDiarization(diarize);
    tooltip(WHISPERGUI_HAS_SHERPA_ONNX
                ? "Neural speaker diarization (sherpa-onnx). Output is labelled 'Speaker 1:', 'Speaker 2:', etc.\n"
                  "Needs the diarization models below. Turns off Live Transcription."
                : "Speaker detection from loudness changes. Output is labelled 'Speaker 1:', 'Speaker 2:', etc.\n"
                  "Build with -DWHISPERGUI_USE_SHERPA_ONNX=ON for neural diarization. Turns off Live Transcription.");
    if (diarize) {
        ImGui::SameLine();
        if (!diarizationReady()) ImGui::TextColored(kOrange, "(Select downloaded models below)");
        else ImGui::TextColored(kGreen, WHISPERGUI_HAS_SHERPA_ONNX ? "(Neural - Ready)" : "(Heuristic)");
    }

    if (WHISPERGUI_HAS_SHERPA_ONNX) {
        ImGui::SeparatorText("Speaker Diarization Models");
        if (ImGui::Button("Download All Diarization Models")) {
            std::vector<const Model*> missing;
            for (size_t i = 0; i < speakerAvailable_.size(); ++i)
                if (!speakerAvailable_[i]) missing.push_back(&models_.speakerModels()[i]);
            download(missing);
        }
        renderSpeakerModels(Kind::Segmentation, "1. Voice Segmentation Model:", "Segmentation");
        renderSpeakerModels(Kind::Embedding, "2. Speaker Identification Model:", "Embedding");
    }

    ImGui::SeparatorText("Automation");
    ImGui::Checkbox("Auto-paste text after transcription", &settings_.autoPaste);
    tooltip("Pastes the text into the focused window (via the clipboard and Ctrl+V).");

    ImGui::SeparatorText("Live Transcription Mode");
    bool live = settings_.liveTranscription;
    if (ImGui::Checkbox("Enable Live Transcription", &live)) setLive(live);
    tooltip("Automatically transcribe audio segments when speech pauses are detected.\n"
            "Allows continuous dictation without stopping the recording. Turns off Speaker Identification.");
    if (live) {
        ImGui::Indent();
        ImGui::SliderFloat("Silence Threshold", &settings_.silenceThreshold, 0.005f, 0.1f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Audio below this level counts as silence.\nCurrent mic level: %.3f", recorder_.peak());
        ImGui::SliderFloat("Silence Duration (sec)", &settings_.silenceDuration, 0.5f, 5.0f, "%.1f");
        tooltip("How long to wait in silence before transcribing the segment.");
        ImGui::SliderFloat("Noise Floor", &settings_.noiseFloor, 0.001f, 0.05f, "%.3f");
        tooltip("Minimum level that counts as speech. Quieter clips are skipped.");
        ImGui::Unindent();
    }
}
