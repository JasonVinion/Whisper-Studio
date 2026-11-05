#pragma once
#include <SDL.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Records 16 kHz mono 16-bit PCM from an SDL capture device straight into a WAV file.
class AudioRecorder {
public:
    struct Stats { float avg = 0, peak = 0; }; // amplitude (0..1) of a finished file

    AudioRecorder();
    ~AudioRecorder();

    static std::vector<std::string> inputDevices();
    bool start(const std::string& deviceName, const std::string& path); // empty name = default device
    Stats stop();
    // Live mode: finish the current file and keep recording into newPath without a gap.
    // Returns false if newPath could not be created (recording continues but is not saved).
    bool rollover(const std::string& newPath, Stats& finished);

    bool isRecording() const { return recording_; }
    float amplitude() const { return amplitude_; } // mean level of the latest audio block
    float peak() const { return peak_; }           // peak level of the latest audio block
    void setSilenceThreshold(float t) { silenceThreshold_ = t; }
    float silenceSeconds() const; // time since the peak last rose above the silence threshold

private:
    static void callback(void* self, Uint8* data, int len);
    bool openFile(const std::string& path);
    Stats closeFile();

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioStream* stream_ = nullptr; // converts whatever the device delivers to 16 kHz mono s16
    std::vector<Uint8> buffer_;
    // Only touched by the audio thread, or while the device is closed/locked:
    FILE* file_ = nullptr;
    uint32_t dataBytes_ = 0;
    double sum_ = 0;
    uint64_t samples_ = 0;
    float filePeak_ = 0;

    std::atomic<bool> recording_{false};
    std::atomic<float> amplitude_{0}, peak_{0}, silenceThreshold_{0.02f};
    std::atomic<int64_t> lastSoundMs_{0};
};
