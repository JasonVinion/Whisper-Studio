#include "ModelManager.h"
#include "Logger.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

ModelManager::ModelManager() {
    initModels();
    initSpeakerModels();
    
    // Use absolute paths relative to the executable
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    fs::path exePath(buffer);
    fs::path baseDir = exePath.parent_path();
    
    modelsDir_ = (baseDir / "models").string();
    segmentationModelsDir_ = (baseDir / "models" / "segmentation").string();
    embeddingModelsDir_ = (baseDir / "models" / "embeddings").string();
    
    if (!fs::exists(modelsDir_)) fs::create_directories(modelsDir_);
    if (!fs::exists(segmentationModelsDir_)) fs::create_directories(segmentationModelsDir_);
    if (!fs::exists(embeddingModelsDir_)) fs::create_directories(embeddingModelsDir_);
    
    LOG_INFO("ModelManager initialized with paths:");
    LOG_INFO("  Models: " + modelsDir_);
    LOG_INFO("  Segmentation: " + segmentationModelsDir_);
    LOG_INFO("  Embeddings: " + embeddingModelsDir_);
}

void ModelManager::initModels() {
    // Complete list of all available whisper.cpp models from huggingface
    models_ = {
        // Tiny models
        {"Tiny", "ggml-tiny.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin"},
        {"Tiny.en", "ggml-tiny.en.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"},
        {"Tiny (q5_1)", "ggml-tiny-q5_1.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny-q5_1.bin"},
        {"Tiny.en (q5_1)", "ggml-tiny.en-q5_1.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin"},
        {"Tiny (q8_0)", "ggml-tiny-q8_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny-q8_0.bin"},
        
        // Base models
        {"Base", "ggml-base.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin"},
        {"Base.en", "ggml-base.en.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"},
        {"Base (q5_1)", "ggml-base-q5_1.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base-q5_1.bin"},
        {"Base.en (q5_1)", "ggml-base.en-q5_1.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en-q5_1.bin"},
        {"Base (q8_0)", "ggml-base-q8_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base-q8_0.bin"},
        
        // Small models
        {"Small", "ggml-small.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin"},
        {"Small.en", "ggml-small.en.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin"},
        {"Small (q5_1)", "ggml-small-q5_1.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin"},
        {"Small.en (q5_1)", "ggml-small.en-q5_1.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin"},
        {"Small (q8_0)", "ggml-small-q8_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q8_0.bin"},
        
        // Medium models
        {"Medium", "ggml-medium.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin"},
        {"Medium.en", "ggml-medium.en.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.en.bin"},
        {"Medium (q5_0)", "ggml-medium-q5_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium-q5_0.bin"},
        {"Medium.en (q5_0)", "ggml-medium.en-q5_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.en-q5_0.bin"},
        {"Medium (q8_0)", "ggml-medium-q8_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium-q8_0.bin"},
        
        // Large models
        {"Large v1", "ggml-large-v1.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v1.bin"},
        {"Large v2", "ggml-large-v2.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v2.bin"},
        {"Large v2 (q5_0)", "ggml-large-v2-q5_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v2-q5_0.bin"},
        {"Large v2 (q8_0)", "ggml-large-v2-q8_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v2-q8_0.bin"},
        {"Large v3", "ggml-large-v3.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3.bin"},
        {"Large v3 (q5_0)", "ggml-large-v3-q5_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-q5_0.bin"},
        {"Large v3 Turbo", "ggml-large-v3-turbo.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin"},
        {"Large v3 Turbo (q5_0)", "ggml-large-v3-turbo-q5_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin"},
        {"Large v3 Turbo (q8_0)", "ggml-large-v3-turbo-q8_0.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q8_0.bin"}
    };
}

std::vector<ModelManager::ModelInfo> ModelManager::getAvailableModels() {
    return models_;
}

bool ModelManager::isModelAvailable(const std::string& modelName) {
    std::string path = getModelPath(modelName);
    return fs::exists(path);
}

std::string ModelManager::getModelPath(const std::string& modelName) {
    for (const auto& model : models_) {
        if (model.name == modelName) {
            return (fs::path(modelsDir_) / model.filename).string();
        }
    }
    return "";
}

bool ModelManager::downloadModel(const std::string& modelName) {
    std::string url;
    std::string filename;
    for (const auto& model : models_) {
        if (model.name == modelName) {
            url = model.url;
            filename = model.filename;
            break;
        }
    }

    if (url.empty()) return false;

    std::string outputPath = (fs::path(modelsDir_) / filename).string();
    
    downloadProgress_.currentModel = modelName;
    downloadProgress_.startTime = std::chrono::steady_clock::now();
    downloadProgress_.isDownloading = true;
    
    bool success = downloadFile(url, outputPath);
    
    downloadProgress_.isDownloading = false;
    return success && fs::exists(outputPath);
}

