// platform/dm_logger/logger.h
#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <cstddef>

namespace dream_machine {

    // ================================================================
    // 日志级别
    //
    // 命名规则：所有枚举值避免与 Windows 宏冲突。
    //   - ERROR（wingdi.h）→ 用 ERR
    //   - DEBUG（MSVC Debug 配置 / 某些 SDK）→ 用 DBG
    //
    // 注：本原则（风险规避优先于先例）是项目既有约定——ERR 早已采用
    //     此做法；本文件补齐 DEBUG → DBG，保持一致。
    // ================================================================
    enum class LogLevel {
        DBG,      // 开发调试（默认关闭，由 setMinLevel 控制）
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
    //   - 信号总线订阅（通过内部 Adapter，方案 D）
    //   - 请求关联 ID（A.10 D2：thread_local，跨进程日志关联）
    //
    // 信号订阅（方案 D）：
    //   Logger 不直接继承 ISignalSink；内部通过 LoggerSignalAdapter
    //   （定义在 logger.cpp 的匿名命名空间）订阅 SignalBus。
    //   上层通过静态方法 attach_to_signal_bus() / detach_from_signal_bus()
    //   显式装配/卸载。
    //   原因：保持 logger.h 独立（不 include signal_sink.h），
    //         避免向所有下游传递 dm_signal 依赖。
    //
    // 日志文件命名规范：
    //   {process_name}.log                         主日志
    //   {process_name}.log.N                       轮转文件（N 从 1 开始）
    //   {process_name}.error.log                   ERROR 独立文件
    //   {process_name}.error.log.N                 其轮转
    //   {process_name}.{channel}.log               channel 路由
    //   {process_name}.{channel}.error.log         channel + ERROR
    //   logs/.clean_exit_{process_name}            正常退出标记
    //   logs/crashes/crash_{timestamp}_{process}[_{session_id}].log  崩溃归档
    // ================================================================
    class Logger {
    public:
        static Logger& instance();

        // ============================================================
        // 配置接口
        // ============================================================

        void setProcessName(const std::string& name);
        void setLogDirectory(const std::string& path);
        void setMinLevel(LogLevel level);

        void setMaxFileSize(std::size_t bytes);
        void setMaxBackupFiles(int count);

        // ============================================================
        // 生命周期标记
        // ============================================================

        [[nodiscard]] bool archiveLastSessionIfDirty();
        void markCleanExit();

        // ============================================================
        // Channel 路由
        // ============================================================

        void setChannel(const std::string& channel);
        [[nodiscard]] std::string getChannel() const;

        // ============================================================
        // 请求关联 ID（A.10 D2）
        //
        // 用途：跨进程日志关联。同一请求在不同进程的日志中
        //       携带同一 request_id，便于时间线分析。
        //
        // 语义：
        //   - thread_local：每个线程独立；当前单线程模型下等价于进程内
        //   - 为空时：日志格式不输出 [req:xxx] 前缀（向后兼容）
        //   - 非空时：日志格式输出 [req:xxx] 前缀
        //
        // 使用方式（手动）：
        //   Logger::setRequestId("42");
        //   LOG_INFO("...");  // 日志中出现 [req:42] 前缀
        //   Logger::clearRequestId();
        //
        // 使用方式（RAII，推荐）：
        //   {
        //       RequestIdScope scope("42");   // 构造时 set
        //       LOG_INFO("...");              // 日志携带 [req:42]
        //   }                                 // 析构时自动恢复
        //
        // 建议使用场景：
        //   - 收到跨进程请求时：从消息中提取 request_id
        //   - 处理完毕后：析构自动恢复
        //
        // 线程安全：thread_local，无共享状态。
        // ============================================================

        static void setRequestId(const std::string& request_id);
        static void clearRequestId();
        [[nodiscard]] static std::string getRequestId();

