// platform/dm_base/constants.h
#pragma once

#include <string>
#include <cstdint>

namespace dream_machine {

// ================================================================
// 错误码定义
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
};

// ================================================================
// 管道名称生成（跨进程通信名称约定）
// ================================================================

// 设计决策：所有进程通过命名管道名称约定进行通信。
// 格式：\\.\pipe\DreamMachine_{角色1}_{角色2}
// 不使用句柄传递（已由专家模式确认不可行）

namespace pipe_names {

// ----- launcher 与三个直接子进程的管道 -----

// launcher ↔ monitor
inline std::string launcher_monitor() {
    return R"(\\.\pipe\DreamMachine_Launcher_Monitor)";
}

// launcher ↔ executor
inline std::string launcher_executor() {
    return R"(\\.\pipe\DreamMachine_Launcher_Executor)";
}

// launcher ↔ gui
inline std::string launcher_gui() {
    return R"(\\.\pipe\DreamMachine_Launcher_Gui)";
}

// ----- monitor ↔ core_engine（多会话） -----

// monitor ↔ core_engine（每个会话独立管道）
// @param session_id  会话标识符（自动进行安全编码，防止非法管道名）
// @return            完整管道名称
std::string monitor_core(const std::string& session_id);

// ----- core_engine ↔ executor -----

// core_engine 连接 executor 的管道名称
// executor 作为服务端创建此管道，core_engine 作为客户端连接
std::string executor_core();

} // namespace pipe_names

// ================================================================
// 其他常量
// ================================================================

namespace constants {

// 超时参数（毫秒）
constexpr int PIPE_WRITE_TIMEOUT_MS = 100;
constexpr int PIPE_READ_TIMEOUT_MS = 3000;
constexpr int PIPE_CONNECT_TIMEOUT_MS = 5000;
constexpr int SUBPROCESS_START_TIMEOUT_MS = 2000;
constexpr int SCRIPT_EXECUTE_TIMEOUT_MS = 5000;

// 管道缓冲区大小
constexpr int PIPE_BUFFER_SIZE = 4096;

// 最大会话数
constexpr int MAX_SESSIONS = 64;

} // namespace constants

} // namespace dream_machine