void ModelManager::initSpeakerModels() {
    // Speaker diarization models from sherpa-onnx
    // Separated into Segmentation and Embedding categories
    speakerModels_ = {
        // Segmentation models (Pyannote-based)
        {"Pyannote Segmentation 3.0", "sherpa-onnx-pyannote-segmentation-3-0",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2",
         SpeakerModelType::Segmentation, true, "model.onnx"},
        
        // Speaker embedding models
        {"3D-Speaker (ERes2Net Base)", "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx",
         SpeakerModelType::Embedding, false, ""},
        {"WeSpeaker ResNet34 (VoxCeleb)", "wespeaker_en_voxceleb_resnet34.onnx",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/wespeaker_en_voxceleb_resnet34.onnx",
         SpeakerModelType::Embedding, false, ""},
        {"WeSpeaker ResNet34 (CnCeleb)", "wespeaker_zh_cnceleb_resnet34.onnx",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/wespeaker_zh_cnceleb_resnet34.onnx",
         SpeakerModelType::Embedding, false, ""},
    };
}

std::vector<ModelManager::SpeakerModelInfo> ModelManager::getAllSpeakerModels() {
    return speakerModels_;
}

std::vector<ModelManager::SpeakerModelInfo> ModelManager::getSegmentationModels() {
    std::vector<SpeakerModelInfo> result;
    for (const auto& model : speakerModels_) {
        if (model.type == SpeakerModelType::Segmentation) {
            result.push_back(model);
        }
    }
    return result;
}

std::vector<ModelManager::SpeakerModelInfo> ModelManager::getEmbeddingModels() {
    std::vector<SpeakerModelInfo> result;
    for (const auto& model : speakerModels_) {
        if (model.type == SpeakerModelType::Embedding) {
            result.push_back(model);
        }
    }
    return result;
}

bool ModelManager::isSpeakerModelAvailable(const std::string& modelName) {
    std::string path = getActualModelFilePath(modelName);
    if (path.empty()) {
        LOG_DEBUG("isSpeakerModelAvailable: path is empty for " + modelName);
        return false;
    }
    
    bool exists = fs::exists(path);
    if (!exists) {
        LOG_DEBUG("isSpeakerModelAvailable: path not found: " + path);
    }
    return exists;
}

std::string ModelManager::getSpeakerModelPath(const std::string& modelName) {
    for (const auto& model : speakerModels_) {
        if (model.name == modelName) {
            std::string baseDir = (model.type == SpeakerModelType::Segmentation) ? 
                                  segmentationModelsDir_ : embeddingModelsDir_;
            return (fs::path(baseDir) / model.filename).string();
        }
    }
    return "";
}

std::string ModelManager::getActualModelFilePath(const std::string& modelName) {
    for (const auto& model : speakerModels_) {
        if (model.name == modelName) {
            std::string baseDir = (model.type == SpeakerModelType::Segmentation) ? 
                                  segmentationModelsDir_ : embeddingModelsDir_;
            if (model.isArchive && !model.modelFile.empty()) {
                // Return path to the actual model file within the extracted folder
                return (fs::path(baseDir) / model.filename / model.modelFile).string();
            } else {
                // For non-archives, the path is the model file itself
                return (fs::path(baseDir) / model.filename).string();
            }
        }
    }
    return "";
}

bool ModelManager::downloadSpeakerModel(const std::string& modelName) {
    std::string url;
    std::string filename;
    bool isArchive = false;
    SpeakerModelType type = SpeakerModelType::Segmentation;
    
    for (const auto& model : speakerModels_) {
        if (model.name == modelName) {
            url = model.url;
            filename = model.filename;
            isArchive = model.isArchive;
            type = model.type;
            break;
        }
    }
    
    if (url.empty()) return false;
    
    LOG_INFO("Starting download for speaker model: " + modelName);
    
    downloadProgress_.currentModel = modelName;
    downloadProgress_.startTime = std::chrono::steady_clock::now();
    downloadProgress_.isDownloading = true;
    
    std::string baseDir = (type == SpeakerModelType::Segmentation) ? 
                          segmentationModelsDir_ : embeddingModelsDir_;
    
    std::string outputPath;
    if (isArchive) {
        outputPath = (fs::path(baseDir) / (filename + ".tar.bz2")).string();
    } else {
        outputPath = (fs::path(baseDir) / filename).string();
    }
    
    bool success = downloadFile(url, outputPath);
    
    if (success) {
        if (isArchive) {
            LOG_INFO("Download complete, extracting " + outputPath);
            success = extractArchive(outputPath, baseDir);
            // Clean up archive after extraction
            try { fs::remove(outputPath); } catch (...) {}
        }
        LOG_INFO("Speaker model ready: " + modelName);
    } else {
        LOG_ERROR("Failed to download speaker model: " + modelName);
    }
    
    downloadProgress_.isDownloading = false;
    return success;
}

