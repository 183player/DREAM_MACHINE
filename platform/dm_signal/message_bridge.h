// platform/dm_signal/message_bridge.h
#pragma once

// ================================================================
// dm_signal —— 跨进程消息桥接
//
// MessageBridge 是 dm_signal 的订阅者之一：
//   - 订阅信号
//   - 按 filter 匹配目标
//   - 调用目标 sender 发送（sender 内部自行序列化 + 写管道）
//
// 使用方式：
//   // 进程启动时装配（推荐顺序见下方"装配顺序"）
//   SignalBus::instance().subscribe(&Logger::instance());
//   SignalBus::instance().subscribe(&OperationTracker::instance());
//   SignalBus::instance().subscribe(&MessageBridge::instance());
//
//   // 注册发送目标
//   MessageBridge::instance().add_sender(
//       "monitor",
//       [&monitor_pipe](const SignalPayload& p) {
//           // sender 内部：序列化 + 写管道
//           OpContextMessage msg;
//           msg.session_id = p.session_id;
//           // ... 填充
//           return monitor_pipe.writeLine(serializeOpContext(msg))
//                  == PipeResult::PIPE_OK;
//       },
//       [](const SignalPayload& p) {
//           return p.type == SignalType::SGT_OP_CONTEXT;
//       });
//
// 多目标注册示例（launcher 同时向 monitor / gui 发送错误通知）：
//   // 目标 1：monitor
//   MessageBridge::instance().add_sender(
//       "monitor",
//       [&monitor_pipe](const SignalPayload& p) { /* ... */ },
//       [](const SignalPayload& p) {
//           return p.type == SignalType::SGT_ERROR_NOTIFY;
//       });
//
//   // 目标 2：gui（同一信号类型，不同目标）
//   MessageBridge::instance().add_sender(
//       "gui",
//       [&gui_pipe](const SignalPayload& p) { /* ... */ },
//       [](const SignalPayload& p) {
//           return p.type == SignalType::SGT_ERROR_NOTIFY;
//       });
//
// 装配顺序（推荐）：
//   Logger → OperationTracker → MessageBridge
//   - Logger 最先：任何信号都先被记录
//   - OperationTracker 其次：本地状态更新
//   - MessageBridge 最后：跨进程发送（可能失败，不影响前两者）
//
// 设计原则：
//   - 单例 + 多 Sender：一个进程可向多个目标发送
//   - Sender 由调用方注入：dm_signal 不依赖 dm_pipe，零依赖成立
//   - Sender 接收 SignalPayload（非 string）：序列化由调用方决定格式
//   - Filter 可为空：默认接收所有信号
//
// 异常隔离：
//   - Filter 抛异常 → 视为"未通过"（跳过该 sender），记录 last_error_
//   - Sender 抛异常 → 记录 last_error_，继续下一 sender
//   - Sender 返回 false → 记录 last_error_，继续下一 sender
//
// Sender 生命周期约定（重要）：
//   - Sender 通常捕获外部对象引用（如管道）
//   - 这些被捕获的对象必须活到 MessageBridge 析构（进程退出）
//   - 若被捕获对象提前析构，后续 on_signal 会导致悬垂引用 → UB
//
// 线程模型：
//   - 当前单线程：on_signal 在 emit 线程中同步调用
//   - 未来若引入多线程：需在操作 senders_ 时加锁
//
// 生命周期：
//   - 单例；进程退出时自动析构
//   - 析构时自动 unsubscribe（与 Logger / OperationTracker 一致）
// ================================================================

#include "signal_sink.h"
#include "signal_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dream_machine::signal {

