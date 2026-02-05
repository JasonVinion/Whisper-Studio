#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>

// Log levels
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

// Thread-safe singleton logger
class Logger {
public:
    static Logger& instance() {
        static Logger instance;
        return instance;
    }
    
    void log(LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        ss << " [" << levelToString(level) << "] " << message << std::endl;
        
        if (file_.is_open()) {
            file_ << ss.str();
            file_.flush();
        }
        
        // Also output to stderr for debug builds
#ifdef _DEBUG
        std::cerr << ss.str();
#endif
    }
    
    void debug(const std::string& message) { log(LogLevel::Debug, message); }
    void info(const std::string& message) { log(LogLevel::Info, message); }
    void warning(const std::string& message) { log(LogLevel::Warning, message); }
    void error(const std::string& message) { log(LogLevel::Error, message); }
    
private:
    Logger() {
        file_.open("whisper_studio.log", std::ios::app);
        if (file_.is_open()) {
            log(LogLevel::Info, "=== Whisper Studio Started ===");
        }
    }
    
    ~Logger() {
        if (file_.is_open()) {
            log(LogLevel::Info, "=== Whisper Studio Stopped ===");
            file_.close();
        }
    }
    
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    const char* levelToString(LogLevel level) {
        switch (level) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info: return "INFO";
            case LogLevel::Warning: return "WARN";
            case LogLevel::Error: return "ERROR";
            default: return "UNKNOWN";
        }
    }
    
    std::ofstream file_;
    std::mutex mutex_;
};

// Convenience macros
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
#define LOG_INFO(msg) Logger::instance().info(msg)
#define LOG_WARNING(msg) Logger::instance().warning(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