        // ============================================================
        // 信号总线装配（方案 D）
        //
        // attach_to_signal_bus:
        //   将内部 Adapter 订阅到 SignalBus。
        //   幂等：重复调用无副作用。
        //   建议在 main() 启动时调用（在 SignalBus 订阅顺序中排第一）。
        //
        // detach_from_signal_bus:
        //   将内部 Adapter 从 SignalBus 卸载。
        //   幂等：重复调用无副作用。
        //   建议在 main() 退出前调用。
        //
        // 注：Logger 析构中不自动 detach——原因：
        //     Logger 与 SignalBus 均为 Meyers 单例；若 SignalBus 后构造
        //     则先析构，Logger 析构时访问 SignalBus 会导致 UB。
        //     由各进程显式 detach 解决。
        // ============================================================

        static void attach_to_signal_bus();
        static void detach_from_signal_bus();

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
        std::string process_name_;
        std::string log_dir_;
        LogLevel    min_level_;
        std::size_t max_file_size_;
        int         max_backup_files_;
        bool        initialized_;
        bool        error_separate_;

        // ----------------------------------------------------------------
        // 多文件句柄缓存
        //
        // key   = 文件名（不含目录）
        // value = 输出流 + 该文件自上次检查以来的累计写入字节
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

        std::string currentFilename() const;
        std::string errorFilename(const std::string& main_filename) const;

        std::ofstream* getOrOpenStream(const std::string& filename);
        void rotateIfNeeded(const std::string& filename, StreamInfo& info);

        void ensureLogDirectoryExists();

        void archiveLogFile(const std::string& filename);
        void cleanupOldCrashes();

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

    // ================================================================
    // RequestIdScope：request_id 的 RAII 作用域（A.10.3）
    //
    // 用法（推荐）：
    //   {
    //       RequestIdScope scope("42");     // 构造时 setRequestId("42")
    //       LOG_INFO("...");                // 日志携带 [req:42] 前缀
    //       sendResponse();                 // 发出的消息自动携带 request_id
    //   }  // 析构时自动恢复进入作用域之前的 request_id
    //
    // 使用场景：
    //   - 跨进程消息处理：从消息中读出 request_id 后，进入作用域
    //   - 处理完毕：作用域结束自动恢复（无需手动 clearRequestId）
    //
    // 与手动 setRequestId 的关系：
    //   - RequestIdScope 内部调用 setRequestId / getRequestId
    //   - 两者可混用；但推荐优先使用 RAII 形式，避免忘记恢复
    //
    // 嵌套：
    //   - 保存外层 request_id；析构时恢复（而非清空）
    //   - 与 LogChannelScope 行为一致
    //
    // 线程安全：基于 Logger::getRequestId / setRequestId（thread_local），
    //           同一线程内可嵌套使用；跨线程互不影响。
    //
    // 异常安全：析构函数自动调用，栈展开时正常恢复。
    // ================================================================
    class RequestIdScope {
    public:
        explicit RequestIdScope(const std::string& request_id);
        ~RequestIdScope();

        RequestIdScope(const RequestIdScope&) = delete;
        RequestIdScope& operator=(const RequestIdScope&) = delete;

    private:
        std::string saved_request_id_;
    };

} // namespace dream_machine

// ================================================================
// 便捷宏
//
// DBG 级别默认关闭（min_level_ 默认 INFO）。
// 若需输出，调用 Logger::instance().setMinLevel(LogLevel::DBG)。
// ================================================================
#define LOG_DBG(msg)     dream_machine::Logger::instance().log(dream_machine::LogLevel::DBG,   __FILE__, __LINE__, msg)
#define LOG_INFO(msg)    dream_machine::Logger::instance().log(dream_machine::LogLevel::INFO,  __FILE__, __LINE__, msg)
#define LOG_WARN(msg)    dream_machine::Logger::instance().log(dream_machine::LogLevel::WARN,  __FILE__, __LINE__, msg)
#define LOG_ERROR(msg)   dream_machine::Logger::instance().log(dream_machine::LogLevel::ERR,   __FILE__, __LINE__, msg)
#define LOG_FATAL(msg)   dream_machine::Logger::instance().log(dream_machine::LogLevel::FATAL, __FILE__, __LINE__, msg)