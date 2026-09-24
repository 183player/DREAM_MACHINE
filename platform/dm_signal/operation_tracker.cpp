// platform/dm_signal/operation_tracker.cpp
#include "operation_tracker.h"
#include "signal_bus.h"

namespace dream_machine::signal {

// ================================================================
// 单例访问
//
// Meyers 单例：C++11 保证静态局部变量初始化线程安全。
// 生命周期：进程退出时自动析构。
// ================================================================
OperationTracker& OperationTracker::instance() {
    static OperationTracker tracker;
    return tracker;
}

// ================================================================
// 构造 / 析构
//
// 构造：无操作（不在此 subscribe；装配由各进程 main() 显式完成）
//
// 析构：从 SignalBus 注销
//
// 析构顺序保证（重要）：
//   - 装配时：SignalBus::instance() 先被调用（构造 SignalBus），
//             再调用 OperationTracker::instance()（构造自身）
//   - C++ 保证：Meyers 单例按构造的相反顺序析构
//   - 因此：OperationTracker 先析构（SignalBus 仍存活）→ 此处安全
//
// 若未来在 OperationTracker 构造中主动 subscribe，
// 需重新评估析构顺序（避免访问已析构的 SignalBus）。
// ================================================================
OperationTracker::OperationTracker() = default;

OperationTracker::~OperationTracker() {
    SignalBus::instance().unsubscribe(this);
}

// ================================================================
// ISignalSink 接口：on_signal
//
// 处理流程（按顺序过滤）：
//   1. 类型必须是 SGT_OP_CONTEXT —— 其他类型静默忽略
//   2. session_id 非空 —— 操作信号必须有会话
//   3. operation 有值 —— 协议约定 OP_CONTEXT 必带 OperationInfo
//   4. 更新对应会话的记录：
//        record.previous = record.current
//        record.current  = *payload.operation
//        record.last_op_timestamp_ms = payload.timestamp_ms
//
// 首次操作时：
//   - operations_[session_id] 自动默认构造一个 OperationRecord
//   - 其 current / previous 均为 {OPT_CUSTOM, OPS_IDLE, ""}
//   - previous 保留该默认值（语义："从未有过操作"）
// ================================================================
void OperationTracker::on_signal(const SignalPayload& payload) {
    // ---- 过滤 1：只处理操作上下文信号 ----
    if (payload.type != SignalType::SGT_OP_CONTEXT) {
        return;
    }

    // ---- 过滤 2：必须有 session_id ----
    if (payload.session_id.empty()) {
        return;
    }

    // ---- 过滤 3：必须携带 OperationInfo ----
    if (!payload.operation.has_value()) {
        return;
    }

    // ---- 更新记录 ----
    OperationRecord& record = operations_[payload.session_id];
    record.previous = record.current;              // 旧 current → previous
    record.current  = *payload.operation;          // 新操作 → current
    record.last_op_timestamp_ms = payload.timestamp_ms;
}

// ================================================================
// 查询接口
// ================================================================

std::optional<OperationRecord>
OperationTracker::get_record(const std::string& session_id) const {
    auto it = operations_.find(session_id);
    if (it == operations_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<OperationInfo>
OperationTracker::get_current(const std::string& session_id) const {
    auto it = operations_.find(session_id);
    if (it == operations_.end()) {
        return std::nullopt;
    }
    return it->second.current;
}

std::optional<OperationInfo>
OperationTracker::get_previous(const std::string& session_id) const {
    auto it = operations_.find(session_id);
    if (it == operations_.end()) {
        return std::nullopt;
    }
    return it->second.previous;
}

OperationState
OperationTracker::get_state(const std::string& session_id) const {
    auto it = operations_.find(session_id);
    if (it == operations_.end()) {
        return OperationState::OPS_IDLE;
    }
    return it->second.current.state;
}

std::int64_t
OperationTracker::get_last_op_timestamp(const std::string& session_id) const {
    auto it = operations_.find(session_id);
    if (it == operations_.end()) {
        return 0;
    }
    return it->second.last_op_timestamp_ms;
}

// ================================================================
// 清理
// ================================================================

void OperationTracker::clear(const std::string& session_id) {
    operations_.erase(session_id);
}

void OperationTracker::clear_all() {
    operations_.clear();
}

} // namespace dream_machine::signal