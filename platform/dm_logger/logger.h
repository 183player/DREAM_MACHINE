// platform/dm_logger/logger.h
#pragma once

#include <string>
#include <fstream>
#include <mutex>

namespace dream_machine {

    enum class LogLevel {
        INFO,
        WARN,
        ERR,      // 避免与 Windows wingdi.h 中的 ERROR 宏冲突
        FATAL
    };

    class Logger {
    public:
        static Logger& instance();

        void setProcessName(const std::string& name);
        void setLogDirectory(const std::string& path);
        void setMinLevel(LogLevel level);

        void log(LogLevel level, const char* file, int line, const std::string& msg);

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

    private:
        Logger();
        ~Logger();

        std::string process_name_;
        std::string log_dir_;
        LogLevel min_level_;
        std::ofstream file_stream_;
        std::mutex mutex_;
        bool initialized_;

        std::string levelToString(LogLevel level) const;
        std::string currentTimestamp() const;
        std::string formatMessage(LogLevel level, const char* file, int line, const std::string& msg) const;
        void openLogFile();
        void ensureLogDirectoryExists();  // 新增声明
    };

} // namespace dream_machine

#define LOG_INFO(msg)    dream_machine::Logger::instance().log(dream_machine::LogLevel::INFO, __FILE__, __LINE__, msg)
#define LOG_WARN(msg)    dream_machine::Logger::instance().log(dream_machine::LogLevel::WARN, __FILE__, __LINE__, msg)
#define LOG_ERROR(msg)   dream_machine::Logger::instance().log(dream_machine::LogLevel::ERR,  __FILE__, __LINE__, msg)
#define LOG_FATAL(msg)   dream_machine::Logger::instance().log(dream_machine::LogLevel::FATAL, __FILE__, __LINE__, msg)