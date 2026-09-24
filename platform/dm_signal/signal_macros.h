// platform/dm_signal/signal_macros.h
#pragma once

// ================================================================
// dm_signal —— 便捷发射宏
//
// 提供两个宏，简化信号发射：
//
//   1. SIGNAL_EMIT(payload)
//      最通用的发射：直接转发到 SignalBus::publish
//
//      用法：
//        SignalPayload p;
//        p.type = SignalType::SGT_STARTUP;
//        p.level = SignalLevel::SGL_INFO;
//        p.timestamp_ms = now_ms();
//        p.description = "process started";
//        SIGNAL_EMIT(p);
//
//   2. SIGNAL_EMIT_OP(session_id, op_id, op_type, op_state, level, desc)
//      操作信号的特化：构造 SGT_OP_CONTEXT 信号
//
//      用法：
//        SIGNAL_EMIT_OP(session_id, op_id,
//                       OperationType::OPT_READ_FILE,
//                       OperationState::OPS_PREPARING,
//                       SignalLevel::SGL_INFO,
//                       "准备读取 a.txt");
//
// 设计原则：
//   - 宏内只做转发；复杂构造由 inline 函数完成
//   - 规避 Qt 宏 emit / signals / slots
//   - 零依赖：仅 include signal_types.h + signal_bus.h
// ================================================================

#include "signal_types.h"
#include "signal_bus.h"
#include <utility>

namespace dream_machine::signal {

// ================================================================
// make_op_signal：构造操作信号载荷
//
// 将"操作信号"的构造逻辑集中在此，便于：
//   - 后续修改（如新增字段）只改一处
//   - 调试时可在函数内断点
//   - 单元测试可直接调用（宏不能断点）
//
// 参数：
//   session_id  - 所属会话（可为空；会话无关操作留空）
//   op_id       - 操作唯一标识（用于配对 PREPARING / FINISHING / FAILED）
//   op_type     - 操作类型（OPT_*）
//   op_state    - 操作状态（OPS_*）
//   level       - 信号级别（SGL_*）
//   description - 人类可读描述
//
// 返回值：填好的 SignalPayload（type = SGT_OP_CONTEXT）
// ================================================================
inline SignalPayload make_op_signal(const std::string& session_id,
                                    const std::string& op_id,
                                    OperationType op_type,
                                    OperationState op_state,
                                    SignalLevel level,
                                    const std::string& description) {
    SignalPayload payload;
    payload.type = SignalType::SGT_OP_CONTEXT;
    payload.level = level;
    payload.timestamp_ms = now_ms();
    payload.session_id = session_id;
    payload.description = description;

    OperationInfo info;
    info.type = op_type;
    info.state = op_state;
    info.operation_id = op_id;
    payload.operation = std::move(info);

    return payload;
}

} // namespace dream_machine::signal

// ================================================================
// SIGNAL_EMIT：通用信号发射
//
// 直接转发到 SignalBus::publish。调用方需自行构造 SignalPayload。
// ================================================================
#define SIGNAL_EMIT(payload) \
    ::dream_machine::signal::SignalBus::instance().publish(payload)

// ================================================================
// SIGNAL_EMIT_OP：操作信号发射
//
// 构造 SGT_OP_CONTEXT 信号并发射。
// 构造逻辑在 make_op_signal inline 函数中。
// ================================================================
#define SIGNAL_EMIT_OP(session_id, op_id, op_type, op_state, level, desc) \
    ::dream_machine::signal::SignalBus::instance().publish( \
        ::dream_machine::signal::make_op_signal( \
            (session_id), (op_id), (op_type), (op_state), (level), (desc)))