#pragma once
// Small shared helpers: logging, time formatting, hidden process launch, background worker.
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

inline std::string localTime(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    return std::string(buf, std::strftime(buf, sizeof buf, fmt, &tm));
}

// Appends a line to whisper_studio.log in the working directory (main() sets it to the exe folder).
// The log is started fresh once it passes 1 MB.
inline void logLine(const char* level, const std::string& msg) {
    static std::mutex mutex;
    static std::ofstream file = [] {
        std::error_code ec;
        auto size = std::filesystem::file_size("whisper_studio.log", ec);
        return std::ofstream("whisper_studio.log", !ec && size > (1u << 20) ? std::ios::trunc : std::ios::app);
    }();
    std::lock_guard<std::mutex> lock(mutex);
    file << localTime("%Y-%m-%d %H:%M:%S") << " [" << level << "] " << msg << std::endl;
}
#define LOG_INFO(msg) logLine("INFO", msg)
#define LOG_WARNING(msg) logLine("WARN", msg)
#define LOG_ERROR(msg) logLine("ERROR", msg)

// Runs a command line without a console window and waits for it to finish.
// Returns the exit code, or -1 if the program could not be started.
inline int runHidden(std::string cmd) {
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

inline HICON loadAppIcon() {
    if (HICON icon = LoadIconA(GetModuleHandleA(nullptr), "IDI_ICON1")) return icon;
    if (auto icon = static_cast<HICON>(LoadImageA(nullptr, "resources/app.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE)))
        return icon;
    return LoadIconA(nullptr, reinterpret_cast<LPCSTR>(IDI_APPLICATION));
}

// Runs posted jobs one at a time, in order, on a background thread.
class Worker {
public:
    Worker() : thread_([this] { run(); }) {}
    ~Worker() { shutdown(); }

    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            jobs_.push_back(std::move(job));
            ++pending_;
        }
        cv_.notify_one();
    }
    int pending() const { return pending_; } // queued + running

    // Drops queued jobs and waits for the running one to finish.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            jobs_.clear();
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try {
                job();
            } catch (const std::exception& e) {
                LOG_ERROR(std::string("Background job failed: ") + e.what());
            }
            --pending_;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::atomic<int> pending_{0};
    std::thread thread_; // last, so everything above exists before the thread starts
};
