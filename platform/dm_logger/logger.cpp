// platform/dm_logger/logger.cpp
#include "logger.h"

#include "signal_bus.h"
#include "signal_sink.h"
#include "signal_strings.h"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dream_machine {

// ================================================================
// 匿名命名空间：常量、thread_local channel、辅助函数、信号 Adapter
// ================================================================
namespace {

// 默认参数（可由 setMaxFileSize / setMaxBackupFiles 覆盖）
constexpr std::size_t DEFAULT_MAX_FILE_SIZE = 10 * 1024 * 1024;  // 10MB
constexpr int DEFAULT_MAX_BACKUP_FILES = 10;
constexpr int MAX_CRASH_BACKUPS_PER_PROCESS = 20;
constexpr const char* CRASH_DIR_NAME = "crashes";

// channel 使用 thread_local 语义：每个线程独立的当前 channel。
// 主线程默认为空字符串；通过 setChannel() / LogChannelScope 切换。
thread_local std::string t_current_channel;

// 生成用于文件名的紧凑时间戳：YYYYMMDD_HHMMSS_mmm
std::string makeTimestampForFilename() {
    using namespace std::chrono;

    auto now = system_clock::now();
    auto ms_part = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::time_t t = system_clock::to_time_t(now);
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &t);
#else
    localtime_r(&t, &tm_now);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y%m%d_%H%M%S")
        << '_' << std::setw(3) << std::setfill('0') << ms_part;
    return oss.str();
}

// ================================================================
// 信号级别 → 日志级别映射
//
// 一对一映射；语义：
//   SGL_DEBUG → LogLevel::DBG
//   SGL_INFO  → LogLevel::INFO
//   SGL_WARN  → LogLevel::WARN
//   SGL_ERROR → LogLevel::ERR    （LogLevel 用 ERR 避免 ERROR 宏）
//   SGL_FATAL → LogLevel::FATAL
// ================================================================
LogLevel signal_level_to_log_level(signal::SignalLevel level) {
    switch (level) {
        case signal::SignalLevel::SGL_DEBUG: return LogLevel::DBG;
        case signal::SignalLevel::SGL_INFO:  return LogLevel::INFO;
        case signal::SignalLevel::SGL_WARN:  return LogLevel::WARN;
        case signal::SignalLevel::SGL_ERROR: return LogLevel::ERR;
        case signal::SignalLevel::SGL_FATAL: return LogLevel::FATAL;
    }
    return LogLevel::INFO;   // 兜底（穷尽 switch 下不会到达）
}

// ================================================================
// LoggerSignalAdapter：Logger 的信号订阅适配器
//
// 方案 D 的核心：
//   - Logger 本身不继承 ISignalSink（logger.h 保持独立）
//   - 此 Adapter 在 logger.cpp 内部实现 ISignalSink
//   - Adapter 将 SignalPayload 转字符串后调 Logger::instance().log()
//
// 生命周期：
//   - 匿名命名空间静态对象，main 前构造
//   - 构造不访问 SignalBus——安全（SignalBus 可能未构造）
//   - 析构不访问 SignalBus——安全（SignalBus 可能已析构）
//   - 装配/卸载由 attach_to_signal_bus / detach_from_signal_bus 控制
// ================================================================
class LoggerSignalAdapter : public signal::ISignalSink {
public:
    void on_signal(const signal::SignalPayload& payload) override {
        // 1. 级别映射
        const LogLevel log_level = signal_level_to_log_level(payload.level);

        // 2. 构造消息内容
        //    格式：[signal:<type>] <description>[ | <detail>]
        std::string content;
        content.reserve(64 + payload.description.size() + payload.detail.size());
        content += "[signal:";
        content += signal::signal_type_to_string(payload.type);
        content += "] ";
        content += payload.description;
        if (!payload.detail.empty()) {
            content += " | ";
            content += payload.detail;
        }

        // 3. 写入日志
        //    file/line 传占位 "<signal>" / 0——表示消息来自信号总线
        //    （formatMessage 当前忽略 file/line；占位便于未来溯源）
        Logger::instance().log(log_level, "<signal>", 0, content);
    }
};

// Adapter 单例（main 前构造；无副作用）
LoggerSignalAdapter g_signal_adapter;

// 装配状态（幂等保护）
std::atomic<bool> g_signal_bus_attached{false};

} // namespace

// ================================================================
// 构造函数 / 析构函数
// ================================================================

