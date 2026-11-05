#pragma once
#include <atomic>
#include <string>
#include <vector>

// Catalog of downloadable whisper.cpp and sherpa-onnx diarization models, stored under ./models.
class ModelManager {
public:
    enum class Kind { Whisper, Segmentation, Embedding };
    struct Model {
        std::string name, file, url;
        Kind kind = Kind::Whisper;
        std::string archiveEntry; // set when url is a .tar.bz2 that unpacks to folder `file` holding this file
    };
    struct Progress { std::atomic<double> done{0}, total{0}, speed{0}; }; // bytes, bytes (0 = unknown), bytes/s

    ModelManager();
    const std::vector<Model>& whisperModels() const { return whisper_; }
    const std::vector<Model>& speakerModels() const { return speaker_; }
    const Model* findSpeakerModel(const std::string& name) const;
    std::string path(const Model& m) const; // the file whisper.cpp / sherpa-onnx loads
    bool isAvailable(const Model& m) const;

    bool download(const Model& m); // blocking; run it on a background thread
    void cancel() { cancel_ = true; } // stops the current and all later downloads (shutdown)
    const Progress& progress() const { return progress_; }

private:
    bool downloadFile(const std::string& url, const std::string& out);

    std::vector<Model> whisper_, speaker_;
    std::string dirs_[3]; // per Kind
    Progress progress_;
    std::atomic<bool> cancel_{false};
};
