// platform/dm_signal/signal_types.h
#pragma once

// ================================================================
// dm_signal —— 进程内信号总线：类型定义
//
// 本文件是 dm_signal 的基础，定义：
//   - SignalType      : 信号的种类（路由到不同订阅者）
//   - SignalLevel     : 信号的严重级别（与 LogLevel 一一对应）
//   - OperationType   : 原子操作的类别（仅 OP_CONTEXT 时有效）
//   - OperationState  : 原子操作的状态（仅 OP_CONTEXT 时有效）
//   - OperationInfo   : 操作相关字段的聚合
//   - SignalPayload   : 信号的完整载荷
//   - now_ms()        : 毫秒时间戳辅助
//
// 设计原则：
//   - 零依赖：仅使用 C++ 标准库，不 include 任何项目头文件
//   - POD 风格：结构体成员不加后缀（与 plugin_types.h 一致）
//   - 枚举值加前缀：完全规避 windows.h 宏污染 + C 标准保留前缀
//     前缀规则：
//       SignalLevel    → SGL_        （避开 C 标准保留的 SIG_）
//       SignalType     → SGT_        （同上）
//       OperationType  → OPT_        （OP_ 无保留，但 OPT_ 更清晰）
//       OperationState → OPS_        （OP_STATE_ 过长，缩写为 OPS_）
//   - 值域封闭 + CUSTOM 扩展：核心路径类型安全，插件可受控扩展
// ================================================================

