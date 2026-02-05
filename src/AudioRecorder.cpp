#include "AudioRecorder.h"
#include <iostream>
#include <cmath>
#include <algorithm>

AudioRecorder::AudioRecorder() {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL Audio Init Failed: " << SDL_GetError() << std::endl;
    }
    lastSoundTime_ = std::chrono::steady_clock::now();
}

AudioRecorder::~AudioRecorder() {
    stopRecording();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

std::vector<AudioRecorder::DeviceInfo> AudioRecorder::getInputDevices() {
    std::vector<DeviceInfo> devices;
    int count = SDL_GetNumAudioDevices(1); // 1 for input
    for (int i = 0; i < count; ++i) {
        devices.push_back({SDL_GetAudioDeviceName(i, 1), i});
    }
    return devices;
}

bool AudioRecorder::startRecording(int deviceIndex, const std::string& outputPath, bool useMp3) {
    if (isRecording_) return false;

    outputPath_ = outputPath;
    isMp3_ = useMp3;
    currentAmplitude_ = 0.0f;
    recentPeakAmplitude_ = 0.0f;
    lastSoundTime_ = std::chrono::steady_clock::now();

    outputFile_.open(outputPath, std::ios::binary);
    if (!outputFile_.is_open()) {
        std::cerr << "Failed to open output file: " << outputPath << std::endl;
        return false;
    }

    tempWavFile_.open(tempWavPath_, std::ios::binary);
    if (!tempWavFile_.is_open()) {
         std::cerr << "Failed to open temp wav file" << std::endl;
         outputFile_.close();
         return false;
    }
    writeWavHeader(tempWavFile_, sampleRate_, 16, channels_, 0);

    if (isMp3_) {
        // MP3 disabled
        std::cerr << "MP3 support disabled in this build." << std::endl;
        isMp3_ = false; // Fallback to WAV
        writeWavHeader(outputFile_, sampleRate_, 16, channels_, 0);
    } else {
        // Write placeholder WAV header for output file
        writeWavHeader(outputFile_, sampleRate_, 16, channels_, 0);
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = sampleRate_;
    want.format = AUDIO_S16SYS;
    want.channels = channels_;
    want.samples = 1024;
    want.callback = AudioCallback;
    want.userdata = this;

    const char* deviceName = nullptr;
    if (deviceIndex >= 0 && deviceIndex < SDL_GetNumAudioDevices(1)) {
        deviceName = SDL_GetAudioDeviceName(deviceIndex, 1);
    }

    deviceId_ = SDL_OpenAudioDevice(deviceName, 1, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (deviceId_ == 0) {
        std::cerr << "Failed to open audio device: " << SDL_GetError() << std::endl;
        outputFile_.close();
        tempWavFile_.close();
        return false;
    }

    audioStream_ = SDL_NewAudioStream(have.format, have.channels, have.freq,
                                      AUDIO_S16SYS, channels_, sampleRate_);

    if (!audioStream_) {
        std::cerr << "Failed to create audio stream: " << SDL_GetError() << std::endl;
        SDL_CloseAudioDevice(deviceId_);
        outputFile_.close();
        tempWavFile_.close();
        return false;
    }

    totalBytesRecorded_ = 0;
    isRecording_ = true;
    SDL_PauseAudioDevice(deviceId_, 0);
    return true;
}

void AudioRecorder::stopRecording() {
    if (!isRecording_) return;

    if (deviceId_ != 0) {
        SDL_LockAudioDevice(deviceId_);
    }
    isRecording_ = false;
    if (deviceId_ != 0) {
        SDL_UnlockAudioDevice(deviceId_);
    }
    SDL_CloseAudioDevice(deviceId_);
    deviceId_ = 0;

    if (audioStream_) {
        SDL_FreeAudioStream(audioStream_);
        audioStream_ = nullptr;
    }

    // Finalize temp wav
    finalizeWavFile(); // Refactored below to handle tempWavFile_
    tempWavFile_.close();

    if (false /*isMp3_ && lame_*/) {
        // ...
    } else {
        // Finalize output wav
        if (outputFile_.is_open()) {
            outputFile_.seekp(0, std::ios::beg);
            writeWavHeader(outputFile_, sampleRate_, 16, channels_, totalBytesRecorded_);
        }
    }

    outputFile_.close();
    currentAmplitude_ = 0.0f;
}

void AudioRecorder::AudioCallback(void* userdata, Uint8* stream, int len) {
    auto* recorder = static_cast<AudioRecorder*>(userdata);

    if (recorder->audioStream_) {
        if (SDL_AudioStreamPut(recorder->audioStream_, stream, len) == 0) {
            int available = SDL_AudioStreamAvailable(recorder->audioStream_);
            if (available > 0) {
                std::vector<Uint8> buffer(available);
                int bytesRead = SDL_AudioStreamGet(recorder->audioStream_, buffer.data(), available);
                if (bytesRead > 0) {
                    recorder->processAudio(buffer.data(), bytesRead);
                }
            }
        }
    }
}

void AudioRecorder::processAudio(const Uint8* stream, int len) {
    if (!isRecording_) return;

    // Write to temp WAV
    tempWavFile_.write(reinterpret_cast<const char*>(stream), len);

    // int nsamples = len / 2;

    if (false /*isMp3_*/) {
        // ...
    } else {
        outputFile_.write(reinterpret_cast<const char*>(stream), len);
    }

    totalBytesRecorded_ += len;

    const int16_t* samples = reinterpret_cast<const int16_t*>(stream);
    int sampleCount = len / 2;
    float sum = 0;
    float peak = 0;
    for (int i = 0; i < sampleCount; ++i) {
        float sample = std::abs(samples[i]) / 32768.0f;
        sum += sample;
        peak = std::max(peak, sample);
    }
    if (sampleCount > 0) {
        currentAmplitude_ = sum / sampleCount;
        recentPeakAmplitude_ = peak;
        
        // Update last sound time if amplitude is above a minimum threshold
        if (peak > 0.01f) {
            lastSoundTime_ = std::chrono::steady_clock::now();
        }
    }
}

void AudioRecorder::writeWavHeader(std::ofstream& file, int sampleRate, int bitsPerSample, int channels, int dataSize) {
    file << "RIFF";
    uint32_t fileSize = 36 + dataSize;
    file.write(reinterpret_cast<const char*>(&fileSize), 4);
    file << "WAVE";
    file << "fmt ";
    uint32_t fmtSize = 16;
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    uint16_t audioFormat = 1; // PCM
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    uint16_t numChannels = channels;
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    uint32_t sampleRate32 = sampleRate;
    file.write(reinterpret_cast<const char*>(&sampleRate32), 4);
    uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    uint16_t blockAlign = channels * bitsPerSample / 8;
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    uint16_t bitsPerSample16 = bitsPerSample;
    file.write(reinterpret_cast<const char*>(&bitsPerSample16), 2);
    file << "data";
    uint32_t dataSize32 = dataSize;
    file.write(reinterpret_cast<const char*>(&dataSize32), 4);
}

void AudioRecorder::finalizeWavFile() {
    // Only for temp file, logic reused
    if (tempWavFile_.is_open()) {
        tempWavFile_.seekp(0, std::ios::beg);
        writeWavHeader(tempWavFile_, sampleRate_, 16, channels_, totalBytesRecorded_);
    }
}

float AudioRecorder::getSilenceDuration(float threshold) const {
    if (recentPeakAmplitude_ > threshold) {
        return 0.0f;
    }
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - lastSoundTime_).count();
}

bool AudioRecorder::isAudioSilent(const std::string& wavPath, float threshold) {
    std::ifstream file(wavPath, std::ios::binary);
    if (!file.is_open()) return true;
    
    // Skip WAV header
    file.seekg(44, std::ios::beg);
    
    // Read samples and check amplitude
    std::vector<int16_t> buffer(4096);
    float maxAmplitude = 0.0f;
    int totalSamples = 0;
    float totalAmplitude = 0.0f;
    
    while (file.read(reinterpret_cast<char*>(buffer.data()), buffer.size() * sizeof(int16_t))) {
        size_t samplesRead = file.gcount() / sizeof(int16_t);
        for (size_t i = 0; i < samplesRead; ++i) {
            float amplitude = std::abs(buffer[i]) / 32768.0f;
            maxAmplitude = std::max(maxAmplitude, amplitude);
            totalAmplitude += amplitude;
            totalSamples++;
        }
    }
    
    // Also check remaining bytes
    size_t remaining = file.gcount() / sizeof(int16_t);
    for (size_t i = 0; i < remaining; ++i) {
        float amplitude = std::abs(buffer[i]) / 32768.0f;
        maxAmplitude = std::max(maxAmplitude, amplitude);
        totalAmplitude += amplitude;
        totalSamples++;
    }
    
    if (totalSamples == 0) return true;
    
    float avgAmplitude = totalAmplitude / totalSamples;
    // Consider silent if both average and peak are below threshold
    return (avgAmplitude < threshold && maxAmplitude < threshold * 3.0f);
}

bool AudioRecorder::resetToNewFile(const std::string& newOutputPath) {
    if (!isRecording_) return false;

    const bool shouldLockDevice = deviceId_ != 0;
    if (shouldLockDevice) {
        SDL_LockAudioDevice(deviceId_);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Finalize current output file
    if (outputFile_.is_open()) {
        outputFile_.seekp(0, std::ios::beg);
        writeWavHeader(outputFile_, sampleRate_, 16, channels_, totalBytesRecorded_);
        outputFile_.close();
    }
    
    // Store the old path for transcription
    std::string oldPath = outputPath_;
    
    // Reset counters
    totalBytesRecorded_ = 0;
    outputPath_ = newOutputPath;
    currentAmplitude_ = 0.0f;
    recentPeakAmplitude_ = 0.0f;
    lastSoundTime_ = std::chrono::steady_clock::now();
    
    // Open new output file
    outputFile_.open(outputPath_, std::ios::binary);
    if (!outputFile_.is_open()) {
        std::cerr << "Failed to open new output file: " << outputPath_ << std::endl;
        if (shouldLockDevice) {
            SDL_UnlockAudioDevice(deviceId_);
        }
        return false;
    }
    
    // Write placeholder WAV header
    writeWavHeader(outputFile_, sampleRate_, 16, channels_, 0);
    
    if (shouldLockDevice) {
        SDL_UnlockAudioDevice(deviceId_);
    }
    return true;
}
