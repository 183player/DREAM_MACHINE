// platform/dm_signal/signal_bus.cpp
#include "signal_bus.h"
#include "signal_sink.h"

#include <algorithm>
#include <exception>

namespace dream_machine::signal {

// ================================================================
// 单例访问
//
// Meyers 单例：C++11 保证静态局部变量的初始化线程安全
// （标准 [stmt.dcl]/4：若并发访问，编译器负责保证只初始化一次）。
//
// 生命周期：进程退出时自动析构（static 析构顺序在 main 返回后）。
// ================================================================
SignalBus& SignalBus::instance() {
    static SignalBus bus;
    return bus;
}

// ================================================================
// 订阅：全量（无过滤器）
//
// 添加一个接收所有信号的订阅条目。
// ================================================================
void SignalBus::subscribe(ISignalSink* sink) {
    if (!sink) {
        return;   // 静默忽略：零依赖下无法记日志；上层保证非空
    }

    SubscriberEntry entry;
    entry.sink = sink;
    entry.type_filter = std::nullopt;
    entry.min_level = SignalLevel::SGL_DEBUG;
    subscribers_.push_back(std::move(entry));
}

// ================================================================
// 订阅：按类型
//
// 仅接收指定 type 的信号。
// min_level 默认 SGL_DEBUG —— 该类型的所有级别都接收。
// ================================================================
void SignalBus::subscribe(SignalType type, ISignalSink* sink) {
    if (!sink) {
        return;
    }

    SubscriberEntry entry;
    entry.sink = sink;
    entry.type_filter = type;
    entry.min_level = SignalLevel::SGL_DEBUG;
    subscribers_.push_back(std::move(entry));
}

// ================================================================
// 订阅：按级别
//
// 接收 min_level 及以上级别的所有类型信号。
// ================================================================
void SignalBus::subscribe(SignalLevel min_level, ISignalSink* sink) {
    if (!sink) {
        return;
    }

    SubscriberEntry entry;
    entry.sink = sink;
    entry.type_filter = std::nullopt;
    entry.min_level = min_level;
    subscribers_.push_back(std::move(entry));
}

// ================================================================
// 取消订阅
//
// 移除该 sink 的所有订阅条目（含重复订阅产生的多个条目）。
// 未知 sink 静默忽略（幂等）。
// ================================================================
void SignalBus::unsubscribe(ISignalSink* sink) {
    if (!sink) {
        return;
    }

    subscribers_.erase(
        std::remove_if(
            subscribers_.begin(),
            subscribers_.end(),
            [sink](const SubscriberEntry& e) {
                return e.sink == sink;
            }
        ),
        subscribers_.end()
    );
}

// ================================================================
// 发布（同步分发）
//
// 流程：
//   1. 清空上次的错误
//   2. 快照 subscribers_（防止订阅者在回调中修改订阅列表）
//   3. 遍历快照，对每个匹配的条目调用 on_signal
//   4. 单个订阅者抛异常 → 记录到 last_error_，继续下一订阅者
//
// 匹配规则：
//   - type_filter 有值：要求 payload.type == *type_filter
//   - type_filter 为空：任意类型
//   - 级别：payload.level >= entry.min_level
//
// 快照原因：
//   若订阅者在 on_signal 中调用 subscribe/unsubscribe，
//   直接遍历 subscribers_ 会导致迭代器失效（vector 可能 reallocate 或 erase）。
//   快照后，本次分发基于"发布时刻的订阅者集合"，语义清晰。
// ================================================================
void SignalBus::publish(const SignalPayload& payload) {
    last_error_.clear();

    // 快照：保证遍历期间的迭代器稳定性
    // 注：订阅者数量通常很小（个位数），拷贝开销可忽略
    const std::vector<SubscriberEntry> snapshot = subscribers_;

    for (const auto& entry : snapshot) {
        if (!entry.sink) {
            continue;   // 防御：理论上 subscribe 已过滤空指针
        }

        // ---- 类型过滤 ----
        if (entry.type_filter.has_value() &&
            *entry.type_filter != payload.type) {
            continue;
        }

        // ---- 级别过滤 ----
        // enum class 不支持 operator<，显式转 int 比较
        if (static_cast<int>(payload.level) <
            static_cast<int>(entry.min_level)) {
            continue;
        }

        // ---- 分发（异常隔离） ----
        try {
            entry.sink->on_signal(payload);
        } catch (const std::exception& e) {
            last_error_ = std::string("subscriber threw: ") + e.what();
        } catch (...) {
            last_error_ = "subscriber threw unknown exception";
        }
    }
}

// ================================================================
// 诊断：最近一次发布中的错误
//
// 返回最近一次 publish 中第一个被捕获的异常信息；
// 无错误时返回空字符串。
//
// 用途：dm_signal 零依赖无法记日志；
//       上层（如 Logger 订阅者）可通过此接口轮询并记录。
// ================================================================
std::string SignalBus::last_error() const {
    return last_error_;
}

} // namespace dream_machine::signal