#include <cstdint>
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace dream_machine::signal {

// ================================================================
// SignalLevel：信号严重级别
//
// 与 LogLevel 一一对应（Logger 订阅者会做映射）：
//   SGL_DEBUG  → LogLevel::DEBUG
//   SGL_INFO   → LogLevel::INFO
//   SGL_WARN   → LogLevel::WARN
//   SGL_ERROR  → LogLevel::ERR
//   SGL_FATAL  → LogLevel::FATAL
//
// 与 ErrorSeverity 的映射在 dm_base 上层完成（避免 dm_signal 依赖 dm_base）
// ================================================================
enum class SignalLevel : std::uint8_t {
    SGL_DEBUG = 0,
    SGL_INFO  = 1,
    SGL_WARN  = 2,
    SGL_ERROR = 3,
    SGL_FATAL = 4
};

// ================================================================
// SignalType：信号的种类
//
// 用于将信号路由到不同的订阅者。
//
// 语义边界：
//   - 本枚举描述的是"信号的种类"
//   - 具体的操作类别（read_file 等）在 OperationType 中
//   - 具体的操作状态（RUNNING 等）在 OperationState 中
//
// SGT_OP_CONTEXT 是"原子操作上下文"的载体信号，
// 携带 OperationInfo（含 OperationType + OperationState）。
//
// SGT_CUSTOM 用于插件扩展：配合 SignalPayload::custom_type 字符串。
// ================================================================
enum class SignalType : std::uint8_t {
    // ---- 操作生命周期 ----
    SGT_OP_CONTEXT              = 0,    // 原子操作上下文（含 OperationInfo）

    // ---- 错误 ----
    SGT_ERROR_NOTIFY            = 10,   // 通用错误通知
    SGT_ENGINE_DIED             = 11,   // core_engine 死亡（monitor 发出）

    // ---- 状态变更 ----
    SGT_SESSION_STATE_CHANGED   = 20,   // 会话状态变化
    SGT_PROCESS_STATE_CHANGED   = 21,   // 进程状态变化

    // ---- 生命周期 ----
    SGT_STARTUP                 = 30,   // 进程启动
    SGT_SHUTDOWN_REQUESTED      = 31,   // 关机请求

    // ---- 扩展 ----
    SGT_CUSTOM                  = 100   // 插件自定义（配合 custom_type）
};

// ================================================================
// OperationType：原子操作的类别
//
// 仅在 SignalType::SGT_OP_CONTEXT 时有效。
//
// 核心操作集（封闭）：
//   - 与 IMPROVEMENTS.md A2 规划的原子操作集一致
//
// 扩展：
//   - OPT_CUSTOM 配合 SignalPayload::custom_type 字符串（插件扩展）
// ================================================================
enum class OperationType : std::uint8_t {
    OPT_READ_FILE        = 0,    // 读文件
    OPT_WRITE_FILE       = 1,    // 写文件
    OPT_DELETE_FILE      = 2,    // 删文件
    OPT_LIST_DIR         = 3,    // 列目录
    OPT_ATOMIC_WRITE_L2  = 4,    // L2 原子写入（副本 + 替换）
    OPT_CUSTOM           = 100   // 插件自定义
};

// ================================================================
// OperationState：原子操作的状态
//
// 仅在 SignalType::SGT_OP_CONTEXT 时有效。
//
// 状态机（单向迁移，除 FAILED 可从任意态进入）：
//
//   OPS_IDLE
//     ↓ (发起操作)
//   OPS_PREPARING   ← executor emit SGT_OP_CONTEXT{state=OPS_PREPARING}
//     ↓
//   OPS_RUNNING     ← 可选：执行中 emit
//     ↓
//   OPS_FINISHING   ← 执行完成，emit SGT_OP_CONTEXT{state=OPS_FINISHING}
//     ↓
//   OPS_IDLE
//
//   OPS_FAILED      ← 任何阶段都可能进入；emit 后回到 OPS_IDLE
//
// 用途：
//   - OperationTracker 订阅后维护本地状态
//   - 退出分类判定时作为"崩溃瞬间是否在执行操作"的依据
// ================================================================
enum class OperationState : std::uint8_t {
    OPS_IDLE       = 0,    // 无操作
    OPS_PREPARING  = 1,    // 准备执行（已 emit PREPARING）
    OPS_RUNNING    = 2,    // 执行中
    OPS_FINISHING  = 3,    // 执行完毕（已 emit FINISHING）
    OPS_FAILED     = 4     // 失败（emit FAILED 后回到 IDLE）
};

// ================================================================
// OperationInfo：操作相关字段的聚合
//
// 仅在 SignalType::SGT_OP_CONTEXT 时由 SignalPayload::operation 携带。
//
// operation_id 用于配对同一操作的 PREPARING / FINISHING / FAILED 信号；
// 由 executor 生成（建议用 UUID 或递增整数）。
// ================================================================
struct OperationInfo {
    OperationType  type = OperationType::OPT_CUSTOM;
    OperationState state = OperationState::OPS_IDLE;
    std::string    operation_id;    // 唯一标识，用于配对
};

// ================================================================
// SignalPayload：信号的完整载荷
//
// 字段分三类：
//   1. 结构化字段（程序自用，判定依据）：
//        type / level / timestamp_ms / session_id / operation
//   2. 人类可读字段（日志用，判定不依赖）：
//        description / detail
//   3. 扩展字段（插件）：
//        custom_type / extra
//
// 默认构造合法（type=SGT_CUSTOM / level=SGL_INFO / timestamp=0），
// 便于上层用"填充 + emit"模式。
// ================================================================
struct SignalPayload {
    // ---- 结构化字段（程序自用） ----
    SignalType  type  = SignalType::SGT_CUSTOM;
    SignalLevel level = SignalLevel::SGL_INFO;
    std::int64_t timestamp_ms = 0;

    // 会话标识（可选；launcher / 系统级信号可留空）
    std::string session_id;

    // 操作信息（仅 SGT_OP_CONTEXT 时有效）
    std::optional<OperationInfo> operation;

    // ---- 人类可读字段（日志用） ----
    std::string description;    // 简要描述（如"准备读取 a.txt"）
    std::string detail;         // 补充详情（如错误堆栈）

    // ---- 扩展字段（插件） ----
    std::string custom_type;    // 仅 type == SGT_CUSTOM 时有效
    std::map<std::string, std::string> extra;
};

// ================================================================
// now_ms：毫秒时间戳辅助
//
// 用途：填充 SignalPayload::timestamp_ms。
//
// 注：使用 system_clock（可跨进程比较），非 steady_clock。
//     秒级精度足够，毫秒级用于退出分类的窗口判定。
// ================================================================
inline std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

} // namespace dream_machine::signal