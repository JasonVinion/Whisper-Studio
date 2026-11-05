#include "WhisperEngine.h"
#include "SpeakerDiarizer.h"
#include "Util.h"
#include <whisper.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
// Reads a 16 kHz, 16-bit PCM WAV (mono or stereo) as mono floats. Returns false for anything else.
bool readWav16k(const std::string& path, std::vector<float>& out) {
    std::error_code ec;
    const auto fileSize = fs::file_size(path, ec);
    std::ifstream f(path, std::ios::binary);
    char id[4];
    uint32_t size = 0;
    if (ec || !f.read(id, 4) || std::memcmp(id, "RIFF", 4) || !f.read(reinterpret_cast<char*>(&size), 4) ||
        !f.read(id, 4) || std::memcmp(id, "WAVE", 4))
        return false;

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    while (f.read(id, 4) && f.read(reinterpret_cast<char*>(&size), 4)) {
        if (!std::memcmp(id, "fmt ", 4) && size >= 16) {
            char b[16];
            if (!f.read(b, 16)) return false;
            std::memcpy(&format, b, 2);
            std::memcpy(&channels, b + 2, 2);
            std::memcpy(&rate, b + 4, 4);
            std::memcpy(&bits, b + 14, 2);
            f.seekg(size - 16 + (size & 1), std::ios::cur);
        } else if (!std::memcmp(id, "data", 4)) {
            if (format != 1 || bits != 16 || rate != 16000 || (channels != 1 && channels != 2)) return false;
            const size_t frameBytes = 2u * channels;
            // Never trust the header size beyond what the file actually holds.
            const size_t available = static_cast<size_t>(fileSize - std::min<uint64_t>(fileSize, static_cast<uint64_t>(f.tellg())));
            const size_t frames = std::min<size_t>(size, available) / frameBytes;
            out.resize(frames);
            int16_t buf[8192];
            size_t done = 0;
            while (done < frames) {
                const size_t want = std::min(frames - done, sizeof buf / frameBytes);
                f.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(want * frameBytes));
                const size_t got = static_cast<size_t>(f.gcount()) / frameBytes;
                for (size_t i = 0; i < got; ++i)
                    out[done + i] = channels == 1 ? buf[i] / 32768.0f : (buf[2 * i] + buf[2 * i + 1]) / 65536.0f;
                done += got;
                if (got < want) break;
            }
            out.resize(done);
            return done > 0;
        } else {
            f.seekg(size + (size & 1), std::ios::cur);
        }
    }
    return false;
}

std::string timeLabel(int64_t cs) { // whisper timestamps are in centiseconds
    char s[16];
    std::snprintf(s, sizeof s, "%02d:%02d.%03d", int(cs / 6000), int(cs / 100 % 60), int(cs % 100) * 10);
    return s;
}
} // namespace

WhisperEngine::WhisperEngine() = default;

WhisperEngine::~WhisperEngine() {
    if (ctx_) whisper_free(ctx_);
}

bool WhisperEngine::loadModel(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) whisper_free(ctx_); // free first so two models are never in memory at once
    loaded_ = false;
    LOG_INFO("Loading whisper model: " + path);
    ctx_ = whisper_init_from_file_with_params(path.c_str(), whisper_context_default_params());
    loaded_ = ctx_ != nullptr;
    if (!ctx_) LOG_ERROR("Failed to load whisper model: " + path);
    return loaded_;
}

void WhisperEngine::setOptions(Options o) {
    std::lock_guard<std::mutex> lock(optionsMutex_);
    options_ = std::move(o);
}

std::string WhisperEngine::transcribeFile(const std::string& path) {
    if (!loaded_) return "Error: No model loaded.";
    std::vector<float> pcm;
    if (!readWav16k(path, pcm)) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tmp = fs::temp_directory_path() / ("whispergui_" + std::to_string(stamp) + ".wav");
        // CreateProcess also finds the ffmpeg.exe shipped next to our exe, not just one on PATH.
        const int rc = runHidden("ffmpeg -nostdin -y -loglevel error -i \"" + path +
                                 "\" -ac 1 -ar 16000 -c:a pcm_s16le \"" + tmp.string() + "\"");
        const bool ok = rc == 0 && readWav16k(tmp.string(), pcm);
        std::error_code ec;
        fs::remove(tmp, ec);
        if (!ok) return rc == -1 ? "Error: ffmpeg was not found. Install it to transcribe this format."
                                 : "Error: Could not decode the audio file.";
    }
    Options o;
    {
        std::lock_guard<std::mutex> lock(optionsMutex_);
        o = options_;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ctx_) return "Error: No model loaded.";
    return transcribe(pcm, o);
}

std::string WhisperEngine::transcribe(const std::vector<float>& pcm, const Options& o) {
    std::vector<SpeakerSegment> speakers;
    if (!o.diarize) {
        diarizer_.reset(); // release the diarization models' memory while the feature is off
    } else {
        const std::string models = o.segmentationModel + '|' + o.embeddingModel;
        if (!diarizer_ || models != diarizerModels_) {
            diarizer_ = std::make_unique<SpeakerDiarizer>();
            if (!diarizer_->initialize(o.segmentationModel, o.embeddingModel)) diarizer_.reset();
            diarizerModels_ = models;
        }
        if (diarizer_) speakers = diarizer_->process(pcm.data(), static_cast<int>(pcm.size()));
    }

    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.print_progress = p.print_special = p.print_realtime = p.print_timestamps = false;
    p.translate = o.translate;
    p.language = o.language == "auto" ? nullptr : o.language.c_str();
    p.n_threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    p.abort_callback = [](void* cancel) { return static_cast<std::atomic<bool>*>(cancel)->load(); };
    p.abort_callback_user_data = &cancel_;
    if (whisper_full(ctx_, p, pcm.data(), static_cast<int>(pcm.size())) != 0) return "Error: Transcription failed.";

    std::string out;
    int lastSpeaker = -1;
    for (int i = 0, n = whisper_full_n_segments(ctx_); i < n; ++i) {
        const int64_t t0 = whisper_full_get_segment_t0(ctx_, i), t1 = whisper_full_get_segment_t1(ctx_, i);
        if (!speakers.empty()) {
            // Label with the speaker whose segment overlaps this one most (or lies nearest to it).
            const float s = t0 / 100.0f, e = t1 / 100.0f;
            float best = -1e30f;
            int who = 0;
            for (const auto& seg : speakers) {
                const float overlap = std::min(e, seg.end) - std::max(s, seg.start);
                if (overlap > best) {
                    best = overlap;
                    who = seg.speaker;
                }
            }
            if (who != lastSpeaker) {
                if (i) out += '\n';
                out += "Speaker " + std::to_string(who + 1) + ": ";
                lastSpeaker = who;
            }
        }
        if (o.timestamps) out += "[" + timeLabel(t0) + " --> " + timeLabel(t1) + "] ";
        out += whisper_full_get_segment_text(ctx_, i);
        if (i < n - 1) out += '\n';
    }
    return out;
}
