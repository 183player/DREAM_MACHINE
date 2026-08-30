// platform/dm_base/error_codes.h
#pragma once

#include "constants.h"   // 使用 ErrorCode 和 errorCodeToString
#include <string>
#include <cstdint>

namespace dream_machine {

// ================================================================
// 错误严重级别
// ================================================================
enum class ErrorSeverity : uint8_t {
    NONE = 0,
    DEBUG = 1,
    INFO = 2,
    WARNING = 3,
    RECOVERABLE = 4,
    FATAL = 5,
    PANIC = 6
};

// ================================================================
// 错误上下文
// ================================================================
struct ErrorContext {
    ErrorCode code = ErrorCode::SUCCESS;
    ErrorSeverity severity = ErrorSeverity::NONE;
    std::string message;
    std::string source;
    std::string details;

    static ErrorContext ok() {
        return {ErrorCode::SUCCESS, ErrorSeverity::NONE, "OK", "", ""};
    }

    static ErrorContext make(ErrorCode code, ErrorSeverity severity,
                             const std::string& msg,
                             const std::string& src = "",
                             const std::string& det = "") {
        return {code, severity, msg, src, det};
    }

    [[nodiscard]] bool isSuccess() const {
        return code == ErrorCode::SUCCESS;
    }

    [[nodiscard]] bool isFatal() const {
        return severity == ErrorSeverity::FATAL || severity == ErrorSeverity::PANIC;
    }

    [[nodiscard]] bool isRecoverable() const {
        return severity == ErrorSeverity::RECOVERABLE;
    }

    [[nodiscard]] std::string toString() const {
        std::string out = "[" + std::to_string(static_cast<int>(code)) + "] ";
        out += message;
        if (!source.empty()) out += " (from " + source + ")";
        if (!details.empty()) out += ": " + details;
        return out;
    }
};

// ================================================================
// 辅助：将 ErrorSeverity 转为字符串
// ================================================================
inline const char* severityToString(ErrorSeverity sev) {
    switch (sev) {
        case ErrorSeverity::NONE: return "NONE";
        case ErrorSeverity::DEBUG: return "DEBUG";
        case ErrorSeverity::INFO: return "INFO";
        case ErrorSeverity::WARNING: return "WARNING";
        case ErrorSeverity::RECOVERABLE: return "RECOVERABLE";
        case ErrorSeverity::FATAL: return "FATAL";
        case ErrorSeverity::PANIC: return "PANIC";
        default: return "UNKNOWN";
    }
}

} // namespace dream_machine