bool ModelManager::downloadFile(const std::string& url, const std::string& outputPath) {
LOG_INFO("Starting download: " + url);
LOG_INFO("Output path: " + outputPath);
std::cout << "Downloading: " << url << std::endl;
std::cout << "To: " << outputPath << std::endl;
    
// Parse URL
std::wstring wUrl(url.begin(), url.end());
    
    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;
    
    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &urlComp)) {
        std::cerr << "Failed to parse URL" << std::endl;
        return false;
    }
    
    // Open session
    HINTERNET hSession = WinHttpOpen(L"WhisperGUI/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        std::cerr << "WinHttpOpen failed" << std::endl;
        return false;
    }
    
    // Connect
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        std::cerr << "WinHttpConnect failed" << std::endl;
        return false;
    }
    
    // Open request
    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
                                             NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        std::cerr << "WinHttpOpenRequest failed" << std::endl;
        return false;
    }
    
    // Handle redirects (for GitHub releases)
    DWORD dwOption = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &dwOption, sizeof(dwOption));
    
    // Send request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        std::cerr << "WinHttpSendRequest failed" << std::endl;
        return false;
    }
    
    // Receive response
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        std::cerr << "WinHttpReceiveResponse failed" << std::endl;
        return false;
    }
    
    // Get content length
    DWORD dwSize = sizeof(DWORD);
    DWORD dwContentLength = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &dwContentLength, &dwSize, NULL);
    downloadProgress_.totalBytes = static_cast<double>(dwContentLength);
    downloadProgress_.bytesDownloaded = 0;
    
    // Open output file
    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        std::cerr << "Failed to create output file" << std::endl;
        return false;
    }
    
    // Download data
    char buffer[8192];
    DWORD dwDownloaded = 0;
    DWORD dwBytesRead = 0;
    auto lastSpeedCheck = std::chrono::steady_clock::now();
    double bytesInInterval = 0;
    
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;
        
        DWORD toRead = (dwSize > sizeof(buffer)) ? sizeof(buffer) : dwSize;
        if (!WinHttpReadData(hRequest, buffer, toRead, &dwBytesRead)) break;
        
        outFile.write(buffer, dwBytesRead);
        dwDownloaded += dwBytesRead;
        bytesInInterval += dwBytesRead;
        downloadProgress_.bytesDownloaded = static_cast<double>(dwDownloaded);
        
        // Update speed every 500ms
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSpeedCheck).count();
        if (elapsed >= 500) {
            downloadProgress_.downloadSpeed = bytesInInterval / (elapsed / 1000.0);
            bytesInInterval = 0;
            lastSpeedCheck = now;
        }
    } while (dwBytesRead > 0);
    
    outFile.close();
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    bool success = dwDownloaded > 0;
    if (success) {
        LOG_INFO("Download completed: " + std::to_string(dwDownloaded) + " bytes");
    } else {
        LOG_ERROR("Download failed: 0 bytes downloaded");
    }
    return success;
}

bool ModelManager::extractArchive(const std::string& archivePath, const std::string& destDir) {
    LOG_INFO("Extracting archive: " + archivePath);
    std::cout << "Extracting: " << archivePath << std::endl;
    
    // Use tar command (available on Windows 10+) with CREATE_NO_WINDOW
    std::string command = "tar -xjf \"" + archivePath + "\" -C \"" + destDir + "\"";
    
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    std::string cmdLine = "cmd.exe /C " + command;
    
    if (!CreateProcessA(NULL, const_cast<char*>(cmdLine.c_str()), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        LOG_ERROR("Failed to run tar extraction command");
        std::cerr << "Failed to run tar extraction" << std::endl;
        return false;
    }
    
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    if (exitCode == 0) {
        LOG_INFO("Extraction successful");
    } else {
        LOG_ERROR("Extraction failed with exit code: " + std::to_string(exitCode));
    }
    
    return exitCode == 0;
}
