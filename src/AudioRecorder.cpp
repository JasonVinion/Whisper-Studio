#include "AudioRecorder.h"
#include "Util.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
constexpr uint32_t kRate = 16000;

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct WavHeader { // canonical 44-byte PCM header (all fields naturally aligned, so no padding)
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t riffSize;
    char wave[4] = {'W', 'A', 'V', 'E'}, fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t format = 1, channels = 1;
    uint32_t rate = kRate, byteRate = kRate * 2;
    uint16_t blockAlign = 2, bits = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize;
};
static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");

void writeHeader(FILE* f, uint32_t dataBytes) {
    WavHeader h;
    h.riffSize = 36 + dataBytes;
    h.dataSize = dataBytes;
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(&h, sizeof h, 1, f);
}
} // namespace

AudioRecorder::AudioRecorder() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) LOG_ERROR(std::string("SDL audio init failed: ") + SDL_GetError());
}

AudioRecorder::~AudioRecorder() {
    stop();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

std::vector<std::string> AudioRecorder::inputDevices() {
    std::vector<std::string> out;
    for (int i = 0, n = SDL_GetNumAudioDevices(1); i < n; ++i) {
        const char* name = SDL_GetAudioDeviceName(i, 1);
        out.emplace_back(name ? name : "Unknown device");
    }
    return out;
}

bool AudioRecorder::start(const std::string& deviceName, const std::string& path) {
    if (recording_ || !openFile(path)) return false;

    SDL_AudioSpec want{}, have{};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = callback;
    want.userdata = this;
    const char* name = deviceName.empty() ? nullptr : deviceName.c_str();
    device_ = SDL_OpenAudioDevice(name, 1, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (!device_ && name) {
        LOG_WARNING("Could not open '" + deviceName + "', falling back to the default microphone");
        device_ = SDL_OpenAudioDevice(nullptr, 1, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
    }
    if (device_) stream_ = SDL_NewAudioStream(have.format, have.channels, have.freq, AUDIO_S16SYS, 1, kRate);
    if (!stream_) {
        LOG_ERROR(std::string("Failed to open audio device: ") + SDL_GetError());
        if (device_) SDL_CloseAudioDevice(device_);
        device_ = 0;
        closeFile();
        std::remove(path.c_str());
        return false;
    }
    buffer_.resize(8192);
    lastSoundMs_ = nowMs();
    recording_ = true;
    SDL_PauseAudioDevice(device_, 0);
    return true;
}

AudioRecorder::Stats AudioRecorder::stop() {
    if (!recording_) return {};
    recording_ = false;
    SDL_CloseAudioDevice(device_); // waits for a running callback to return
    device_ = 0;
    SDL_FreeAudioStream(stream_);
    stream_ = nullptr;
    amplitude_ = peak_ = 0;
    return closeFile();
}

bool AudioRecorder::rollover(const std::string& newPath, Stats& finished) {
    SDL_LockAudioDevice(device_);
    finished = closeFile();
    bool ok = openFile(newPath);
    SDL_UnlockAudioDevice(device_);
    return ok;
}

float AudioRecorder::silenceSeconds() const {
    return (nowMs() - lastSoundMs_) / 1000.0f;
}

bool AudioRecorder::openFile(const std::string& path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        LOG_ERROR("Failed to create " + path);
        return false;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 16);
    writeHeader(file_, 0); // placeholder, rewritten with the real size on close
    return true;
}

AudioRecorder::Stats AudioRecorder::closeFile() {
    Stats s{samples_ ? static_cast<float>(sum_ / samples_) : 0.0f, filePeak_};
    if (file_) {
        writeHeader(file_, dataBytes_);
        std::fclose(file_);
        file_ = nullptr;
    }
    dataBytes_ = 0;
    sum_ = 0;
    samples_ = 0;
    filePeak_ = 0;
    return s;
}

void AudioRecorder::callback(void* self, Uint8* data, int len) {
    auto& r = *static_cast<AudioRecorder*>(self);
    if (!r.recording_ || SDL_AudioStreamPut(r.stream_, data, len) != 0) return;
    int n = SDL_AudioStreamAvailable(r.stream_);
    if (n <= 0) return;
    if (static_cast<size_t>(n) > r.buffer_.size()) r.buffer_.resize(n); // rare; normally reused
    n = SDL_AudioStreamGet(r.stream_, r.buffer_.data(), n);
    if (n <= 0) return;

    if (r.file_) {
        std::fwrite(r.buffer_.data(), 1, n, r.file_);
        r.dataBytes_ += n;
    }
    const auto* s = reinterpret_cast<const int16_t*>(r.buffer_.data());
    const int count = n / 2;
    float sum = 0, peak = 0;
    for (int i = 0; i < count; ++i) {
        float a = std::abs(s[i]) / 32768.0f;
        sum += a;
        peak = std::max(peak, a);
    }
    if (count == 0) return;
    r.sum_ += sum;
    r.samples_ += count;
    r.filePeak_ = std::max(r.filePeak_, peak);
    r.amplitude_ = sum / count;
    r.peak_ = peak;
    if (peak > r.silenceThreshold_) r.lastSoundMs_ = nowMs();
}
