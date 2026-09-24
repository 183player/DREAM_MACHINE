// platform/dm_signal/signal_bus.h
#pragma once

// ================================================================
// dm_signal —— 进程内信号总线
//
// SignalBus 是 dm_signal 的核心：进程内的发布-订阅总线。
//
// 使用方式：
//   SignalBus::instance().subscribe(&my_sink);       // 订阅全部
//   SignalBus::instance().subscribe(type, &my_sink); // 订阅指定类型
//   SignalBus::instance().subscribe(level, &my_sink);// 订阅指定级别及以上
//
//   SignalPayload p;
//   p.type = SignalType::SGT_STARTUP;
//   p.description = "process started";
//   SignalBus::instance().publish(p);                // 同步分发
//
// 设计原则：
//   - 零依赖：仅 include signal_types.h + 标准库
//   - 同步分发：publish 时立即调用所有匹配的订阅者
//   - 单线程：所有操作在同一线程；未来若引入线程再加锁
//   - 异常隔离：单个订阅者抛异常不影响其他订阅者
//   - 前向声明：ISignalSink 不需完整定义
//
// 命名注意：
//   - 方法名用 publish 而非 emit：
//     Qt 定义了 `#define emit Q_EMIT`，若方法名为 emit 会被宏替换，
//     导致 dm_logger 等 include Qt 的编译单元无法编译。
//
// 生命周期：
//   - SignalBus 是 Meyers 单例，进程退出时自动析构
//   - subscribe 不持有订阅者所有权；订阅者须在析构前 unsubscribe
//   - 建议：在各自析构函数中 unsubscribe，避免悬垂指针
// ================================================================

#include "signal_types.h"

#include <optional>
#include <string>
#include <vector>

namespace dream_machine::signal {

// 前向声明（头文件只需指针，不需完整定义）
class ISignalSink;

// ================================================================
// SignalBus：进程内信号总线（单例）
//
// 线程模型：单线程
//   - subscribe / unsubscribe / publish 应在同一线程调用
//   - 当前所有进程均为单线程事件循环，符合此假设
//   - 未来若引入多线程，需在方法内加锁
// ================================================================
class SignalBus {
public:
    // ============================================================
    // 单例访问
    // ============================================================
    static SignalBus& instance();

    // 禁止拷贝 / 移动
    SignalBus(const SignalBus&) = delete;
    SignalBus& operator=(const SignalBus&) = delete;
    SignalBus(SignalBus&&) = delete;
    SignalBus& operator=(SignalBus&&) = delete;

    // ============================================================
    // 订阅
    //
    // 三种订阅方式，每次调用添加一个订阅条目：
    //   subscribe(sink)           - 接收所有信号（无过滤器）
    //   subscribe(type, sink)     - 仅接收指定 type 的信号
    //   subscribe(min_level, sink)- 接收 min_level 及以上级别的信号
    //
    // 同一 sink 可以多次 subscribe 以添加多个过滤器；
    // unsubscribe(sink) 会移除该 sink 的所有条目。
    //
    // sink 为 nullptr 时静默忽略（不添加条目）。
    // ============================================================
    void subscribe(ISignalSink* sink);
    void subscribe(SignalType type, ISignalSink* sink);
    void subscribe(SignalLevel min_level, ISignalSink* sink);

    // ============================================================
    // 取消订阅
    //
    // 移除该 sink 的所有订阅条目。
    // sink 为 nullptr 时静默忽略。
    // 未知 sink 静默忽略（幂等）。
    // ============================================================
    void unsubscribe(ISignalSink* sink);

    // ============================================================
    // 发布（同步分发）
    //
    // 立即按订阅顺序调用所有匹配的订阅者：
    //   - 对每个订阅条目，检查 type_filter 和 min_level
    //   - 若匹配，在 try/catch 中调用 sink->on_signal(payload)
    //   - 抛异常的订阅者被跳过（记录到 last_error_），其他订阅者继续
    //
    // 注意：
    //   - 订阅者应快速返回（同步调用会阻塞 publish）
    //   - 耗时操作应由订阅者自行入队异步处理
    // ============================================================
    void publish(const SignalPayload& payload);

    // ============================================================
    // 诊断
    //
    // 返回最近一次 publish 中订阅者抛出的异常信息。
    // 无错误时返回空字符串。
    //
    // 用途：零依赖约束下，dm_signal 无法记日志；
    //       上层可通过此接口轮询错误并记录。
    // ============================================================
    [[nodiscard]] std::string last_error() const;

private:
    // ============================================================
    // 单例：构造 / 析构私有
    // ============================================================
    SignalBus() = default;
    ~SignalBus() = default;

    // ============================================================
    // 内部：订阅条目
    //
    // 每个条目表示"某 sink 对某类信号的兴趣"。
    //   - type_filter 有值：仅接收该类型的信号
    //   - type_filter 为空：接收所有类型
    //   - min_level：仅接收该级别及以上的信号
    //
    // 匹配规则：type 匹配 && level >= min_level
    // ============================================================
    struct SubscriberEntry {
        ISignalSink* sink = nullptr;
        std::optional<SignalType> type_filter;
        SignalLevel min_level = SignalLevel::SGL_DEBUG;
    };

    // ============================================================
    // 内部状态
    // ============================================================
    std::vector<SubscriberEntry> subscribers_;
    std::string last_error_;
};

} // namespace dream_machine::signal