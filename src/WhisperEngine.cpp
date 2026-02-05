#include "WhisperEngine.h"
#include "SpeakerDiarizer.h"
#include "Logger.h"
#include <whisper.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <thread>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <map>

WhisperEngine::WhisperEngine() : diarizer_(std::make_unique<SpeakerDiarizer>()) {
    // whisper_log_set(nullptr, nullptr); // Disable logs if needed
}

std::string WhisperEngine::transcribeFile(const std::string& audioPath) {
    namespace fs = std::filesystem;
    std::string extension = fs::path(audioPath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == ".wav") {
        std::string result = transcribe(audioPath);
        if (result.find("Error: Unsupported sample rate") == std::string::npos) {
            return result;
        }
    }

    auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path tempPath = fs::temp_directory_path() / ("whispergui_" + std::to_string(timestamp) + ".wav");

    std::string command = "ffmpeg -y -i \"" + audioPath + "\" -ac 1 -ar 16000 -c:a pcm_s16le \"" + tempPath.string() + "\"";
    int ret = system(command.c_str());
    if (ret != 0 || !fs::exists(tempPath)) {
        return "Error: Failed to convert audio file. Please install ffmpeg.";
    }

    std::string result = transcribe(tempPath.string());
    try {
        fs::remove(tempPath);
    } catch (...) {
    }
    return result;
}

WhisperEngine::~WhisperEngine() {
    if (ctx_) {
        whisper_free(ctx_);
    }
}

bool WhisperEngine::loadModel(const std::string& modelPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
    }

    LOG_INFO("Loading whisper model: " + modelPath);

    struct whisper_context_params cparams = whisper_context_default_params();
    // cparams.use_gpu = true; // if available, auto-detected usually

    ctx_ = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (!ctx_) {
        LOG_ERROR("Failed to initialize whisper context from " + modelPath);
        std::cerr << "Failed to initialize whisper context from " << modelPath << std::endl;
        return false;
    }
    LOG_INFO("Whisper model loaded successfully");
    return true;
}

std::string WhisperEngine::transcribe(const std::string& wavPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ctx_) return "Error: Model not loaded.";

    std::vector<float> pcmf32;
    int sampleRate = 0;
    int channels = 0;

    if (!readWav(wavPath, pcmf32, sampleRate, channels)) {
        return "Error: Failed to read WAV file.";
    }

    if (sampleRate != 16000) {
        // Simple decimation or error if rate is wrong.
        // AudioRecorder records at 16k, so this should match.
        return "Error: Unsupported sample rate. Please record at 16kHz.";
    }

    // If speaker diarization is enabled and initialized, run it first
    std::vector<SpeakerSegment> diarizationSegments;
    if (speakerDiarization_ && diarizer_ && diarizer_->isInitialized()) {
        diarizationSegments = diarizer_->process(pcmf32.data(), 
                                                   static_cast<int>(pcmf32.size()), 
                                                   sampleRate);
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = printTimestamps_;
    wparams.translate = translate_;
    wparams.language = (language_ == "auto") ? nullptr : language_.c_str();
    wparams.n_threads = std::thread::hardware_concurrency();

    if (whisper_full(ctx_, wparams, pcmf32.data(), pcmf32.size()) != 0) {
        return "Error: Transcription failed.";
    }

    std::string result;
    const int n_segments = whisper_full_n_segments(ctx_);
    int lastSpeaker = -1;
    
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text(ctx_, i);
        const int64_t t0 = whisper_full_get_segment_t0(ctx_, i);
        const int64_t t1 = whisper_full_get_segment_t1(ctx_, i);
        
        // Convert whisper timestamps (centiseconds) to seconds
        float segmentStart = static_cast<float>(t0) / 100.0f;
        float segmentEnd = static_cast<float>(t1) / 100.0f;
        float segmentMid = (segmentStart + segmentEnd) / 2.0f;
        
        // Speaker diarization: find which speaker is talking at this segment's midpoint
        if (speakerDiarization_ && !diarizationSegments.empty()) {
            int currentSpeaker = -1;
            
            // Find the speaker segment that contains this whisper segment's midpoint
            for (const auto& seg : diarizationSegments) {
                if (segmentMid >= seg.start && segmentMid <= seg.end) {
                    currentSpeaker = seg.speaker;
                    break;
                }
            }
            
            // If no match found at midpoint, use the closest segment
            if (currentSpeaker == -1) {
                float minDist = std::numeric_limits<float>::max();
                for (const auto& seg : diarizationSegments) {
                    float dist = std::min(std::abs(segmentMid - seg.start), 
                                          std::abs(segmentMid - seg.end));
                    if (dist < minDist) {
                        minDist = dist;
                        currentSpeaker = seg.speaker;
                    }
                }
            }
            
            // Add speaker label if speaker changed
            if (currentSpeaker != lastSpeaker && currentSpeaker >= 0) {
                if (i > 0) result += "\n";
                result += "Speaker " + std::to_string(currentSpeaker + 1) + ": ";
                lastSpeaker = currentSpeaker;
            }
        }
        
        if (printTimestamps_) {
            char timestamp[32];
            snprintf(timestamp, sizeof(timestamp), "[%02d:%02d.%03d --> %02d:%02d.%03d] ",
                     (int)(t0 / 100 / 60), (int)(t0 / 100 % 60), (int)(t0 % 100) * 10,
                     (int)(t1 / 100 / 60), (int)(t1 / 100 % 60), (int)(t1 % 100) * 10);
            result += timestamp;
        }
        result += text;
        if (i < n_segments - 1) result += "\n";
    }

    return result;
}

