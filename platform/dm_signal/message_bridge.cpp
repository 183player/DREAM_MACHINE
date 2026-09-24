// platform/dm_signal/message_bridge.cpp
#include "message_bridge.h"
#include "signal_bus.h"

#include <algorithm>
#include <exception>

namespace dream_machine::signal {

// ================================================================
// 单例访问
//
// Meyers 单例：C++11 保证静态局部变量初始化线程安全。
// 生命周期：进程退出时自动析构。
// ================================================================
MessageBridge& MessageBridge::instance() {
    static MessageBridge bridge;
    return bridge;
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
//             再调用 MessageBridge::instance()（构造自身）
//   - C++ 保证：Meyers 单例按构造的相反顺序析构
//   - 因此：MessageBridge 先析构（SignalBus 仍存活）→ 此处安全
//
// 注：Sender 捕获的外部对象（如管道）必须活到 MessageBridge 析构。
//     各进程 main() 中 Sender 通常捕获 main() 局部对象，
//     MessageBridge 是静态对象——析构顺序上 MessageBridge 晚于局部对象。
//     **这意味着 Sender 捕获局部引用会有悬垂风险**。
//     修正方式：各进程将管道提升为 static / 全局，或在 main() 退出前
//     显式调用 remove_all_senders()。
//     相关约束见 main.cpp 装配代码（第 22-26 轮）。
// ================================================================
MessageBridge::MessageBridge() = default;

MessageBridge::~MessageBridge() {
    SignalBus::instance().unsubscribe(this);
}

// ================================================================
// Sender 管理
// ================================================================

std::uint64_t MessageBridge::add_sender(const std::string& target_name,
                                        Sender sender,
                                        Filter filter) {
    if (!sender) {
        return 0;   // 空 sender 静默忽略
    }

    SenderEntry entry;
    entry.id = next_sender_id_++;
    entry.target_name = target_name;
    entry.sender = std::move(sender);
    entry.filter = std::move(filter);
    senders_.push_back(std::move(entry));

    return senders_.back().id;
}

void MessageBridge::remove_sender(std::uint64_t sender_id) {
    if (sender_id == 0) {
        return;   // 0 是"无效 id"
    }

    senders_.erase(
        std::remove_if(
            senders_.begin(),
            senders_.end(),
            [sender_id](const SenderEntry& e) {
                return e.id == sender_id;
            }
        ),
        senders_.end()
    );
}

void MessageBridge::remove_all_senders() {
    senders_.clear();
}

// ================================================================
// 查询
// ================================================================

std::size_t MessageBridge::sender_count() const {
    return senders_.size();
}

std::string MessageBridge::last_error() const {
    return last_error_;
}

// ================================================================
// ISignalSink 接口：on_signal
//
// 处理流程：
//   1. 清空 last_error_
//   2. 无 sender → 直接返回
//   3. 快照 senders_（防止 sender 内部修改订阅列表）
//   4. 遍历快照：
//        a. Filter 存在时调用（异常隔离）：
//             - 抛异常 → 视为"未通过"，记录 last_error_，跳过
//             - 返回 false → 跳过
//        b. 调用 Sender（异常隔离）：
//             - 抛异常 → 记录 last_error_，继续下一 sender
//             - 返回 false → 记录 last_error_，继续下一 sender
//
// 快照原因：
//   Sender 可能调用 add_sender / remove_sender，
//   直接遍历 senders_ 会导致迭代器失效（vector 可能 reallocate 或 erase）。
//   快照后，本次分发基于"发布时刻的 sender 集合"，语义清晰。
//
// last_error_ 语义：
//   - 多个错误只保留"最近一次"（覆盖式，不追加）
//   - 错误消息格式见下方各分支
// ================================================================
void MessageBridge::on_signal(const SignalPayload& payload) {
    last_error_.clear();

    if (senders_.empty()) {
        return;
    }

    // 快照：保证遍历期间的迭代器稳定性
    // 注：sender 数量通常很小（个位数），拷贝开销可忽略
    const std::vector<SenderEntry> snapshot = senders_;

    for (const auto& entry : snapshot) {
        // ---- Filter 过滤（异常隔离） ----
        if (entry.filter) {
            bool passed = false;
            try {
                passed = entry.filter(payload);
            } catch (const std::exception& e) {
                last_error_ = "filter (" + entry.target_name +
                              ") threw: " + e.what();
                continue;   // 视为未通过，跳过该 sender
            } catch (...) {
                last_error_ = "filter (" + entry.target_name +
                              ") threw unknown exception";
                continue;
            }

            if (!passed) {
                continue;   // Filter 明确返回 false
            }
        }

        // ---- Sender 发送（异常隔离） ----
        bool sent = false;
        try {
            sent = entry.sender(payload);
        } catch (const std::exception& e) {
            last_error_ = "sender (" + entry.target_name +
                          ") threw: " + e.what();
            continue;
        } catch (...) {
            last_error_ = "sender (" + entry.target_name +
                          ") threw unknown exception";
            continue;
        }

        if (!sent) {
            last_error_ = "sender (" + entry.target_name +
                          ") returned false";
            // 继续下一 sender（不中断）
        }
    }
}

} // namespace dream_machine::signal