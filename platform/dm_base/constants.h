// platform/dm_base/constants.h
#pragma once

#include <string>
#include <cstdint>

namespace dream_machine {

// ================================================================
// 错误码定义（扩展版本）
// ================================================================
enum class ErrorCode : int32_t {
    SUCCESS = 0,
    UNKNOWN_ERROR = 1,
    INVALID_PARAM = 2,
    NOT_FOUND = 3,
    ALREADY_EXISTS = 4,
    PERMISSION_DENIED = 5,
    TIMEOUT = 6,
    PROCESS_ERROR = 7,
    PIPE_ERROR = 8,
    IO_ERROR = 9,
    JSON_ERROR = 10,

    // ---- 新增通用错误 (100+) ----
    UNKNOWN = 100,
    NOT_IMPLEMENTED = 101,

    // ---- 资源错误 (200+) ----
    RESOURCE_UNAVAILABLE = 200,
    RESOURCE_EXHAUSTED = 201,
    FILE_NOT_FOUND = 202,
    FILE_ACCESS_DENIED = 203,
    FILE_IO_ERROR = 204,

    // ---- 进程错误 (300+) ----
    PROCESS_LAUNCH_FAILED = 300,
    PROCESS_TERMINATED = 301,
    PROCESS_CRASHED = 302,
    PROCESS_TIMEOUT = 303,

    // ---- 管道错误 (400+) ----
    PIPE_CREATE_FAILED = 400,
    PIPE_CONNECT_FAILED = 401,
    PIPE_BROKEN = 402,
    PIPE_TIMEOUT = 403,
    PIPE_INVALID_STATE = 404,

    // ---- 插件错误 (500+) ----
    PLUGIN_MANIFEST_INVALID = 500,
    PLUGIN_LOAD_FAILED = 501,
    PLUGIN_VERIFY_FAILED = 502,
    PLUGIN_DISABLED = 503,

    // ---- 会话错误 (600+) ----
    SESSION_NOT_FOUND = 600,
    SESSION_ALREADY_RUNNING = 601,
    SESSION_MAX_REACHED = 602,
    SESSION_ENGINE_FAILED = 603,
    SESSION_SHUTDOWN = 604,

    // ---- 脚本错误 (700+) ----
    SCRIPT_PARSE_ERROR = 700,
    SCRIPT_EXECUTION_ERROR = 701,
    SCRIPT_TIMEOUT = 702,

    // ---- 配置错误 (800+) ----
    CONFIG_NOT_FOUND = 800,
    CONFIG_PARSE_ERROR = 801,
    CONFIG_INVALID = 802,