Logger::Logger()
    : process_name_("unknown")
    , log_dir_("./logs")
    , min_level_(LogLevel::INFO)
    , max_file_size_(DEFAULT_MAX_FILE_SIZE)
    , max_backup_files_(DEFAULT_MAX_BACKUP_FILES)
    , initialized_(false)
    , error_separate_(false)
{
    // DM_LOG_ERROR_SEPARATE=1 时启用 ERROR 独立文件
    const char* env = std::getenv("DM_LOG_ERROR_SEPARATE");
    if (env && std::string(env) == "1") {
        error_separate_ = true;
    }
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, info] : streams_) {
        if (info.stream.is_open()) {
            info.stream.flush();
            info.stream.close();
        }
    }
    streams_.clear();
}

// ================================================================
// 单例访问
// ================================================================

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

// ================================================================
// 配置接口
// ================================================================

void Logger::setProcessName(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (process_name_ == name) {
        return;
    }
    process_name_ = name;

    // 文件名前缀变了：关闭所有现有流，下次写入时按新前缀重建
    if (initialized_) {
        for (auto& [filename, info] : streams_) {
            if (info.stream.is_open()) {
                info.stream.flush();
                info.stream.close();
            }
        }
        streams_.clear();
    }
}

void Logger::setLogDirectory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_dir_ == path) {
        return;
    }
    log_dir_ = path;

    if (initialized_) {
        for (auto& [filename, info] : streams_) {
            if (info.stream.is_open()) {
                info.stream.flush();
                info.stream.close();
            }
        }
        streams_.clear();
    }
}

void Logger::setMinLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = level;
}

void Logger::setMaxFileSize(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > 0) {
        max_file_size_ = bytes;
    }
}

void Logger::setMaxBackupFiles(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count >= 0) {
        max_backup_files_ = count;
    }
}

// ================================================================
// 信号总线装配（方案 D）
//
// attach / detach 幂等：
//   - 用 g_signal_bus_attached 原子布尔跟踪状态
//   - 重复调用无副作用
//
// 时序保证：
//   - attach 中调用 SignalBus::instance() 首次触发其构造
//   - detach 中调用同上（若 attach 未调用过则 detach 直接返回）
//   - 各进程 main() 负责配对调用
// ================================================================

void Logger::attach_to_signal_bus() {
    if (g_signal_bus_attached.exchange(true)) {
        return;   // 已 attach
    }
    signal::SignalBus::instance().subscribe(&g_signal_adapter);
}

void Logger::detach_from_signal_bus() {
    if (!g_signal_bus_attached.exchange(false)) {
        return;   // 未 attach
    }
    signal::SignalBus::instance().unsubscribe(&g_signal_adapter);
}

// ================================================================
// 生命周期标记
// ================================================================

bool Logger::archiveLastSessionIfDirty() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::filesystem::path marker =
        std::filesystem::path(log_dir_) / (".clean_exit_" + process_name_);

    std::error_code ec;

    // 上次正常退出：删除标记，无需归档
    if (std::filesystem::exists(marker, ec)) {
        std::filesystem::remove(marker, ec);
        return false;
    }

    // 标记不存在：可能是异常退出，也可能是首次启动
    std::filesystem::path main_log =
        std::filesystem::path(log_dir_) / (process_name_ + ".log");

    if (!std::filesystem::exists(main_log, ec)) {
        return false;   // 首次启动，无日志可归档
    }

    // 归档主日志
    archiveLogFile(process_name_ + ".log");

    // 若启用 ERROR 独立文件且存在，一并归档
    if (error_separate_) {
        std::filesystem::path err_log =
            std::filesystem::path(log_dir_) / (process_name_ + ".error.log");
        if (std::filesystem::exists(err_log, ec)) {
            archiveLogFile(process_name_ + ".error.log");
        }
    }

    cleanupOldCrashes();
    return true;
}

void Logger::markCleanExit() {
    std::lock_guard<std::mutex> lock(mutex_);

    ensureLogDirectoryExists();

    std::filesystem::path marker =
        std::filesystem::path(log_dir_) / (".clean_exit_" + process_name_);

    std::ofstream marker_file(marker.string());
    if (marker_file.is_open()) {
        marker_file << "clean_exit\n";
    }
}

// ================================================================
// Channel 接口（thread_local）
// ================================================================

void Logger::setChannel(const std::string& channel) {
    t_current_channel = channel;
}

std::string Logger::getChannel() const {
    return t_current_channel;
}

// ================================================================
// LogChannelScope：RAII 保存/恢复 channel
// ================================================================

LogChannelScope::LogChannelScope(const std::string& channel)
    : saved_channel_(Logger::instance().getChannel())
{
    Logger::instance().setChannel(channel);
}

