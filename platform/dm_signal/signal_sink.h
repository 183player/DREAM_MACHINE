// platform/dm_signal/signal_sink.h
#pragma once

// ================================================================
// dm_signal —— 信号订阅者接口
//
// ISignalSink 是 dm_signal 的订阅者抽象基类。任何想要接收信号的
// 组件（Logger / OperationTracker / MessageBridge / ...）都实现此接口。
//
// 设计原则：
//   - 零依赖：仅 include signal_types.h + 标准库
//   - 最小接口：只有一个纯虚函数 on_signal
//   - 虚拟析构：支持通过基类指针 delete
//   - 异常约定：派生类可以抛异常，由 SignalBus 捕获并隔离
// ================================================================

#include "signal_types.h"

namespace dream_machine::signal {

    // ================================================================
    // ISignalSink：信号接收接口
    //
    // 使用方式：
    //   class MySink : public ISignalSink {
    //   public:
    //       void on_signal(const SignalPayload& payload) override {
    //           // 处理信号
    //       }
    //   };
    //
    //   MySink sink;
    //   SignalBus::instance().subscribe(&sink);
    //
    // 生命周期：
    //   - 订阅者由使用者管理（栈对象 / 成员变量 / unique_ptr）
    //   - SignalBus 不持有订阅者的所有权
    //   - 订阅者析构前必须 unsubscribe（或由派生类在析构中调用）
    //
    // 线程安全：
    //   - 当前单线程模型；on_signal 在 emit 线程中同步调用
    //   - 若未来引入多线程，需在 SignalBus 中加锁
    // ================================================================
    class ISignalSink {
    public:
        virtual ~ISignalSink() = default;

        // 接收信号
        //
        // 参数：
        //   payload - 信号载荷（只读；包含结构化字段 + 人类可读字段）
        //
        // 约定：
        //   1. 实现应尽快返回（避免阻塞 emit 方）
        //   2. 耗时操作（如磁盘 I/O、网络）应在内部入队后异步处理
        //   3. 异常会被 SignalBus 捕获并隔离，不影响其他订阅者
        //      但强烈建议实现内部消化异常，避免依赖 SignalBus 的兜底
        virtual void on_signal(const SignalPayload& payload) = 0;
    };

} // namespace dream_machine::signal