#include "SpeakerDiarizer.h"
#include "Logger.h"
#include <iostream>
#include <filesystem>
#include <cmath>
#include <cstring>

#if WHISPERGUI_HAS_SHERPA_ONNX
#include "sherpa-onnx/c-api/c-api.h"
#endif

namespace fs = std::filesystem;

// Heuristic thresholds for fallback mode
namespace {
    constexpr float kEnergyChangeThreshold = 0.5f;
    constexpr float kPeakChangeThreshold = 0.4f;
    constexpr float kSilenceAmplitudeThreshold = 0.01f;
    constexpr size_t kNewTranscriptionGapSamples = 16000;
    constexpr size_t kSilenceCheckWindowSamples = 8000;
}

SpeakerDiarizer::SpeakerDiarizer() {
    // Constructor - initialization happens in initialize()
}

SpeakerDiarizer::~SpeakerDiarizer() {
#if WHISPERGUI_HAS_SHERPA_ONNX
    std::lock_guard<std::mutex> lock(mutex_);
    if (diarizer_) {
        SherpaOnnxDestroyOfflineSpeakerDiarization(diarizer_);
        diarizer_ = nullptr;
    }
#endif
}

bool SpeakerDiarizer::isInitialized() const {
#if WHISPERGUI_HAS_SHERPA_ONNX
    return diarizer_ != nullptr;
#else
    return initialized_;
#endif
}

bool SpeakerDiarizer::isUsingNeuralDiarization() {
#if WHISPERGUI_HAS_SHERPA_ONNX
    return true;
#else
    return false;
#endif
}

bool SpeakerDiarizer::initialize(const std::string& segmentationModel,
                                  const std::string& embeddingModel,
                                  int numSpeakers) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    numSpeakers_ = numSpeakers;
    
    LOG_INFO("Initializing speaker diarizer");
    LOG_INFO("  Segmentation model: " + segmentationModel);
    LOG_INFO("  Embedding model: " + embeddingModel);
    
#if WHISPERGUI_HAS_SHERPA_ONNX
    // Clean up existing diarizer
    if (diarizer_) {
        SherpaOnnxDestroyOfflineSpeakerDiarization(diarizer_);
        diarizer_ = nullptr;
    }
    
    // Verify model files exist
    if (!fs::exists(segmentationModel)) {
        LOG_ERROR("Segmentation model not found: " + segmentationModel);
        std::cerr << "Segmentation model not found: " << segmentationModel << std::endl;
        return false;
    }
    if (!fs::exists(embeddingModel)) {
        LOG_ERROR("Embedding model not found: " + embeddingModel);
        std::cerr << "Embedding model not found: " << embeddingModel << std::endl;
        return false;
    }
    
    // Configure the diarization pipeline
    SherpaOnnxOfflineSpeakerDiarizationConfig config;
    memset(&config, 0, sizeof(config));
    
    // Segmentation model config (pyannote-based)
    config.segmentation.pyannote.model = segmentationModel.c_str();
    config.segmentation.num_threads = 2;
    config.segmentation.debug = 0;
    config.segmentation.provider = "cpu";
    
    // Speaker embedding extractor config
    config.embedding.model = embeddingModel.c_str();
    config.embedding.num_threads = 2;
    config.embedding.debug = 0;
    config.embedding.provider = "cpu";
    
    // Clustering config
    config.clustering.num_clusters = numSpeakers;  // -1 for auto
    config.clustering.threshold = clusteringThreshold_;
    
    // Segment filtering
    config.min_duration_on = 0.2f;   // Minimum segment duration in seconds
    config.min_duration_off = 0.5f;  // Minimum gap to merge segments
    
    // Create the diarizer
    diarizer_ = SherpaOnnxCreateOfflineSpeakerDiarization(&config);
    
    if (!diarizer_) {
        LOG_ERROR("Failed to create speaker diarization pipeline");
        std::cerr << "Failed to create speaker diarization pipeline" << std::endl;
        return false;
    }
    
    LOG_INFO("Speaker diarization initialized successfully (sherpa-onnx)");
    std::cout << "Speaker diarization initialized (sherpa-onnx)" << std::endl;
    std::cout << "  Segmentation model: " << segmentationModel << std::endl;
    std::cout << "  Embedding model: " << embeddingModel << std::endl;
    
    return true;
