#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>

// Model categories for speaker diarization
enum class SpeakerModelType {
    Segmentation,  // Pyannote-style segmentation models
    Embedding      // Speaker embedding models (3D-Speaker, WeSpeaker, etc.)
};

class ModelManager {
public:
    struct ModelInfo {
        std::string name;
        std::string filename;
        std::string url;
        bool isArchive = false; // True if the model is a tar.bz2 archive
    };
    
    struct SpeakerModelInfo {
        std::string name;
        std::string filename;
        std::string url;
        SpeakerModelType type;
        bool isArchive = false;
        std::string modelFile; // Actual model file within extracted folder (for archives)
    };
    
    // Download progress information
    struct DownloadProgress {
        std::atomic<bool> isDownloading{false};
        std::atomic<double> bytesDownloaded{0};
        std::atomic<double> totalBytes{0};
        std::atomic<double> downloadSpeed{0}; // bytes per second
        std::chrono::steady_clock::time_point startTime;
        std::string currentModel;
    };

    ModelManager();

    std::vector<ModelInfo> getAvailableModels();
    bool isModelAvailable(const std::string& modelName);
    // Blocking download for simplicity, or we launch a thread.
    // Uses WinHTTP to avoid console windows.
    bool downloadModel(const std::string& modelName);
    std::string getModelPath(const std::string& modelName);
    
    // Speaker diarization models - separate segmentation and embedding
    std::vector<SpeakerModelInfo> getSegmentationModels();
    std::vector<SpeakerModelInfo> getEmbeddingModels();
    std::vector<SpeakerModelInfo> getAllSpeakerModels();
    bool isSpeakerModelAvailable(const std::string& modelName);
    bool downloadSpeakerModel(const std::string& modelName);
    std::string getSpeakerModelPath(const std::string& modelName);
    std::string getActualModelFilePath(const std::string& modelName); // Get the .onnx file path
    
    // Download progress tracking
    DownloadProgress& getDownloadProgress() { return downloadProgress_; }

private:
std::string modelsDir_ = "models";
std::string segmentationModelsDir_ = "models/segmentation";
std::string embeddingModelsDir_ = "models/embeddings";
std::vector<ModelInfo> models_;
std::vector<SpeakerModelInfo> speakerModels_;
DownloadProgress downloadProgress_;
void initModels();
void initSpeakerModels();
    
    // Download file using WinHTTP (no console window)
    bool downloadFile(const std::string& url, const std::string& outputPath);
    // Extract tar.bz2 archive
    bool extractArchive(const std::string& archivePath, const std::string& destDir);
};
