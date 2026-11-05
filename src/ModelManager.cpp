#include "ModelManager.h"
#include "Util.h"
#include <winhttp.h>
#include <cwchar>
#include <chrono>
#include <memory>

namespace fs = std::filesystem;

namespace {
struct InetCloser {
    void operator()(HINTERNET h) const { WinHttpCloseHandle(h); }
};
using Inet = std::unique_ptr<void, InetCloser>;
} // namespace

ModelManager::ModelManager() {
    // Order matters: settings.json stores the selected whisper model by index.
    const char* whisper[][2] = {
        {"Tiny", "tiny"}, {"Tiny.en", "tiny.en"}, {"Tiny (q5_1)", "tiny-q5_1"}, {"Tiny.en (q5_1)", "tiny.en-q5_1"},
        {"Tiny (q8_0)", "tiny-q8_0"}, {"Base", "base"}, {"Base.en", "base.en"}, {"Base (q5_1)", "base-q5_1"},
        {"Base.en (q5_1)", "base.en-q5_1"}, {"Base (q8_0)", "base-q8_0"}, {"Small", "small"}, {"Small.en", "small.en"},
        {"Small (q5_1)", "small-q5_1"}, {"Small.en (q5_1)", "small.en-q5_1"}, {"Small (q8_0)", "small-q8_0"},
        {"Medium", "medium"}, {"Medium.en", "medium.en"}, {"Medium (q5_0)", "medium-q5_0"},
        {"Medium.en (q5_0)", "medium.en-q5_0"}, {"Medium (q8_0)", "medium-q8_0"}, {"Large v1", "large-v1"},
        {"Large v2", "large-v2"}, {"Large v2 (q5_0)", "large-v2-q5_0"}, {"Large v2 (q8_0)", "large-v2-q8_0"},
        {"Large v3", "large-v3"}, {"Large v3 (q5_0)", "large-v3-q5_0"}, {"Large v3 Turbo", "large-v3-turbo"},
        {"Large v3 Turbo (q5_0)", "large-v3-turbo-q5_0"}, {"Large v3 Turbo (q8_0)", "large-v3-turbo-q8_0"},
    };
    for (auto& w : whisper) {
        std::string file = std::string("ggml-") + w[1] + ".bin";
        whisper_.push_back({w[0], file, "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/" + file});
    }

    const std::string sherpa = "https://github.com/k2-fsa/sherpa-onnx/releases/download/";
    speaker_.push_back({"Pyannote Segmentation 3.0", "sherpa-onnx-pyannote-segmentation-3-0",
                        sherpa + "speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2",
                        Kind::Segmentation, "model.onnx"});
    const char* embeddings[][2] = {
        {"3D-Speaker (ERes2Net Base)", "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx"},
        {"WeSpeaker ResNet34 (VoxCeleb)", "wespeaker_en_voxceleb_resnet34.onnx"},
        {"WeSpeaker ResNet34 (CnCeleb)", "wespeaker_zh_cnceleb_resnet34.onnx"},
    };
    for (auto& e : embeddings) // "recongition" is the real (misspelled) sherpa-onnx release tag
        speaker_.push_back({e[0], e[1], sherpa + "speaker-recongition-models/" + e[1], Kind::Embedding});

    const fs::path root = fs::absolute("models");
    const fs::path dirs[] = {root, root / "segmentation", root / "embeddings"};
    for (int i = 0; i < 3; ++i) {
        std::error_code ec;
        fs::create_directories(dirs[i], ec);
        dirs_[i] = dirs[i].string();
    }
    LOG_INFO("Models directory: " + dirs_[0]);
}

const ModelManager::Model* ModelManager::findSpeakerModel(const std::string& name) const {
    for (const auto& m : speaker_)
        if (m.name == name) return &m;
    return nullptr;
}

std::string ModelManager::path(const Model& m) const {
    fs::path p = fs::path(dirs_[static_cast<int>(m.kind)]) / m.file;
    if (!m.archiveEntry.empty()) p /= m.archiveEntry;
    return p.string();
}

bool ModelManager::isAvailable(const Model& m) const {
    std::error_code ec;
    return fs::exists(path(m), ec);
}

bool ModelManager::download(const Model& m) {
    LOG_INFO("Downloading model: " + m.name);
    if (m.archiveEntry.empty()) return downloadFile(m.url, path(m));

    const std::string dir = dirs_[static_cast<int>(m.kind)];
    const std::string archive = (fs::path(dir) / (m.file + ".tar.bz2")).string();
    bool ok = downloadFile(m.url, archive) && runHidden("tar -xjf \"" + archive + "\" -C \"" + dir + "\"") == 0 &&
              isAvailable(m);
    std::error_code ec;
    fs::remove(archive, ec);
    if (!ok) LOG_ERROR("Failed to extract " + archive);
    return ok;
}

// Downloads to "<out>.part" and renames it only once the whole file has arrived, so an interrupted or
// failed download is never mistaken for a usable model.
bool ModelManager::downloadFile(const std::string& url, const std::string& out) {
    progress_.done = progress_.total = progress_.speed = 0;
    std::wstring wurl(url.begin(), url.end());
    wchar_t host[256], urlPath[2048];
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = urlPath;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;

    // WinHTTP follows redirects (GitHub / Hugging Face CDNs) by default, except HTTPS -> HTTP.
    Inet session(WinHttpOpen(L"WhisperStudio/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0));
    Inet connection(session ? WinHttpConnect(session.get(), host, uc.nPort, 0) : nullptr);
    Inet request(connection ? WinHttpOpenRequest(connection.get(), L"GET", urlPath, nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                            : nullptr);
    DWORD status = 0, size = sizeof status;
    if (!request ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr) ||
        !WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        LOG_ERROR("Download failed: " + url + " (HTTP " + std::to_string(status) + ", error " +
                  std::to_string(GetLastError()) + ")");
        return false;
    }
    wchar_t lengthText[32];
    size = sizeof lengthText;
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
                            &size, WINHTTP_NO_HEADER_INDEX))
        total = std::wcstoull(lengthText, nullptr, 10); // read as text: models can exceed 4 GB
    progress_.total = static_cast<double>(total);

    const std::string part = out + ".part";
    std::ofstream file(part, std::ios::binary);
    std::vector<char> buf(1 << 16);
    uint64_t received = 0, sinceTick = 0;
    auto tick = std::chrono::steady_clock::now();
    DWORD n = 0;
    while (!cancel_ && file && WinHttpReadData(request.get(), buf.data(), static_cast<DWORD>(buf.size()), &n) && n) {
        file.write(buf.data(), n);
        received += n;
        sinceTick += n;
        progress_.done = static_cast<double>(received);
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - tick).count();
        if (dt >= 0.5) {
            progress_.speed = sinceTick / dt;
            sinceTick = 0;
            tick = now;
        }
    }
    file.close();

    std::error_code ec;
    const bool complete = !cancel_ && file && n == 0 && received > 0 && (total == 0 || received == total);
    if (complete) fs::rename(part, out, ec);
    if (!complete || ec) {
        fs::remove(part, ec);
        LOG_ERROR("Download incomplete: " + url + " (" + std::to_string(received) + " of " + std::to_string(total) +
                  " bytes)");
        return false;
    }
    LOG_INFO("Downloaded " + std::to_string(received) + " bytes to " + out);
    return true;
}
