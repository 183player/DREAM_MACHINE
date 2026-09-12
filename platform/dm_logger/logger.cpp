// platform/dm_logger/logger.cpp
// ================================================================
// 注意：本文件分两轮输出。
//   上半（本轮）：构造/析构/配置/生命周期/channel/归档辅助
//   下半（下一轮）：log() 及写入路径辅助
// 上半实现完毕后，因 log() 等 8 个函数未实现，链接会失败——符合预期。
// ================================================================
#include "logger.h"

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

#ifdef _WIN32
#include <windows.h>
#endif

namespace dream_machine {

// ================================================================
// 匿名命名空间：常量、thread_local channel、辅助函数
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
    // DM_LOG_ERROR_SEPARATE=1 时启用 ERROR 独立文件（契约 #10）
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
// 生命周期标记
// ================================================================

// 检查上次是否正常退出；若异常，则把旧的 {process_name}.log
// （以及 .error.log，若启用）归档到 logs/crashes/。
//
// 归档文件名：crash_{timestamp}_{process_name}[.error].log
//   例如：crash_20260912_101530_123_launcher.log
//         crash_20260912_101530_123_launcher.error.log
//
// 若 logs/.clean_exit_{process_name} 存在：
//   - 说明上次正常退出 → 删除标记，返回 false
//   - 不归档（日志内容已正常收尾）
//
// 若标记不存在：
//   - 检查 {process_name}.log 是否存在
//     - 存在 → 归档，返回 true
//     - 不存在（首次启动）→ 不归档，返回 false
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
        // 首次启动，无日志可归档
        return false;
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

    // 清理旧归档
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
        // ofstream 析构时自动关闭
    }
    // 写失败不阻塞——fail-fast 语义仅适用于业务故障，日志故障不致命
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
        // 静默失败：写入路径上会再次尝试创建
    }
}

// 将 log_dir_/{filename} 归档到 log_dir_/crashes/
//
// 归档策略：
//   1. 优先 std::filesystem::rename（同卷原子操作）
//   2. 失败则 copy_file + remove（跨卷或 rename 被占用）
//
// 归档文件名：crash_{timestamp}_{base}.log
//   其中 base 为 filename 去掉 ".log" 后缀
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
    // 若 copy 也失败：保留原文件不动，静默返回
}

