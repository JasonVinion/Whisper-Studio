<h1 align="center">Whisper Studio</h1>

<p align="center">
  Native Windows desktop application for real-time speech transcription and dictation using OpenAI Whisper.
</p>

<p align="center">
  <a href="LICENSE">
    <img src="https://img.shields.io/badge/License-MIT-green.svg" alt="MIT License">
  </a>
  <img src="https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=c%2B%2B" alt="C++20">
  <img src="https://img.shields.io/badge/Build-CMake-064F8C?logo=cmake" alt="CMake">
  <a href="https://github.com/JasonVinion/Whisper-Studio/releases">
    <img src="https://img.shields.io/github/v/release/JasonVinion/Whisper-Studio" alt="Latest Release">
  </a>
</p>

---

## Quick Links

- [**Download**](https://github.com/JasonVinion/Whisper-Studio/releases/latest)
- [**About**](#about)
- [**Features**](#features)
- [**Issues**](https://github.com/JasonVinion/Whisper-Studio/issues)
- [**Feature Requests**](https://github.com/JasonVinion/Whisper-Studio/issues/new)
- [**Installation**](#installation)
- [**Usage**](#usage)
- [**Contributing**](#contributing)
- [**License**](#license)


## About

Everyone and their dog has built a Whisper wrapper these days. There are countless Python GUIs, Electron apps, web interfaces, etc. Still somehow, none of them quite fit the workflow I needed, so I built my own. As such Whisper Studio is a native lightweight C++ application that runs locally on your Windows machine, transcribes audio in real-time, identifies speakers, can auto-paste transcriptions, and a few other things. Its not the prettiest app, I suck at design, but it gets the job done.

## Features


- **Native C++** - Implemented in C++. Thanks to the hard work on [whisper.cpp](https://github.com/ggerganov/whisper.cpp) by ggerganov, which made this project possible.
- **Real-Time Dictation** - Live transcription mode which works via automatic silence detection and segmentation.
- **Speaker Diarization** - Identifies and labels different speakers using neural network–based models powered by the hard work of the [Sherpa-ONNX](https://github.com/k2-fsa/sherpa-onnx) team.
- **Auto-Paste** - Self-explanatory
- **Global Hotkeys** - default: F6
- **Built in Editor** - All transcriptions saved locally with inline editing support
- **Export Transcriptions** - Export to TXT, JSON, or SRT (subtitles)
- **Multiple Input Sources** - This should be obvious, but I noticed many low quality python wrappers didn't support it. 
- **Model Management** - Once again should be obvious but including here for the same reason as stated above.
- **System Tray** - Run in background with tray icon for quick access to common actions
- **Audio Format Support** - Handles WAV natively; converts other formats via FFmpeg

## Installation

### Option 1: Use the Installer (Recommended)
This is bundled with all dependencies and is the easiest way to get started. No need to manually install CUDA, FFmpeg, etc. 

1. Download the latest installer from the Releases page
2. Choose between:
    - **Standard (CPU)** - Works on any Windows 10/11 machine
    - **NVIDIA GPU** - Accelerated transcription if you have a CUDA-capable GPU
3. Run the installer and follow the prompts
4. Launch Whisper Studio from your Start Menu

### Option 2: Build from Source

**Requirements:**

- Windows 10/11
- Visual Studio 2022 with C++ Desktop Development workload
- CMake 3.20+
- (Optional) CUDA Toolkit 11.8+ for GPU acceleration
- (Optional) FFmpeg in PATH for non-WAV audio file support

**Build Steps:**

```bash
# Clone the repository
git clone https://github.com/yourusername/whisper-studio.git
cd whisper-studio

# Configure with CMake
# The build system will automatically detect if CUDA is installed
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Run
.\build\Release\WhisperStudio.exe
```

For GPU support, ensure CUDA Toolkit is installed before running CMake. 

## Usage

### Basic Transcription

1. **Select your microphone** from the Audio Device dropdown
2. **Download a model** - Click the Whisper Model dropdown and select a model to download
3. **Click Record** or press your hotkey (F6) to start recording
4. **Speak naturally** - The waveform will show your audio
5. **Stop recording** - Click Stop or press the hotkey again
6. **Wait for transcription** - Results appear in the Transcription panel

### Live Dictation Mode

1. Enable **Live Transcription** in Settings
2. Start recording
3. Speak in natural phrases with brief pauses
4. The app automatically detects silence, transcribes each segment, and continues recording
5. Enable **Auto-Paste** to have text automatically typed into your active window

### Speaker Identification

1. Enable **Speaker Diarization** in Settings
2. Download the required models when prompted (Pyannote segmentation + 3D-Speaker embedding)
3. Transcriptions will be labeled with "Speaker 1:", "Speaker 2:", etc.

### File Transcription

1. Click **Open File** to import an existing audio file
2. Supported formats: WAV (native), MP3, M4A, FLAC (requires FFmpeg)
3. Select the file and click **Transcribe**

## Models

Whisper Studio supports all standard Whisper.cpp models:

|Model|Size|Speed|Accuracy|Best For|
|---|---|---|---|---|
|tiny.en|75 MB|Fastest|Basic|Quick notes, testing|
|base.en|142 MB|Very Fast|Good|General dictation|
|small.en|466 MB|Fast|Better|Quality transcription|
|medium.en|1.5 GB|Moderate|Great|Professional work|
|large-v3|3.1 GB|Slow|Best|Maximum accuracy|

Models are downloaded on-demand through the built-in model manager. Quantized variants (q5_0, q8_0) are also available for reduced memory usage.

## Technical Architecture

Whisper Studio is built entirely in modern C++17 with:

- **GUI**: Dear ImGui (docking branch) + SDL2
- **ASR Engine**: whisper.cpp (local inference, CPU/CUDA)
- **Diarization**: sherpa-onnx (neural speaker identification)
- **Audio**: SDL2 (16kHz mono PCM capture)
- **Networking**: WinHTTP (native Windows, for model downloads)
- **Build**: CMake with FetchContent dependency management
## Performance Recommendations

- **Use GPU acceleration** if you have an NVIDIA card (5-10x faster than CPU)
- **Enable quantization** (q5_0, q8_0 variants) to reduce memory usage with minimal accuracy loss

## Contributing

Contributions are welcome! This project is a personal workflow tool that grew larger than expected, and there's plenty of room for improvement.

## License

This project is licensed under the [MIT License](LICENSE).

## Acknowledgments

- OpenAI for the Whisper models
- [whisper.cpp](https://github.com/ggerganov/whisper.cpp) by Georgi Gerganov
- [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) for speaker diarization
- Dear ImGui for the GUI framework
- The entire open-source speech recognition community

---

**Note**: This is an independent project and is not affiliated with OpenAI.
