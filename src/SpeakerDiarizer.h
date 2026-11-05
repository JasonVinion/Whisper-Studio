#pragma once
#include <string>
#include <vector>

struct SherpaOnnxOfflineSpeakerDiarization;

struct SpeakerSegment {
    float start, end; // seconds
    int speaker;      // 0-based
};

// Speaker diarization. Built with sherpa-onnx (WHISPERGUI_HAS_SHERPA_ONNX=1) it runs neural
// segmentation + embedding models; otherwise it uses a simple loudness heuristic that needs no models.
// Not thread-safe: the owner serializes calls.
class SpeakerDiarizer {
public:
    SpeakerDiarizer() = default;
    SpeakerDiarizer(const SpeakerDiarizer&) = delete;
    SpeakerDiarizer& operator=(const SpeakerDiarizer&) = delete;
    ~SpeakerDiarizer();

    // numSpeakers = -1 detects the number of speakers automatically.
    bool initialize(const std::string& segmentationModel, const std::string& embeddingModel, int numSpeakers = -1);
    std::vector<SpeakerSegment> process(const float* samples, int count, int sampleRate = 16000);

private:
    const SherpaOnnxOfflineSpeakerDiarization* sd_ = nullptr;
};