LogChannelScope::~LogChannelScope() {
    Logger::instance().setChannel(saved_channel_);
}

// ================================================================
// 内部辅助：目录与归档
// ================================================================

void Logger::ensureLogDirectoryExists() {
    std::filesystem::path dir_path(log_dir_);
    if (!std::filesystem::exists(dir_path)) {
        std::error_code ec;
        std::filesystem::create_directories(dir_path, ec);
    }
}

void Logger::archiveLogFile(const std::string& filename) {
    std::filesystem::path src = std::filesystem::path(log_dir_) / filename;

    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return;
    }

    std::filesystem::path crashes_dir =
        std::filesystem::path(log_dir_) / CRASH_DIR_NAME;
    std::filesystem::create_directories(crashes_dir, ec);

    // 计算归档名：base = filename 去掉 ".log"
    std::string base = filename;
    constexpr const char* DOT_LOG = ".log";
    constexpr std::size_t DOT_LOG_LEN = 4;
    if (base.size() > DOT_LOG_LEN &&
        base.compare(base.size() - DOT_LOG_LEN, DOT_LOG_LEN, DOT_LOG) == 0) {
        base = base.substr(0, base.size() - DOT_LOG_LEN);
    }

    std::string archive_name =
        "crash_" + makeTimestampForFilename() + "_" + base + ".log";
    std::filesystem::path dst = crashes_dir / archive_name;

    // 优先 rename
    std::filesystem::rename(src, dst, ec);
    if (!ec) {
        return;
    }

    // rename 失败：copy + remove
    ec.clear();
    std::filesystem::copy_file(
        src, dst,
        std::filesystem::copy_options::overwrite_existing,
        ec);
    if (!ec) {
        std::error_code rm_ec;
        std::filesystem::remove(src, rm_ec);
    }
}

