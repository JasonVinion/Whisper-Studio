#include "SpeakerDiarizer.h"
#include "Util.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

#if WHISPERGUI_HAS_SHERPA_ONNX
#include "sherpa-onnx/c-api/c-api.h"

SpeakerDiarizer::~SpeakerDiarizer() {
    if (sd_) SherpaOnnxDestroyOfflineSpeakerDiarization(sd_);
}

bool SpeakerDiarizer::initialize(const std::string& segmentationModel, const std::string& embeddingModel, int numSpeakers) {
    for (const auto* path : {&segmentationModel, &embeddingModel}) {
        if (!std::filesystem::exists(*path)) {
            LOG_ERROR("Diarization model not found: " + *path);
            return false;
        }
    }
    SherpaOnnxOfflineSpeakerDiarizationConfig config{};
    config.segmentation.pyannote.model = segmentationModel.c_str();
    config.segmentation.num_threads = 2;
    config.segmentation.provider = "cpu";
    config.embedding.model = embeddingModel.c_str();
    config.embedding.num_threads = 2;
    config.embedding.provider = "cpu";
    config.clustering.num_clusters = numSpeakers;
    config.clustering.threshold = 0.5f;
    config.min_duration_on = 0.2f;  // drop speech shorter than this (s)
    config.min_duration_off = 0.5f; // merge gaps shorter than this (s)

    if (sd_) SherpaOnnxDestroyOfflineSpeakerDiarization(sd_);
    sd_ = SherpaOnnxCreateOfflineSpeakerDiarization(&config);
    if (!sd_) {
        LOG_ERROR("Failed to create the sherpa-onnx diarization pipeline");
        return false;
    }
    LOG_INFO("Speaker diarization ready (sherpa-onnx): " + segmentationModel + ", " + embeddingModel);
    return true;
}

std::vector<SpeakerSegment> SpeakerDiarizer::process(const float* samples, int count, int) {
    std::vector<SpeakerSegment> out;
    const auto* result = sd_ ? SherpaOnnxOfflineSpeakerDiarizationProcess(sd_, samples, count) : nullptr;
    if (!result) {
        LOG_ERROR("Speaker diarization failed");
        return out;
    }
    const int n = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(result);
    if (const auto* segs = SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(result)) {
        out.reserve(n);
        for (int i = 0; i < n; ++i) out.push_back({segs[i].start, segs[i].end, segs[i].speaker});
        SherpaOnnxOfflineSpeakerDiarizationDestroySegment(segs);
    }
    LOG_INFO("Diarization found " + std::to_string(SherpaOnnxOfflineSpeakerDiarizationResultGetNumSpeakers(result)) +
             " speakers in " + std::to_string(n) + " segments");
    SherpaOnnxOfflineSpeakerDiarizationDestroyResult(result);
    return out;
}

#else // Heuristic fallback: no models needed.

SpeakerDiarizer::~SpeakerDiarizer() = default;

bool SpeakerDiarizer::initialize(const std::string&, const std::string&, int) {
    LOG_INFO("Speaker diarization ready (heuristic; build with -DWHISPERGUI_USE_SHERPA_ONNX=ON for neural models)");
    return true;
}

// Splits speech on pauses and alternates between two speakers when loudness changes sharply after a pause.
std::vector<SpeakerSegment> SpeakerDiarizer::process(const float* x, int count, int rate) {
    std::vector<SpeakerSegment> out;
    const int chunk = rate / 2; // 0.5 s
    float lastRms = 0, lastPeak = 0, start = 0;
    int speaker = 0, silentChunks = 0;
    bool inSpeech = false;
    auto emit = [&](float end) {
        if (end > start + 0.2f) out.push_back({start, end, speaker});
    };
    for (int c = 0; c + chunk <= count; c += chunk) {
        float sum = 0, peak = 0;
        for (int i = c; i < c + chunk; ++i) {
            float a = std::abs(x[i]);
            sum += a * a;
            peak = std::max(peak, a);
        }
        const float rms = std::sqrt(sum / chunk), t = static_cast<float>(c) / rate;
        if (rms > 0.01f) {
            if (!inSpeech) {
                start = t;
                inSpeech = true;
            }
            bool changed = (lastRms > 0 && std::abs(rms - lastRms) / lastRms > 0.5f) ||
                           (lastPeak > 0 && std::abs(peak - lastPeak) / lastPeak > 0.4f);
            if (silentChunks >= 2 && changed) {
                emit(t);
                speaker ^= 1;
                start = t;
            }
            silentChunks = 0;
            lastRms = rms; // compare speech with speech, not with the pause in between
            lastPeak = peak;
        } else if (++silentChunks >= 3 && inSpeech) {
            emit(t);
            inSpeech = false;
        }
    }
    if (inSpeech) emit(static_cast<float>(count) / rate);
    return out;
}
#endif
