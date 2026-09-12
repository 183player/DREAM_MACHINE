// platform/dm_logger/logger.h
#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <cstddef>

namespace dream_machine {

    enum class LogLevel {
        INFO,
        WARN,
        ERR,      // 避免与 Windows wingdi.h 中的 ERROR 宏冲突
        FATAL
    };

    // ================================================================
    // Logger：进程内单例，支持
    //   - 多文件轮转（大小 + 进程生命周期双触发）
    //   - 崩溃归档（.clean_exit 标记检测）
    //   - channel 路由（thread_local 语义，用于插件/脚本日志隔离）
    //   - ERROR 独立文件（DM_LOG_ERROR_SEPARATE=1 启用）
    //
    // 日志文件命名规范（契约 #4 / #8 / #11 / #16）：
    //   {process_name}.log                         主日志
    //   {process_name}.log.N                       轮转文件（N 从 1 开始）
    //   {process_name}.error.log                   ERROR 独立文件
    //   {process_name}.error.log.N                 其轮转
    //   {process_name}.{channel}.log               channel 路由
    //   {process_name}.{channel}.error.log         channel + ERROR
    //   logs/.clean_exit_{process_name}            正常退出标记
    //   logs/crashes/crash_{timestamp}_{process}[_{session_id}].log  崩溃归档
    //
    // 其中 {process_name} 由 setProcessName() 决定：
    //   - 普通进程："launcher" / "monitor" / "executor" / "gui"
    //   - core_engine："core_engine_{session_id}"
    // ================================================================
    class Logger {
    public:
        static Logger& instance();

        // ============================================================
        // 配置接口
        // ============================================================

        // 设置进程名（决定日志文件名前缀）
        // 若已初始化，会立即切换到新文件
        void setProcessName(const std::string& name);

        void setLogDirectory(const std::string& path);
        void setMinLevel(LogLevel level);

        // 单文件大小上限（默认 10MB），超过后触发轮转
        void setMaxFileSize(std::size_t bytes);

        // 每个日志文件的轮转保留数（默认 10），超过后删除最旧
        void setMaxBackupFiles(int count);

        // ============================================================
        // 生命周期标记
        // ============================================================

        // 启动时调用：检测上次是否异常退出
        //   - 若 logs/.clean_exit_{process_name} 存在 → 上次正常退出
        //     → 删除标记，返回 false
        //   - 否则 → 上次异常退出（或首次启动）
        //     → 若 {process_name}.log 存在，归档到 logs/crashes/
        //     → 返回 true（发生了归档）；无日志可归档时返回 false
        //
        // 副作用：
        //   - 归档后触发 cleanupOldCrashes()，按进程基名分组保留最新 20 个
        [[nodiscard]] bool archiveLastSessionIfDirty();

        // 正常退出时调用：写入 logs/.clean_exit_{process_name}
        // 在下一次启动时被 archiveLastSessionIfDirty() 消费
        void markCleanExit();

        // ============================================================
        // Channel 路由（thread_local 语义）
        //
        // 当前线程 channel 为空时，写入 {process_name}.log；
        // 非空时，写入 {process_name}.{channel}.log。
        //
        // channel 命名建议（契约 #9）：
        //   plugin_{name}    插件日志
        //   script_{name}    脚本日志
        //
        // 推荐使用 LogChannelScope（RAII），避免手动恢复。
        // ============================================================

        void setChannel(const std::string& channel);
        [[nodiscard]] std::string getChannel() const;

        // ============================================================
        // 核心日志接口
        // ============================================================

        void log(LogLevel level, const char* file, int line, const std::string& msg);

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

    private:
        Logger();
        ~Logger();

        // ----------------------------------------------------------------
        // 配置
        // ----------------------------------------------------------------
        std::string process_name_;     // 例如 "launcher" / "core_engine_xxx"
        std::string log_dir_;          // 默认 "./logs"
        LogLevel    min_level_;        // 默认 INFO
        std::size_t max_file_size_;    // 默认 10MB
        int         max_backup_files_; // 默认 10
        bool        initialized_;      // 首次 log() 后置 true
        bool        error_separate_;   // DM_LOG_ERROR_SEPARATE=1 时启用

        // ----------------------------------------------------------------
        // 多文件句柄缓存
        //
        // key   = 文件名（不含目录），如 "launcher.log"
        //        或 "core_engine_xxx.plugin_yyy.log"
        // value = 输出流 + 该文件自上次检查以来的累计写入字节
        //
        // 由 mutex_ 保护。
        // ----------------------------------------------------------------
        struct StreamInfo {
            std::ofstream stream;
            std::size_t   bytes_since_check = 0;
        };
        std::unordered_map<std::string, StreamInfo> streams_;
        std::mutex mutex_;

        // ----------------------------------------------------------------
        // 内部辅助
        // ----------------------------------------------------------------
        std::string levelToString(LogLevel level) const;
        std::string currentTimestamp() const;
        std::string formatMessage(LogLevel level, const char* file, int line,
                                  const std::string& msg) const;

        // 计算当前线程主日志文件名（含 ".log"），如 "launcher.log"
        std::string currentFilename() const;

        // 由主日志文件名推导 ERROR 独立文件名
        //   "launcher.log" → "launcher.error.log"
        //   "core_engine_x.plugin_y.log" → "core_engine_x.plugin_y.error.log"
        std::string errorFilename(const std::string& main_filename) const;

        // 获取（必要时创建）指定文件的流，返回指针；失败返回 nullptr
        std::ofstream* getOrOpenStream(const std::string& filename);

        // 检查文件大小并在必要时轮转
        void rotateIfNeeded(const std::string& filename, StreamInfo& info);

        // 确保 logs/ 目录存在
        void ensureLogDirectoryExists();

        // 将指定文件归档到 logs/crashes/，reason_suffix 附加到归档文件名
        void archiveLogFile(const std::string& filename);

        // 清理 logs/crashes/ 中的旧归档，按进程基名分组保留最新 N 个
        void cleanupOldCrashes();

        // 计算进程基名（去掉 session_id 后缀）
        //   "launcher"              → "launcher"
        //   "core_engine_abc123"    → "core_engine"
        std::string processBaseName() const;
    };

    // ================================================================
    // LogChannelScope：channel 的 RAII 作用域
    //
    // 用法：
    //   {
    //       LogChannelScope scope("plugin_my_plugin");
    //       LOG_INFO("这条写入 launcher.plugin_my_plugin.log");
    //   }  // 析构时自动恢复外层 channel
    //
    // 线程安全：基于 Logger::getChannel / setChannel（thread_local），
    //           同一线程内可嵌套使用；跨线程互不影响。
    // ================================================================
    class LogChannelScope {
    public:
        explicit LogChannelScope(const std::string& channel);
        ~LogChannelScope();

        LogChannelScope(const LogChannelScope&) = delete;
        LogChannelScope& operator=(const LogChannelScope&) = delete;

    private:
        std::string saved_channel_;
    };

} // namespace dream_machine

#define LOG_INFO(msg)    dream_machine::Logger::instance().log(dream_machine::LogLevel::INFO, __FILE__, __LINE__, msg)
#define LOG_WARN(msg)    dream_machine::Logger::instance().log(dream_machine::LogLevel::WARN, __FILE__, __LINE__, msg)
#define LOG_ERROR(msg)   dream_machine::Logger::instance().log(dream_machine::LogLevel::ERR,  __FILE__, __LINE__, msg)
#define LOG_FATAL(msg)   dream_machine::Logger::instance().log(dream_machine::LogLevel::FATAL, __FILE__, __LINE__, msg)