void Logger::cleanupOldCrashes() {
    std::filesystem::path crashes_dir =
        std::filesystem::path(log_dir_) / CRASH_DIR_NAME;

    std::error_code ec;
    if (!std::filesystem::exists(crashes_dir, ec)) {
        return;
    }

    std::unordered_map<std::string,
                       std::vector<std::filesystem::directory_entry>> groups;

    for (const auto& entry : std::filesystem::directory_iterator(crashes_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string name = entry.path().filename().string();

        if (name.size() < 20) continue;
        if (name.compare(0, 6, "crash_") != 0) continue;

        std::size_t p1 = 6 + 8;
        if (p1 >= name.size() || name[p1] != '_') continue;
        std::size_t p2 = p1 + 1 + 6;
        if (p2 >= name.size() || name[p2] != '_') continue;
        std::size_t p3 = p2 + 1 + 3;
        if (p3 >= name.size() || name[p3] != '_') continue;

        std::string base = name.substr(p3 + 1);
        constexpr const char* DOT_LOG = ".log";
        constexpr std::size_t DOT_LOG_LEN = 4;
        if (base.size() > DOT_LOG_LEN &&
            base.compare(base.size() - DOT_LOG_LEN, DOT_LOG_LEN, DOT_LOG) == 0) {
            base = base.substr(0, base.size() - DOT_LOG_LEN);
        }

        groups[base].push_back(entry);
    }

    for (auto& [base, entries] : groups) {
        if (entries.size() <= static_cast<std::size_t>(MAX_CRASH_BACKUPS_PER_PROCESS)) {
            continue;
        }

        std::sort(entries.begin(), entries.end(),
                  [](const std::filesystem::directory_entry& a,
                     const std::filesystem::directory_entry& b) {
                      return a.path().filename() < b.path().filename();
                  });

        const std::size_t to_remove =
            entries.size() - static_cast<std::size_t>(MAX_CRASH_BACKUPS_PER_PROCESS);
        for (std::size_t i = 0; i < to_remove; ++i) {
            std::error_code rm_ec;
            std::filesystem::remove(entries[i].path(), rm_ec);
        }
    }
}

std::string Logger::processBaseName() const {
    constexpr const char* PREFIX = "core_engine_";
    constexpr std::size_t PREFIX_LEN = 12;
    if (process_name_.size() > PREFIX_LEN &&
        process_name_.compare(0, PREFIX_LEN, PREFIX) == 0) {
        return "core_engine";
    }
    return process_name_;
}

// ================================================================
// 核心日志接口
// ================================================================

void Logger::log(LogLevel level, const char* file, int line, const std::string& msg) {
    if (level < min_level_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        ensureLogDirectoryExists();
        initialized_ = true;
    }

    const std::string formatted = formatMessage(level, file, line, msg);
    const std::string main_filename = currentFilename();

    auto write_to = [&](const std::string& filename) {
        if (getOrOpenStream(filename) == nullptr) {
            return;
        }
        auto it = streams_.find(filename);
        if (it == streams_.end()) {
            return;
        }
        rotateIfNeeded(filename, it->second);
        if (it->second.stream.is_open()) {
            it->second.stream << formatted << std::flush;
            it->second.bytes_since_check += formatted.size();
        }
    };

    write_to(main_filename);

    if (error_separate_ && level >= LogLevel::ERR) {
        write_to(errorFilename(main_filename));
    }

#ifndef NDEBUG
    std::cout << formatted << std::flush;
#else
    if (level >= LogLevel::ERR) {
        std::cerr << formatted << std::flush;
    }
#endif
}

// ================================================================
// 私有辅助：格式化
// ================================================================

std::string Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::DBG:   return "DEBUG";   // 对外显示全名（与 ERR→"ERROR" 一致）
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERR:   return "ERROR";
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

std::string Logger::formatMessage(LogLevel level, const char* file, int line,
                                  const std::string& msg) const {
    (void)file;
    (void)line;

    std::ostringstream oss;
    oss << '[' << currentTimestamp() << ']'
        << " [" << process_name_ << ']'
        << " [" << levelToString(level) << "] "
        << msg;
    return oss.str();
}

// ================================================================
// 私有辅助：文件命名
// ================================================================

std::string Logger::currentFilename() const {
    const std::string& channel = t_current_channel;
    if (channel.empty()) {
        return process_name_ + ".log";
    }
    return process_name_ + "." + channel + ".log";
}

std::string Logger::errorFilename(const std::string& main_filename) const {
    constexpr const char* DOT_LOG = ".log";
    constexpr std::size_t DOT_LOG_LEN = 4;

    if (main_filename.size() > DOT_LOG_LEN &&
        main_filename.compare(main_filename.size() - DOT_LOG_LEN,
                              DOT_LOG_LEN, DOT_LOG) == 0) {
        return main_filename.substr(0, main_filename.size() - DOT_LOG_LEN)
               + ".error.log";
    }
    return main_filename + ".error";
}

// ================================================================
// 私有辅助：流管理
// ================================================================

std::ofstream* Logger::getOrOpenStream(const std::string& filename) {
    auto it = streams_.find(filename);

    if (it == streams_.end()) {
        auto [inserted_it, ok] = streams_.emplace(filename, StreamInfo{});
        (void)ok;
        it = inserted_it;
    }

    if (it->second.stream.is_open()) {
        return &it->second.stream;
    }

    std::filesystem::path path = std::filesystem::path(log_dir_) / filename;
    it->second.stream.open(path.string(), std::ios::out | std::ios::app);
    if (!it->second.stream.is_open()) {
        return nullptr;
    }
    return &it->second.stream;
}

// ================================================================
// 私有辅助：轮转
// ================================================================

void Logger::rotateIfNeeded(const std::string& filename, StreamInfo& info) {
    constexpr std::size_t CHECK_INTERVAL_BYTES = 512 * 1024;  // 512KB

    if (max_backup_files_ <= 0) {
        return;
    }

    if (info.bytes_since_check < CHECK_INTERVAL_BYTES) {
        return;
    }
    info.bytes_since_check = 0;

    std::filesystem::path path = std::filesystem::path(log_dir_) / filename;
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return;
    }
    if (size < max_file_size_) {
        return;
    }

    if (info.stream.is_open()) {
        info.stream.flush();
        info.stream.close();
    }

    std::filesystem::path max_backup =
        std::filesystem::path(log_dir_) /
        (filename + "." + std::to_string(max_backup_files_));
    std::filesystem::remove(max_backup, ec);

    for (int i = max_backup_files_ - 1; i >= 1; --i) {
        std::filesystem::path src =
            std::filesystem::path(log_dir_) /
            (filename + "." + std::to_string(i));
        std::filesystem::path dst =
            std::filesystem::path(log_dir_) /
            (filename + "." + std::to_string(i + 1));
        ec.clear();
        std::filesystem::rename(src, dst, ec);
    }

    std::filesystem::path dst1 =
        std::filesystem::path(log_dir_) / (filename + ".1");
    ec.clear();
    std::filesystem::rename(path, dst1, ec);

    info.stream.open(path.string(), std::ios::out | std::ios::app);
}

} // namespace dream_machine