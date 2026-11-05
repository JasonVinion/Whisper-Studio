#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct whisper_context;
class SpeakerDiarizer;

// whisper.cpp wrapper. setOptions() may be called from any thread at any time; each transcription
// uses a snapshot of the options taken when it starts. loadModel()/transcribeFile() are meant to be
// called from one background thread.
class WhisperEngine {
public:
    struct Options {
        std::string language = "en"; // or "auto"
        bool translate = false, timestamps = false, diarize = false;
        std::string segmentationModel, embeddingModel; // diarization model files (sherpa-onnx builds)
    };

    WhisperEngine();
    ~WhisperEngine();

    bool loadModel(const std::string& path);
    bool isModelLoaded() const { return loaded_; }
    void setOptions(Options o);
    // Any audio format: 16 kHz 16-bit PCM WAV is read directly, everything else goes through ffmpeg.
    // Errors are returned as text starting with "Error:".
    std::string transcribeFile(const std::string& path);
    void cancel() { cancel_ = true; } // aborts the running transcription and all future ones (shutdown)

private:
    std::string transcribe(const std::vector<float>& pcm, const Options& o);

    std::mutex mutex_, optionsMutex_;
    whisper_context* ctx_ = nullptr;
    std::atomic<bool> loaded_{false}, cancel_{false};
    Options options_;
    std::unique_ptr<SpeakerDiarizer> diarizer_; // created on first use, freed when diarization is off
    std::string diarizerModels_;                // model paths diarizer_ was built from
};
