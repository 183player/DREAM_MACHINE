// platform/dm_base/error_notify_bridge.h
#pragma once

// ================================================================
// dm_base —— ErrorContext → ErrorNotifyMessage 单一映射
//
// 依据：
//   - D5 §8.2：错误模型单一映射路径（硬性约束）
//   - D3 DREAM-010：ErrorSeverity → SignalLevel 映射规则
//   - D4 §3.5 一致性约束 #2：错误模型只允许一条映射路径
//
// 职责：
//   1. error_context_to_notify(ctx, source)
//        字段级映射：ErrorContext → ErrorNotifyMessage
//        供各进程构造 ErrorNotifyMessage 并写管道
//
//   2. error_severity_to_signal_level_int(sev)
//        ErrorSeverity → SGL_* 枚举的 int 值
//        避免 dm_base include dm_signal（保持依赖图分层）
//        调用方负责 static_cast<signal::SignalLevel>(...)
//
// 设计原则：
//   - 纯 inline 函数，无 .cpp，无 CMake 改动
//   - 只 include error_codes.h + messages.h（同属 dm_base）
//   - 不 include dm_signal / dm_logger（保持层次清晰）
//   - 映射规则集中在本文档，禁止重复定义
//
// 映射规则（D3 DREAM-010）：
//   ErrorSeverity::NONE / DEBUG → SGL_DEBUG (0)
//   ErrorSeverity::INFO         → SGL_INFO  (1)
//   ErrorSeverity::WARNING      → SGL_WARN  (2)
//   ErrorSeverity::RECOVERABLE  → SGL_ERROR (3)
//   ErrorSeverity::FATAL / PANIC→ SGL_FATAL (4)
//
// 字段映射规则（D5 §8.2）：
//   msg.source   ← source（显式传入，标识发起者进程；ctx.source 作为补充）
//   msg.severity ← severityToString(ctx.severity)
//   msg.message  ← ctx.message（人类可读优先）；为空时回退为 errorCodeToString(ctx.code)
//   msg.details  ← "code=XXX" 前缀（若 code != SUCCESS）+ ctx.details（若有）
//
// 使用示例（各进程 isBroken 分支）：
//   ErrorContext ctx = ErrorContext::make(
//       ErrorCode::PIPE_BROKEN,
//       ErrorSeverity::RECOVERABLE,
//       "Monitor pipe broken",
//       "monitor/main.cpp",
//       "isBroken() returned true");
//
//   // 方式 1：直接写管道（发给明确目标）
//   ErrorNotifyMessage notify = error_context_to_notify(ctx, "monitor");
//   target_pipe.writeLine(serializeErrorNotify(notify));
//
//   // 方式 2：走 SignalBus（推荐，A.8 已装配）
//   signal::SignalPayload payload;
//   payload.type = signal::SignalType::SGT_ERROR_NOTIFY;
//   payload.level = static_cast<signal::SignalLevel>(
//       error_severity_to_signal_level_int(ctx.severity));
//   payload.timestamp_ms = signal::now_ms();
//   payload.description  = ctx.message;
//   payload.detail       = ctx.details;
//   signal::SignalBus::instance().publish(payload);
// ================================================================

#include "error_codes.h"   // ErrorContext / ErrorSeverity / severityToString
#include "messages.h"      // ErrorNotifyMessage

#include <string>

namespace dream_machine {

// ================================================================
// error_context_to_notify
//
// 字段级映射：ErrorContext → ErrorNotifyMessage
//
// 参数：
//   ctx    - 已构造的错误上下文
//   source - 发起者进程名（如 "monitor" / "executor"）；
//            会覆盖 ctx.source 作为对外 source 字段
//            若 source 为空，则使用 ctx.source
//
// 返回值：
//   ErrorNotifyMessage —— 可直接 serializeErrorNotify + writeLine
//
// 说明：
//   - severity 用 severityToString() 转字符串（ERROR_NOTIFY 协议约定）
//   - message 优先人类可读；为空时回退错误码
//   - details 携带 "code=XXX" + 原始 details（便于日志分析）
// ================================================================
[[nodiscard]] inline ErrorNotifyMessage
error_context_to_notify(const ErrorContext& ctx, const std::string& source) {
    ErrorNotifyMessage msg;

    // ---- source：显式传入优先；否则回退 ctx.source ----
    msg.source = !source.empty() ? source : ctx.source;

    // ---- severity：ErrorSeverity → 字符串 ----
    msg.severity = severityToString(ctx.severity);

    // ---- message：人类可读优先；为空时回退错误码 ----
    if (!ctx.message.empty()) {
        msg.message = ctx.message;
    } else {
        msg.message = errorCodeToString(ctx.code);
    }

    // ---- details：错误码前缀 + 原始 details ----
    std::string details;
    if (ctx.code != ErrorCode::SUCCESS) {
        details = std::string("code=") + errorCodeToString(ctx.code);
    }
    if (!ctx.details.empty()) {
        if (!details.empty()) {
            details += " | ";
        }
        details += ctx.details;
    }
    if (!details.empty()) {
        msg.details = details;
    }

    return msg;
}

// ================================================================
// error_severity_to_signal_level_int
//
// ErrorSeverity → SGL_* 枚举的 int 数值
//
// 为什么返回 int 而不是 signal::SignalLevel：
//   - dm_base 不依赖 dm_signal（保持依赖图分层）
//   - dm_signal::SignalLevel 的数值稳定（0..4），本函数以 int 暴露
//   - 调用方显式 static_cast<signal::SignalLevel>(...) 恢复类型
//
// 映射规则（D3 DREAM-010，硬性约束）：
//   ErrorSeverity::NONE         → 0 (SGL_DEBUG)
//   ErrorSeverity::DEBUG        → 0 (SGL_DEBUG)
//   ErrorSeverity::INFO         → 1 (SGL_INFO)
//   ErrorSeverity::WARNING      → 2 (SGL_WARN)
//   ErrorSeverity::RECOVERABLE  → 3 (SGL_ERROR)
//   ErrorSeverity::FATAL        → 4 (SGL_FATAL)
//   ErrorSeverity::PANIC        → 4 (SGL_FATAL)
//
// 注：穷尽 switch，无 default 分支干扰编译期检查。
// ================================================================
[[nodiscard]] inline int
error_severity_to_signal_level_int(ErrorSeverity sev) {
    switch (sev) {
        case ErrorSeverity::NONE:        return 0;  // SGL_DEBUG
        case ErrorSeverity::DEBUG:       return 0;  // SGL_DEBUG
        case ErrorSeverity::INFO:        return 1;  // SGL_INFO
        case ErrorSeverity::WARNING:     return 2;  // SGL_WARN
        case ErrorSeverity::RECOVERABLE: return 3;  // SGL_ERROR
        case ErrorSeverity::FATAL:       return 4;  // SGL_FATAL
        case ErrorSeverity::PANIC:       return 4;  // SGL_FATAL
    }
    // 兜底（穷尽 switch 下不会到达；保留以消除 -Wreturn-type 警告）
    return 4;
}

} // namespace dream_machine