    // ---- 系统错误 (900+) ----
    SYSTEM_ERROR = 900,
    SYSTEM_UNSUPPORTED = 901
};

// ================================================================
// 管道名称生成（跨进程通信名称约定）
// ================================================================
namespace pipe_names {

// launcher ↔ 子进程
inline std::string launcher_monitor() {
    return R"(\\.\pipe\DreamMachine_Launcher_Monitor)";
}
inline std::string launcher_executor() {
    return R"(\\.\pipe\DreamMachine_Launcher_Executor)";
}
inline std::string launcher_gui() {
    return R"(\\.\pipe\DreamMachine_Launcher_Gui)";
}

// monitor ↔ core_engine
std::string monitor_core(const std::string& session_id);

// core_engine ↔ executor
std::string executor_core();

} // namespace pipe_names

// ================================================================
// 其他常量
// ================================================================
namespace constants {
    constexpr int PIPE_WRITE_TIMEOUT_MS = 100;
    constexpr int PIPE_READ_TIMEOUT_MS = 3000;
    constexpr int PIPE_CONNECT_TIMEOUT_MS = 5000;
    constexpr int SUBPROCESS_START_TIMEOUT_MS = 2000;
    constexpr int SCRIPT_EXECUTE_TIMEOUT_MS = 5000;
    constexpr int PIPE_BUFFER_SIZE = 4096;
    constexpr int MAX_SESSIONS = 64;
}

// ================================================================
// 错误码转字符串（辅助函数）
// ================================================================
inline const char* errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS: return "SUCCESS";
        case ErrorCode::UNKNOWN_ERROR: return "UNKNOWN_ERROR";
        case ErrorCode::INVALID_PARAM: return "INVALID_PARAM";
        case ErrorCode::NOT_FOUND: return "NOT_FOUND";
        case ErrorCode::ALREADY_EXISTS: return "ALREADY_EXISTS";
        case ErrorCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
        case ErrorCode::TIMEOUT: return "TIMEOUT";
        case ErrorCode::PROCESS_ERROR: return "PROCESS_ERROR";
        case ErrorCode::PIPE_ERROR: return "PIPE_ERROR";
        case ErrorCode::IO_ERROR: return "IO_ERROR";
        case ErrorCode::JSON_ERROR: return "JSON_ERROR";
        case ErrorCode::UNKNOWN: return "UNKNOWN";
        case ErrorCode::NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
        case ErrorCode::RESOURCE_UNAVAILABLE: return "RESOURCE_UNAVAILABLE";
        case ErrorCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
        case ErrorCode::FILE_NOT_FOUND: return "FILE_NOT_FOUND";
        case ErrorCode::FILE_ACCESS_DENIED: return "FILE_ACCESS_DENIED";
        case ErrorCode::FILE_IO_ERROR: return "FILE_IO_ERROR";
        case ErrorCode::PROCESS_LAUNCH_FAILED: return "PROCESS_LAUNCH_FAILED";
        case ErrorCode::PROCESS_TERMINATED: return "PROCESS_TERMINATED";
        case ErrorCode::PROCESS_CRASHED: return "PROCESS_CRASHED";
        case ErrorCode::PROCESS_TIMEOUT: return "PROCESS_TIMEOUT";
        case ErrorCode::PIPE_CREATE_FAILED: return "PIPE_CREATE_FAILED";
        case ErrorCode::PIPE_CONNECT_FAILED: return "PIPE_CONNECT_FAILED";
        case ErrorCode::PIPE_BROKEN: return "PIPE_BROKEN";
        case ErrorCode::PIPE_TIMEOUT: return "PIPE_TIMEOUT";
        case ErrorCode::PIPE_INVALID_STATE: return "PIPE_INVALID_STATE";
        case ErrorCode::PLUGIN_MANIFEST_INVALID: return "PLUGIN_MANIFEST_INVALID";
        case ErrorCode::PLUGIN_LOAD_FAILED: return "PLUGIN_LOAD_FAILED";
        case ErrorCode::PLUGIN_VERIFY_FAILED: return "PLUGIN_VERIFY_FAILED";
        case ErrorCode::PLUGIN_DISABLED: return "PLUGIN_DISABLED";
        case ErrorCode::SESSION_NOT_FOUND: return "SESSION_NOT_FOUND";
        case ErrorCode::SESSION_ALREADY_RUNNING: return "SESSION_ALREADY_RUNNING";
        case ErrorCode::SESSION_MAX_REACHED: return "SESSION_MAX_REACHED";
        case ErrorCode::SESSION_ENGINE_FAILED: return "SESSION_ENGINE_FAILED";
        case ErrorCode::SESSION_SHUTDOWN: return "SESSION_SHUTDOWN";
        case ErrorCode::SCRIPT_PARSE_ERROR: return "SCRIPT_PARSE_ERROR";
        case ErrorCode::SCRIPT_EXECUTION_ERROR: return "SCRIPT_EXECUTION_ERROR";
        case ErrorCode::SCRIPT_TIMEOUT: return "SCRIPT_TIMEOUT";
        case ErrorCode::CONFIG_NOT_FOUND: return "CONFIG_NOT_FOUND";
        case ErrorCode::CONFIG_PARSE_ERROR: return "CONFIG_PARSE_ERROR";
        case ErrorCode::CONFIG_INVALID: return "CONFIG_INVALID";
        case ErrorCode::SYSTEM_ERROR: return "SYSTEM_ERROR";
        case ErrorCode::SYSTEM_UNSUPPORTED: return "SYSTEM_UNSUPPORTED";
        default: return "UNKNOWN_CODE";
    }
}

} // namespace dream_machine