#else
    // Fallback mode - no models needed, just mark as initialized
    LOG_INFO("Speaker diarization initialized (heuristic fallback)");
    std::cout << "Speaker diarization initialized (heuristic fallback)" << std::endl;
    std::cout << "  Note: Build with -DWHISPERGUI_USE_SHERPA_ONNX=ON for neural diarization" << std::endl;
    initialized_ = true;
    return true;
#endif
}

std::vector<SpeakerSegment> SpeakerDiarizer::process(const float* samples, 
                                                       int numSamples, 
                                                       int sampleRate) {
#if WHISPERGUI_HAS_SHERPA_ONNX
    std::vector<SpeakerSegment> segments;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!diarizer_) {
        std::cerr << "Speaker diarizer not initialized" << std::endl;
        return segments;
    }
    
    // Process the audio
    const SherpaOnnxOfflineSpeakerDiarizationResult* result = 
        SherpaOnnxOfflineSpeakerDiarizationProcess(diarizer_, samples, numSamples);
    
    if (!result) {
        std::cerr << "Speaker diarization processing failed" << std::endl;
        return segments;
    }
    
    // Get the number of segments
    int numSegments = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(result);
    int numSpeakers = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSpeakers(result);
    
    LOG_INFO("Diarization found " + std::to_string(numSpeakers) + " speakers in " 
              + std::to_string(numSegments) + " segments");
    
    if (numSegments == 0) {
        LOG_WARNING("Diarization completed but found 0 segments");
    }
    
    // Get segments sorted by start time
    const SherpaOnnxOfflineSpeakerDiarizationSegment* sortedSegments = 
        SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(result);
    
    if (sortedSegments) {
        segments.reserve(numSegments);
        for (int i = 0; i < numSegments; ++i) {
            SpeakerSegment seg;
            seg.start = sortedSegments[i].start;
            seg.end = sortedSegments[i].end;
            seg.speaker = sortedSegments[i].speaker;
            segments.push_back(seg);
        }
        SherpaOnnxOfflineSpeakerDiarizationDestroySegment(sortedSegments);
    } else if (numSegments > 0) {
        LOG_ERROR("Failed to sort diarization segments");
    }
    
    // Clean up result
    SherpaOnnxOfflineSpeakerDiarizationDestroyResult(result);
    
    return segments;
#else
    return processWithHeuristics(samples, numSamples, sampleRate);
#endif
}

