// platform/dm_signal/operation_tracker.h
#pragma once

// ================================================================
// dm_signal —— 操作状态追踪器
//
// OperationTracker 是 dm_signal 的订阅者之一：
//   - 订阅 SGT_OP_CONTEXT 信号
//   - 按 session_id 索引维护每个会话的"当前操作 + 上一个操作 + 时间戳"
//   - 供上层查询（如退出分类判定时查"崩溃瞬间是否在执行操作"）
//
// 为什么保留 previous 和 timestamp：
//   1. previous —— 区分"操作刚完成"与"完全空闲"
//      仅存 current 时，FINISHING 后若收到 IDLE 会丢失"刚完成"信息；
//      previous 保留上一次操作，供诊断与退出分类使用。
//
//   2. last_op_timestamp_ms —— 支持"N 秒窗口"判定
//      退出分类规则："退出前 N 秒内上报 PREPARING / RUNNING → 崩溃"
//      需要时间戳才能实现；当前只存 OperationInfo 无法满足。
//
// 使用方式：
//   // 进程启动时装配
//   SignalBus::instance().subscribe(&OperationTracker::instance());
//
//   // 查询
//   auto record = OperationTracker::instance().get_record(session_id);
//   if (record.has_value()) {
//       // record->current / record->previous / record->last_op_timestamp_ms
//   }
//
// 装配场景：
//   - executor     : ✅ 需要（服务多会话）
//   - core_engine  : ✅ 需要（单会话，统一装配便于一致性）
//   - launcher / monitor / gui : ❌ 不需要
//
// 线程模型：
//   - 当前单线程：on_signal 在 emit 线程中同步调用
//   - 未来若引入多线程：需在操作 operations_ 时加锁
// ================================================================

#include "signal_sink.h"
#include "signal_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace dream_machine::signal {

// ================================================================
// OperationRecord：单个会话的操作记录
//
// 字段：
//   current              - 当前操作（最后一次 OP_CONTEXT 的内容）
//   previous             - 上一个操作（用于区分"刚完成"vs"空闲"）
//   last_op_timestamp_ms - 最后一次 OP_CONTEXT 信号的毫秒时间戳（0 表示从未收到）
//
// 初始值：
//   current / previous 均为默认构造的 OperationInfo（OPT_CUSTOM / OPS_IDLE / ""）
//   表示"从未有过操作"
// ================================================================
struct OperationRecord {
    OperationInfo current;
    OperationInfo previous;
    std::int64_t  last_op_timestamp_ms = 0;
};

// ================================================================
// OperationTracker：按会话索引的操作状态追踪器（单例）
// ================================================================
class OperationTracker : public ISignalSink {
public:
    // ============================================================
    // 单例访问
    // ============================================================
    static OperationTracker& instance();

    // 禁止拷贝 / 移动
    OperationTracker(const OperationTracker&) = delete;
    OperationTracker& operator=(const OperationTracker&) = delete;
    OperationTracker(OperationTracker&&) = delete;
    OperationTracker& operator=(OperationTracker&&) = delete;

    // ============================================================
    // ISignalSink 接口
    //
    // 只处理 SGT_OP_CONTEXT 类型的信号；其他类型静默忽略。
    //
    // 处理逻辑：
    //   1. 若 payload.type != SGT_OP_CONTEXT → 忽略
    //   2. 若 payload.session_id 为空 → 忽略
    //   3. 若 payload.operation 无值 → 忽略
    //   4. 更新 record：
    //        record.previous = record.current
    //        record.current  = *payload.operation
    //        record.last_op_timestamp_ms = payload.timestamp_ms
    // ============================================================
    void on_signal(const SignalPayload& payload) override;

    // ============================================================
    // 查询接口
    // ============================================================

    // 获取指定会话的完整操作记录
    // 无记录时返回 std::nullopt
    [[nodiscard]] std::optional<OperationRecord>
    get_record(const std::string& session_id) const;

    // 获取指定会话的当前操作信息
    // 无记录时返回 std::nullopt
    [[nodiscard]] std::optional<OperationInfo>
    get_current(const std::string& session_id) const;

    // 获取指定会话的上一个操作信息
    // 无记录时返回 std::nullopt
    [[nodiscard]] std::optional<OperationInfo>
    get_previous(const std::string& session_id) const;

    // 获取指定会话的当前操作状态
    // 无记录时返回 OPS_IDLE（简化查询接口）
    [[nodiscard]] OperationState
    get_state(const std::string& session_id) const;

    // 获取指定会话最后一次 OP_CONTEXT 的时间戳（毫秒）
    // 无记录时返回 0（0 表示"从未收到"）
    [[nodiscard]] std::int64_t
    get_last_op_timestamp(const std::string& session_id) const;

    // ============================================================
    // 清理
    // ============================================================

    // 移除指定会话的操作记录（会话结束时调用）
    // 未知 session_id 静默忽略（幂等）
    void clear(const std::string& session_id);

    // 移除所有会话的操作记录
    void clear_all();

private:
    // ============================================================
    // 单例：构造 / 析构私有
    // ============================================================
    OperationTracker();
    ~OperationTracker() override;

    // ============================================================
    // 内部状态
    //
    // operations_：session_id → OperationRecord
    //   - 保留 current + previous + timestamp
    //   - 保留 1 个历史（previous），非完整历史列表
    //
    // 单线程假设；未来若引入多线程需加锁。
    // ============================================================
    std::unordered_map<std::string, OperationRecord> operations_;
};

} // namespace dream_machine::signal