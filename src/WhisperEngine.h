#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <memory>

struct whisper_context;
class SpeakerDiarizer;

class WhisperEngine {
public:
    WhisperEngine();
    ~WhisperEngine();

    bool loadModel(const std::string& modelPath);
    std::string transcribe(const std::string& wavPath);
    std::string transcribeFile(const std::string& audioPath);
    bool isModelLoaded() const { return ctx_ != nullptr; }
    
    // Whisper settings - thread-safe, acquires lock
    void setLanguage(const std::string& lang) { 
        std::lock_guard<std::mutex> lock(mutex_);
        language_ = lang; 
    }
    void setTranslate(bool translate) { 
        std::lock_guard<std::mutex> lock(mutex_);
        translate_ = translate; 
    }
    void setPrintTimestamps(bool print) { 
        std::lock_guard<std::mutex> lock(mutex_);
        printTimestamps_ = print; 
    }
    void setSpeakerDiarization(bool enable) {
        std::lock_guard<std::mutex> lock(mutex_);
        speakerDiarization_ = enable;
    }
    
    // Speaker diarization with sherpa-onnx
    bool initializeSpeakerDiarization(const std::string& segmentationModel,
                                       const std::string& embeddingModel,
                                       int numSpeakers = -1);
    bool isSpeakerDiarizationReady() const;
    void setNumSpeakers(int numSpeakers);

private:
    struct whisper_context* ctx_ = nullptr;
    std::mutex mutex_;
    std::string language_ = "en";
    bool translate_ = false;
    bool printTimestamps_ = false;
    bool speakerDiarization_ = false;

    bool readWav(const std::string& wavPath, std::vector<float>& pcmf32, int& sampleRate, int& channels);
    
    // sherpa-onnx based speaker diarization (production-grade)
    std::unique_ptr<SpeakerDiarizer> diarizer_;
};