bool WhisperEngine::readWav(const std::string& wavPath, std::vector<float>& pcmf32, int& sampleRate, int& channels) {
    std::ifstream file(wavPath, std::ios::binary);
    if (!file.is_open()) return false;

    char riffHeader[12];
    file.read(riffHeader, sizeof(riffHeader));
    if (file.gcount() < static_cast<std::streamsize>(sizeof(riffHeader))) return false;

    if (std::strncmp(riffHeader, "RIFF", 4) != 0) return false;
    if (std::strncmp(riffHeader + 8, "WAVE", 4) != 0) return false;

    bool foundFmt = false;
    bool foundData = false;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;
    std::streampos dataOffset = 0;

    while (file && (!foundFmt || !foundData)) {
        char chunkId[4];
        uint32_t chunkSize = 0;

        file.read(chunkId, sizeof(chunkId));
        if (file.gcount() < static_cast<std::streamsize>(sizeof(chunkId))) break;
        file.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));
        if (file.gcount() < static_cast<std::streamsize>(sizeof(chunkSize))) break;

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            uint16_t audioFormat = 0;
            uint16_t channelsRaw = 0;
            uint32_t sampleRateRaw = 0;

            file.read(reinterpret_cast<char*>(&audioFormat), sizeof(audioFormat));
            file.read(reinterpret_cast<char*>(&channelsRaw), sizeof(channelsRaw));
            file.read(reinterpret_cast<char*>(&sampleRateRaw), sizeof(sampleRateRaw));

            if (!file) return false;

            channels = channelsRaw;
            sampleRate = sampleRateRaw;

            file.seekg(6, std::ios::cur); // byteRate (4) + blockAlign (2)
            file.read(reinterpret_cast<char*>(&bitsPerSample), sizeof(bitsPerSample));
            if (!file) return false;

            if (chunkSize > 16) {
                file.seekg(chunkSize - 16, std::ios::cur);
            }
            foundFmt = true;
        } else if (std::strncmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            dataOffset = file.tellg();
            file.seekg(chunkSize, std::ios::cur);
            foundData = true;
        } else {
            file.seekg(chunkSize, std::ios::cur);
        }

        if (chunkSize % 2 == 1) {
            file.seekg(1, std::ios::cur); // pad byte
        }
    }

    if (!foundFmt || !foundData) return false;
    if (bitsPerSample != 16) return false;
    if (channels != 1 && channels != 2) return false;

    const uint32_t bytesPerFrame = static_cast<uint32_t>(channels * (bitsPerSample / 8));
    if (bytesPerFrame == 0) return false;

    const size_t numFrames = static_cast<size_t>(dataSize / bytesPerFrame);
    const size_t maxFrames = 100'000'000; // avoid excessive memory use
    if (numFrames == 0 || numFrames > maxFrames) return false;

    pcmf32.resize(numFrames);

    file.clear();
    file.seekg(dataOffset, std::ios::beg);

    std::vector<int16_t> buffer(4096 * channels);
    size_t framesReadTotal = 0;

    while (framesReadTotal < numFrames && file) {
        const size_t framesRemaining = numFrames - framesReadTotal;
        const size_t framesToRead = std::min(framesRemaining, buffer.size() / channels);
        const size_t bytesToRead = framesToRead * bytesPerFrame;

        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(bytesToRead));
        std::streamsize bytesRead = file.gcount();
        if (bytesRead <= 0) break;

        size_t framesRead = static_cast<size_t>(bytesRead) / bytesPerFrame;
        for (size_t i = 0; i < framesRead; ++i) {
            if (channels == 1) {
                pcmf32[framesReadTotal + i] = static_cast<float>(buffer[i]) / 32768.0f;
            } else {
                size_t idx = i * 2;
                float left = static_cast<float>(buffer[idx]) / 32768.0f;
                float right = static_cast<float>(buffer[idx + 1]) / 32768.0f;
                pcmf32[framesReadTotal + i] = (left + right) / 2.0f;
            }
        }

        framesReadTotal += framesRead;
        if (framesRead == 0) break;
    }

    if (framesReadTotal != numFrames) {
        pcmf32.resize(framesReadTotal);
    }

    return !pcmf32.empty();
}

// =============================================================================
// SPEAKER DIARIZATION (sherpa-onnx based - production-grade)
// =============================================================================
// Using sherpa-onnx (https://github.com/k2-fsa/sherpa-onnx) for speaker diarization
// This provides neural network-based speaker segmentation and embedding extraction
// with state-of-the-art accuracy.
// =============================================================================

bool WhisperEngine::initializeSpeakerDiarization(const std::string& segmentationModel,
                                                   const std::string& embeddingModel,
                                                   int numSpeakers) {
    LOG_INFO("Initializing speaker diarization in WhisperEngine");
    if (!diarizer_) {
        diarizer_ = std::make_unique<SpeakerDiarizer>();
    }
    bool result = diarizer_->initialize(segmentationModel, embeddingModel, numSpeakers);
    if (result) {
        LOG_INFO("Speaker diarization ready in WhisperEngine");
    } else {
        LOG_ERROR("Failed to initialize speaker diarization in WhisperEngine");
    }
    return result;
}

bool WhisperEngine::isSpeakerDiarizationReady() const {
    return diarizer_ && diarizer_->isInitialized();
}

void WhisperEngine::setNumSpeakers(int numSpeakers) {
    if (diarizer_) {
        diarizer_->setNumSpeakers(numSpeakers);
    }
}
