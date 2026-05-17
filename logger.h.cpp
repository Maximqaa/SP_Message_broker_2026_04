// =============================================================================
// Файл: logger.h
// Система логирования с уровнями и временными метками.
// =============================================================================
#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <string>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <mutex>

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) { min_level_ = level; }

    static std::string color(LogLevel lvl) {
        switch(lvl) {
            case LogLevel::DEBUG: return "\033[36m";
            case LogLevel::INFO:  return "\033[32m";
            case LogLevel::WARN:  return "\033[33m";
            case LogLevel::ERROR: return "\033[31m";
        }
        return "\033[0m";
    }

    static std::string level_str(LogLevel lvl) {
        switch(lvl) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
        }
        return "UNKN ";
    }

    void log(LogLevel lvl, const std::string& msg) {
        if (lvl < min_level_) return;

        auto now = std::time(nullptr);
        auto tm = *std::localtime(&now);

        std::ostringstream oss;
        oss << color(lvl)
            << "[" << std::put_time(&tm, "%H:%M:%S") << "] "
            << "[" << level_str(lvl) << "] "
            << msg
            << "\033[0m\n";

        std::lock_guard<std::mutex> lock(mutex_);
        std::cerr << oss.str() << std::flush;
    }

private:
    Logger() : min_level_(LogLevel::INFO) {}
    LogLevel min_level_;
    std::mutex mutex_;
};

#define LOG_DEBUG(msg) Logger::instance().log(LogLevel::DEBUG, msg)
#define LOG_INFO(msg)  Logger::instance().log(LogLevel::INFO, msg)
#define LOG_WARN(msg)  Logger::instance().log(LogLevel::WARN, msg)
#define LOG_ERROR(msg) Logger::instance().log(LogLevel::ERROR, msg)

#endif