// 清理 logs/crashes/ 中的旧归档。
//
// 分组策略：按"进程基名"分组（契约 #6）
//   - crash_TS_launcher.log          → 组 "launcher"
//   - crash_TS_core_engine_abc.log   → 组 "core_engine_abc"
// 每组保留最新 MAX_CRASH_BACKUPS_PER_PROCESS 个，其余删除。
//
// 时间戳格式 YYYYMMDD_HHMMSS_mmm 保证字典序 == 时间序，
// 因此按 filename 字符串排序即可。
void Logger::cleanupOldCrashes() {
    std::filesystem::path crashes_dir =
        std::filesystem::path(log_dir_) / CRASH_DIR_NAME;

    std::error_code ec;
    if (!std::filesystem::exists(crashes_dir, ec)) {
        return;
    }

    // 分组：base → 该 base 下的所有归档 entry
    std::unordered_map<std::string,
                       std::vector<std::filesystem::directory_entry>> groups;

    for (const auto& entry : std::filesystem::directory_iterator(crashes_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string name = entry.path().filename().string();

        // 文件名格式：crash_YYYYMMDD_HHMMSS_mmm_{base}.log
        // 逐段定位：crash_ / YYYYMMDD / HHMMSS / mmm / base
        if (name.size() < 20) continue;
        if (name.compare(0, 6, "crash_") != 0) continue;

        // 跳过 "crash_" 后的前 4 个 '_' 分隔符
        // 分段：YYYYMMDD(8) '_' HHMMSS(6) '_' mmm(3) '_' base
        std::size_t p1 = 6 + 8;        // YYYYMMDD 结束
        if (p1 >= name.size() || name[p1] != '_') continue;
        std::size_t p2 = p1 + 1 + 6;   // HHMMSS 结束
        if (p2 >= name.size() || name[p2] != '_') continue;
        std::size_t p3 = p2 + 1 + 3;   // mmm 结束
        if (p3 >= name.size() || name[p3] != '_') continue;

        std::string base = name.substr(p3 + 1);
        // 去掉末尾 ".log"
        constexpr const char* DOT_LOG = ".log";
        constexpr std::size_t DOT_LOG_LEN = 4;
        if (base.size() > DOT_LOG_LEN &&
            base.compare(base.size() - DOT_LOG_LEN, DOT_LOG_LEN, DOT_LOG) == 0) {
            base = base.substr(0, base.size() - DOT_LOG_LEN);
        }

        groups[base].push_back(entry);
    }

    // 逐组清理
    for (auto& [base, entries] : groups) {
        if (entries.size() <= static_cast<std::size_t>(MAX_CRASH_BACKUPS_PER_PROCESS)) {
            continue;
        }

        // 按 filename 字典序排序（时间戳保证 == 时间序）
        std::sort(entries.begin(), entries.end(),
                  [](const std::filesystem::directory_entry& a,
                     const std::filesystem::directory_entry& b) {
                      return a.path().filename() < b.path().filename();
                  });

        // 删除最旧的若干，保留最新 MAX_CRASH_BACKUPS_PER_PROCESS 个
        const std::size_t to_remove =
            entries.size() - static_cast<std::size_t>(MAX_CRASH_BACKUPS_PER_PROCESS);
        for (std::size_t i = 0; i < to_remove; ++i) {
            std::error_code rm_ec;
            std::filesystem::remove(entries[i].path(), rm_ec);
        }
    }
}

// 计算进程基名（去掉 core_engine 的 session 后缀）
//   "launcher"           → "launcher"
//   "core_engine_abc123" → "core_engine"
//
// 目前 cleanupOldCrashes 使用归档文件名中的 base 字段进行分组，
// 不依赖本函数；本函数保留为未来"按进程基名统一限流"的接口。
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

    // 写入单个文件：打开（必要时）→ 轮转检查 → 写入 → 累计字节
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

    // ---- 主日志 ----
    write_to(main_filename);

    // ---- ERROR 独立文件（DM_LOG_ERROR_SEPARATE=1 时启用）----
    // 写入失败静默：不调用 LOG_WARN（避免递归）
    if (error_separate_ && level >= LogLevel::ERR) {
        write_to(errorFilename(main_filename));
    }

    // ---- 控制台输出（保留原有行为）----
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

std::string Logger::formatMessage(LogLevel level, const char* file, int line,
                                  const std::string& msg) const {
    // 注：file / line 参数保留以匹配调用宏，当前未使用
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

// 当前线程主日志文件名：
//   channel 为空 → "{process_name}.log"
//   channel 非空 → "{process_name}.{channel}.log"
std::string Logger::currentFilename() const {
    const std::string& channel = t_current_channel;
    if (channel.empty()) {
        return process_name_ + ".log";
    }
    return process_name_ + "." + channel + ".log";
}

// 从主日志文件名推导 ERROR 独立文件名：
//   "launcher.log"                     → "launcher.error.log"
//   "core_engine_x.plugin_y.log"       → "core_engine_x.plugin_y.error.log"
std::string Logger::errorFilename(const std::string& main_filename) const {
    constexpr const char* DOT_LOG = ".log";
    constexpr std::size_t DOT_LOG_LEN = 4;

    if (main_filename.size() > DOT_LOG_LEN &&
        main_filename.compare(main_filename.size() - DOT_LOG_LEN,
                              DOT_LOG_LEN, DOT_LOG) == 0) {
        return main_filename.substr(0, main_filename.size() - DOT_LOG_LEN)
               + ".error.log";
    }
    // 无 .log 后缀时的兜底
    return main_filename + ".error";
}

// ================================================================
// 私有辅助：流管理
// ================================================================

// 获取指定文件的流；不存在则创建并打开（append 模式）。
// 返回 nullptr 表示打开失败。
//
// 注：调用方需持有 mutex_。
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

// 文件大小触发式轮转。
//
// 触发条件（双阈值）：
//   1. 自上次检查以来累计写入 ≥ CHECK_INTERVAL_BYTES（避免频繁 file_size）
//   2. 当前文件大小 ≥ max_file_size_
//
// 轮转命名：
//   {filename}       → {filename}.1
//   {filename}.1     → {filename}.2
//   ...
//   {filename}.N-1   → {filename}.N
//   {filename}.N     → 删除（N = max_backup_files_）
//
// max_backup_files_ ≤ 0 时禁用轮转（文件无限增长）。
//
// 注：调用方需持有 mutex_，且 info.stream 已打开。
void Logger::rotateIfNeeded(const std::string& filename, StreamInfo& info) {
    constexpr std::size_t CHECK_INTERVAL_BYTES = 512 * 1024;  // 512KB

    if (max_backup_files_ <= 0) {
        return;  // 禁用轮转
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

    // ---- 执行轮转 ----
    // 1. 关闭流（Windows 上被占用的文件无法 rename）
    if (info.stream.is_open()) {
        info.stream.flush();
        info.stream.close();
    }

    // 2. 删除最大编号备份
    std::filesystem::path max_backup =
        std::filesystem::path(log_dir_) /
        (filename + "." + std::to_string(max_backup_files_));
    std::filesystem::remove(max_backup, ec);

    // 3. 从大到小 rename：.N-1 → .N, ..., .1 → .2
    for (int i = max_backup_files_ - 1; i >= 1; --i) {
        std::filesystem::path src =
            std::filesystem::path(log_dir_) /
            (filename + "." + std::to_string(i));
        std::filesystem::path dst =
            std::filesystem::path(log_dir_) /
            (filename + "." + std::to_string(i + 1));
        ec.clear();
        std::filesystem::rename(src, dst, ec);
        // src 不存在时忽略（尚未轮转过那么多次）
    }

    // 4. 当前文件 → .1
    std::filesystem::path dst1 =
        std::filesystem::path(log_dir_) / (filename + ".1");
    ec.clear();
    std::filesystem::rename(path, dst1, ec);

    // 5. 重开当前文件（追加模式，必然创建新文件）
    info.stream.open(path.string(), std::ios::out | std::ios::app);
    // 若重开失败，后续 log() 的 is_open() 检查会跳过写入
}
} // namespace dream_machine