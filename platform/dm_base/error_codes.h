// platform/dm_base/error_codes.h
#pragma once

#include <string>

namespace dream_machine {

enum class ErrorCode {
    // 通用
    OK = 0,
    UNKNOWN_ERROR,

    // 管道通信
    PIPE_BROKEN,
    PIPE_TIMEOUT,
    PIPE_BUSY,
    PIPE_NOT_FOUND,

    // 文件操作
    FILE_NOT_FOUND,
    FILE_ACCESS_DENIED,
    FILE_IO_ERROR,
    FILE_CORRUPTED,

    // L2 写入
    L2_ERR_MISSING_ANCHOR,
    L2_ERR_INVALID_STATUS,
    L2_ERR_WRITE_FAILED,

    // 进程管理
    PROCESS_LAUNCH_FAILED,
    PROCESS_TERMINATE_FAILED,
    PROCESS_NOT_RESPONDING,

    // 插件
    PLUGIN_MANIFEST_INVALID,
    PLUGIN_EXTRACT_FAILED,
    PLUGIN_LOAD_FAILED,

    // 脚本
    SCRIPT_TIMEOUT,
    SCRIPT_SYNTAX_ERROR,
    SCRIPT_RUNTIME_ERROR,
};

inline const char* errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::OK: return "OK";
        case ErrorCode::UNKNOWN_ERROR: return "Unknown error";
        case ErrorCode::PIPE_BROKEN: return "Pipe broken";
        case ErrorCode::PIPE_TIMEOUT: return "Pipe timeout";
        case ErrorCode::PIPE_BUSY: return "Pipe busy";
        case ErrorCode::PIPE_NOT_FOUND: return "Pipe not found";
        case ErrorCode::FILE_NOT_FOUND: return "File not found";
        case ErrorCode::FILE_ACCESS_DENIED: return "Access denied";
        case ErrorCode::FILE_IO_ERROR: return "File I/O error";
        case ErrorCode::FILE_CORRUPTED: return "File corrupted";
        case ErrorCode::L2_ERR_MISSING_ANCHOR: return "L2: missing source_anchor";
        case ErrorCode::L2_ERR_INVALID_STATUS: return "L2: invalid status";
        case ErrorCode::L2_ERR_WRITE_FAILED: return "L2: write failed";
        case ErrorCode::PROCESS_LAUNCH_FAILED: return "Process launch failed";
        case ErrorCode::PROCESS_TERMINATE_FAILED: return "Process terminate failed";
        case ErrorCode::PROCESS_NOT_RESPONDING: return "Process not responding";
        case ErrorCode::PLUGIN_MANIFEST_INVALID: return "Plugin manifest invalid";
        case ErrorCode::PLUGIN_EXTRACT_FAILED: return "Plugin extract failed";
        case ErrorCode::PLUGIN_LOAD_FAILED: return "Plugin load failed";
        case ErrorCode::SCRIPT_TIMEOUT: return "Script timeout";
        case ErrorCode::SCRIPT_SYNTAX_ERROR: return "Script syntax error";
        case ErrorCode::SCRIPT_RUNTIME_ERROR: return "Script runtime error";
        default: return "Unknown error code";
    }
}

} // namespace dream_machine