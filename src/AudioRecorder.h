#pragma once

#include <SDL.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <fstream>
#include <thread>
// #include <lame/lame.h>

class AudioRecorder {
public:
    struct DeviceInfo {
        std::string name;
        int index;
    };

    AudioRecorder();
    ~AudioRecorder();

    std::vector<DeviceInfo> getInputDevices();
    bool startRecording(int deviceIndex, const std::string& outputPath, bool useMp3 = true);
    void stopRecording();
    bool isRecording() const { return isRecording_; }
    float getAmplitude() const { return currentAmplitude_; }
    
    // Get peak amplitude from recent samples for silence detection
    float getRecentPeakAmplitude() const { return recentPeakAmplitude_; }
    
    // Check if audio is effectively silent (for noise filtering)
    static bool isAudioSilent(const std::string& wavPath, float threshold = 0.01f);
    
    // Get duration of silence (in seconds) since last sound above threshold
    float getSilenceDuration(float threshold) const;
    
    // Reset recording to new file (for live transcription)
    bool resetToNewFile(const std::string& newOutputPath);

private:
    static void AudioCallback(void* userdata, Uint8* stream, int len);
    void processAudio(const Uint8* stream, int len);
    void writeWavHeader(std::ofstream& file, int sampleRate, int bitsPerSample, int channels, int dataSize);
    void finalizeWavFile();

    SDL_AudioStream* audioStream_ = nullptr;
    SDL_AudioDeviceID deviceId_ = 0;
    std::atomic<bool> isRecording_{false};
    std::string outputPath_;
    std::ofstream outputFile_;

    std::string tempWavPath_ = "temp_recording.wav";
    std::ofstream tempWavFile_;

    // Capture settings
    const int sampleRate_ = 16000;
    const int channels_ = 1;

    // Stats
    std::atomic<float> currentAmplitude_{0.0f};
    std::atomic<float> recentPeakAmplitude_{0.0f};
    std::chrono::steady_clock::time_point lastSoundTime_;

    std::mutex mutex_;
    uint32_t totalBytesRecorded_ = 0;

    // MP3
    // lame_global_flags* lame_ = nullptr;
    // std::vector<unsigned char> mp3Buffer_;
    bool isMp3_ = false;
};