// ================================================================
// MessageBridge：跨进程消息桥接（单例）
// ================================================================
class MessageBridge : public ISignalSink {
public:
    // ============================================================
    // 类型别名
    //
    // Sender  - 发送函数。接收完整 SignalPayload；内部负责序列化 + 发送。
    //          返回 true 表示发送成功；false 表示失败（记入 last_error_）。
    //          由调用方注入，内部通常调用管道 writeLine。
    //
    // Filter  - 过滤函数。返回 true 表示该信号应发给此 sender。
    //          为空时表示"全部通过"。
    // ============================================================
    using Sender = std::function<bool(const SignalPayload& payload)>;
    using Filter = std::function<bool(const SignalPayload& payload)>;

    // ============================================================
    // 单例访问
    // ============================================================
    static MessageBridge& instance();

    // 禁止拷贝 / 移动
    MessageBridge(const MessageBridge&) = delete;
    MessageBridge& operator=(const MessageBridge&) = delete;
    MessageBridge(MessageBridge&&) = delete;
    MessageBridge& operator=(MessageBridge&&) = delete;

    // ============================================================
    // Sender 管理
    // ============================================================

    // 注册一个发送目标
    //
    // 参数：
    //   target_name - 目标名（日志 / 诊断用，如 "monitor" / "gui"）
    //   sender      - 发送函数（为空则静默忽略，不注册）
    //   filter      - 过滤函数（为空则接收所有信号）
    //
    // 返回：
    //   注册成功返回 sender_id（>= 1）；
    //   sender 为空返回 0。
    //
    // 同一个 target_name 可多次注册——每次返回不同 sender_id。
    [[nodiscard]] std::uint64_t add_sender(const std::string& target_name,
                                           Sender sender,
                                           Filter filter = nullptr);

    // 移除指定 sender
    // 未知 id 静默忽略（幂等）
    void remove_sender(std::uint64_t sender_id);

    // 移除所有 sender
    void remove_all_senders();

    // ============================================================
    // 查询
    // ============================================================

    // 当前注册的 sender 数量（诊断用）
    [[nodiscard]] std::size_t sender_count() const;

    // 最近一次 on_signal 中的错误信息
    // 无错误时返回空字符串
    //
    // 语义：多个 sender 失败时只保留"最近一次"（与 SignalBus::last_error 一致）
    [[nodiscard]] std::string last_error() const;

    // ============================================================
    // ISignalSink 接口
    //
    // 处理流程：
    //   1. 清空 last_error_
    //   2. 若 senders_ 为空 → 返回
    //   3. 快照 senders_（防止 sender 内部修改订阅列表）
    //   4. 遍历快照：
    //        a. 若 filter 存在：
    //             try 调用 filter(payload)
    //             - 抛异常 → 视为"未通过"，记录 last_error_，跳过
    //             - 返回 false → 跳过
    //        b. try 调用 sender(payload)
    //             - 抛异常 → 记录 last_error_，继续下一 sender
    //             - 返回 false → 记录 last_error_，继续下一 sender
    //   5. 全部遍历完毕
    //
    // 快照原因：
    //   若 sender 内部调用 add_sender / remove_sender，
    //   直接遍历 senders_ 会导致迭代器失效。
    // ============================================================
    void on_signal(const SignalPayload& payload) override;

private:
    // ============================================================
    // 单例：构造 / 析构私有
    // ============================================================
    MessageBridge();
    ~MessageBridge() override;

    // ============================================================
    // 内部：发送目标条目
    // ============================================================
    struct SenderEntry {
        std::uint64_t id = 0;
        std::string   target_name;
        Sender        sender;
        Filter        filter;
    };

    // ============================================================
    // 内部状态
    //
    // senders_        - 已注册的发送目标
    // next_sender_id_ - 递增 ID 生成器（从 1 开始；0 保留为"无效 id"）
    // last_error_     - 最近一次 on_signal 的错误
    //
    // 单线程假设；未来若引入多线程需加锁。
    // ============================================================
    std::vector<SenderEntry> senders_;
    std::uint64_t next_sender_id_ = 1;
    std::string   last_error_;
};

} // namespace dream_machine::signal