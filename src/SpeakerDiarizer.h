#pragma once
#include <string>
#include <vector>
#include <mutex>

#if WHISPERGUI_HAS_SHERPA_ONNX
// Forward declarations for sherpa-onnx types
struct SherpaOnnxOfflineSpeakerDiarization;
struct SherpaOnnxOfflineSpeakerDiarizationResult;
#endif

// Represents a speaker segment with timing information
struct SpeakerSegment {
    float start;    // Start time in seconds
    float end;      // End time in seconds
    int speaker;    // Speaker ID (0-indexed)
};

// Wrapper class for speaker diarization
// When sherpa-onnx is available (WHISPERGUI_HAS_SHERPA_ONNX=1), uses production-grade
// neural network-based diarization from sherpa-onnx (10k+ stars on GitHub).
// Otherwise, falls back to a basic heuristic-based approach.
class SpeakerDiarizer {
public:
    SpeakerDiarizer();
    ~SpeakerDiarizer();

    // Initialize with model paths (sherpa-onnx mode)
    // segmentationModel: path to pyannote segmentation ONNX model
    // embeddingModel: path to speaker embedding ONNX model
    bool initialize(const std::string& segmentationModel, 
                    const std::string& embeddingModel,
                    int numSpeakers = -1);  // -1 for auto-detect

    // Check if models are loaded and ready
    bool isInitialized() const;

    // Process audio samples and return speaker segments
    // samples: audio samples (normalized to [-1, 1])
    // sampleRate: sample rate of the audio (should be 16000)
    // numSamples: number of samples
    std::vector<SpeakerSegment> process(const float* samples, int numSamples, int sampleRate = 16000);

    // Get the expected sample rate
    int getSampleRate() const;

    // Set clustering parameters
    void setNumSpeakers(int numSpeakers);
    void setClusteringThreshold(float threshold);

    // Get model paths for download URLs
    static std::string getSegmentationModelUrl();
    static std::string getEmbeddingModelUrl();
    static std::string getDefaultModelsDir();
    
    // Check if using sherpa-onnx or fallback
    static bool isUsingNeuralDiarization();

private:
#if WHISPERGUI_HAS_SHERPA_ONNX
    const SherpaOnnxOfflineSpeakerDiarization* diarizer_ = nullptr;
#endif
    std::mutex mutex_;
    int numSpeakers_ = -1;
    float clusteringThreshold_ = 0.5f;
    bool initialized_ = false;
    
    // Fallback heuristic-based detection state
    float lastEnergy_ = 0.0f;
    float lastPeak_ = 0.0f;
    size_t silenceCounter_ = 0;
    int lastDetectedSpeaker_ = 0;
    size_t lastSampleEnd_ = 0;
    
    // Fallback method when sherpa-onnx is not available
    std::vector<SpeakerSegment> processWithHeuristics(const float* samples, int numSamples, int sampleRate);
};
