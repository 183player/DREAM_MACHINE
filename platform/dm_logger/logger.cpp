// platform/dm_logger/logger.cpp
#include "logger.h"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dream_machine {

// ============================================================================
// 构造函数 / 析构函数
// ============================================================================

Logger::Logger()
    : process_name_("unknown")
    , log_dir_("./logs")
    , min_level_(LogLevel::INFO)
    , initialized_(false)
{
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_stream_.is_open()) {
        file_stream_.flush();
        file_stream_.close();
    }
}

// ============================================================================
// 单例访问
// ============================================================================

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

// ============================================================================
// 配置接口
// ============================================================================

void Logger::setProcessName(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    process_name_ = name;
    if (initialized_) {
        openLogFile();
    }
}

void Logger::setLogDirectory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_dir_ = path;
    if (initialized_) {
        openLogFile();
    }
}

void Logger::setMinLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = level;
}

// ============================================================================
// 核心日志接口
// ============================================================================

void Logger::log(LogLevel level, const char* file, int line, const std::string& msg) {
    if (level < min_level_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        ensureLogDirectoryExists();
        openLogFile();
        initialized_ = true;
    }

    std::string formatted = formatMessage(level, file, line, msg);

    if (file_stream_.is_open()) {
        file_stream_ << formatted << std::flush;
    }

    // 控制台输出：Debug 模式下全部输出，Release 模式下仅 ERR/FATAL
#ifndef NDEBUG
    std::cout << formatted << std::flush;
#else
    if (level >= LogLevel::ERR) {
        std::cerr << formatted << std::flush;
    }
#endif
}

// ============================================================================
// 私有辅助方法
// ============================================================================

std::string Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERR:   return "ERROR";   // 对外显示仍为 ERROR
        case LogLevel::FATAL: return "FATAL";
        default:              return "UNKNOWN";
    }
}

std::string Logger::currentTimestamp() const {
    using namespace std::chrono;

    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    auto seconds_part = duration_cast<seconds>(now.time_since_epoch());
    auto ms_part = ms - duration_cast<milliseconds>(seconds_part).count();

    std::time_t time_t_now = system_clock::to_time_t(now);
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms_part;
    return oss.str();
}

std::string Logger::formatMessage(LogLevel level, const char* file, int line, const std::string& msg) const {
    std::ostringstream oss;
    oss << '[' << currentTimestamp() << ']'
        << " [" << process_name_ << ']'
        << " [" << levelToString(level) << "] "
        << msg;
    return oss.str();
}

void Logger::ensureLogDirectoryExists() {
    std::filesystem::path dir_path(log_dir_);
    if (!std::filesystem::exists(dir_path)) {
        std::error_code ec;
        std::filesystem::create_directories(dir_path, ec);
        // 静默失败，后续 openLogFile 会处理
    }
}

void Logger::openLogFile() {
    if (file_stream_.is_open()) {
        file_stream_.flush();
        file_stream_.close();
    }

    std::filesystem::path file_path = std::filesystem::path(log_dir_) / (process_name_ + ".log");
    file_stream_.open(file_path.string(), std::ios::out | std::ios::app);
}

} // namespace dream_machine