std::vector<SpeakerSegment> SpeakerDiarizer::processWithHeuristics(const float* samples, 
                                                                     int numSamples, 
                                                                     int sampleRate) {
    std::vector<SpeakerSegment> segments;
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_) {
        return segments;
    }
    
    // Simple heuristic: analyze audio in chunks and detect energy changes
    const int chunkSize = sampleRate / 2;  // 0.5 second chunks
    const int numChunks = numSamples / chunkSize;
    
    // Reset state for new audio
    lastEnergy_ = 0.0f;
    lastPeak_ = 0.0f;
    silenceCounter_ = 0;
    lastDetectedSpeaker_ = 0;
    
    int currentSpeaker = 0;
    float currentStart = 0.0f;
    bool inSpeech = false;
    
    for (int chunk = 0; chunk < numChunks; ++chunk) {
        const float* chunkStart = samples + chunk * chunkSize;
        
        // Calculate energy for this chunk
        float energy = 0.0f;
        float peakEnergy = 0.0f;
        
        for (int i = 0; i < chunkSize; ++i) {
            float sample = std::abs(chunkStart[i]);
            energy += sample * sample;
            if (sample > peakEnergy) {
                peakEnergy = sample;
            }
        }
        energy = std::sqrt(energy / static_cast<float>(chunkSize));
        
        float chunkTime = static_cast<float>(chunk * chunkSize) / sampleRate;
        
        // Check if this is speech
        bool isSpeech = energy > kSilenceAmplitudeThreshold;
        
        if (isSpeech) {
            if (!inSpeech) {
                // Start of new speech segment
                currentStart = chunkTime;
                inSpeech = true;
            }
            
            // Check for speaker change
            bool energyChanged = lastEnergy_ > 0.0f && 
                                 std::abs(energy - lastEnergy_) / lastEnergy_ > kEnergyChangeThreshold;
            bool peakChanged = lastPeak_ > 0.0f && 
                               std::abs(peakEnergy - lastPeak_) / lastPeak_ > kPeakChangeThreshold;
            
            if (silenceCounter_ >= 2 && (energyChanged || peakChanged)) {
                // End current segment
                if (chunkTime > currentStart + 0.2f) {
                    SpeakerSegment seg;
                    seg.start = currentStart;
                    seg.end = chunkTime;
                    seg.speaker = currentSpeaker;
                    segments.push_back(seg);
                }
                
                // Switch speaker
                currentSpeaker = (currentSpeaker == 0) ? 1 : 0;
                currentStart = chunkTime;
                silenceCounter_ = 0;
            }
        } else {
            silenceCounter_++;
            
            if (inSpeech && silenceCounter_ >= 3) {
                // End of speech segment
                if (chunkTime > currentStart + 0.2f) {
                    SpeakerSegment seg;
                    seg.start = currentStart;
                    seg.end = chunkTime;
                    seg.speaker = currentSpeaker;
                    segments.push_back(seg);
                }
                inSpeech = false;
            }
        }
        
        lastEnergy_ = energy;
        lastPeak_ = peakEnergy;
    }
    
    // Close any remaining segment
    if (inSpeech) {
        float endTime = static_cast<float>(numSamples) / sampleRate;
        if (endTime > currentStart + 0.2f) {
            SpeakerSegment seg;
            seg.start = currentStart;
            seg.end = endTime;
            seg.speaker = currentSpeaker;
            segments.push_back(seg);
        }
    }
    
    return segments;
}

int SpeakerDiarizer::getSampleRate() const {
#if WHISPERGUI_HAS_SHERPA_ONNX
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    if (diarizer_) {
        return SherpaOnnxOfflineSpeakerDiarizationGetSampleRate(diarizer_);
    }
#endif
    return 16000;  // Default expected sample rate
}

void SpeakerDiarizer::setNumSpeakers(int numSpeakers) {
    std::lock_guard<std::mutex> lock(mutex_);
    numSpeakers_ = numSpeakers;
    
#if WHISPERGUI_HAS_SHERPA_ONNX
    if (diarizer_) {
        SherpaOnnxOfflineSpeakerDiarizationConfig config;
        memset(&config, 0, sizeof(config));
        config.clustering.num_clusters = numSpeakers;
        config.clustering.threshold = clusteringThreshold_;
        SherpaOnnxOfflineSpeakerDiarizationSetConfig(diarizer_, &config);
    }
#endif
}

void SpeakerDiarizer::setClusteringThreshold(float threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    clusteringThreshold_ = threshold;
    
#if WHISPERGUI_HAS_SHERPA_ONNX
    if (diarizer_) {
        SherpaOnnxOfflineSpeakerDiarizationConfig config;
        memset(&config, 0, sizeof(config));
        config.clustering.num_clusters = numSpeakers_;
        config.clustering.threshold = threshold;
        SherpaOnnxOfflineSpeakerDiarizationSetConfig(diarizer_, &config);
    }
#endif
}

std::string SpeakerDiarizer::getSegmentationModelUrl() {
    // sherpa-onnx pyannote segmentation model
    return "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2";
}

std::string SpeakerDiarizer::getEmbeddingModelUrl() {
    // 3D-Speaker embedding model from sherpa-onnx
    // Note: "recongition" is the correct spelling as used in sherpa-onnx release tags
    return "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";
}

std::string SpeakerDiarizer::getDefaultModelsDir() {
    // Use the same models directory as whisper models
    return "models/speaker-diarization";
}
