// platform/dm_event/event_loop.h
#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include <vector>
#include <chrono>
#include <cstdint>

namespace dream_machine {
namespace event {

// ================================================================
// 事件类型
//
// 【C6 命名落差 · 诚实标注】
//
// 本模块对外命名与日志使用 "event-driven main loop"，
// 但 READABLE 的实际实现是轮询（PeekNamedPipe + Sleep(10)），
// 并非真正进入 WaitForMultipleObjects。
//
// 当前各事件类型的真实实现：
//   - WAITABLE  : ✅ 真正进入 WaitForMultipleObjects
//   - SIGNAL    : ✅ 真正进入 WaitForMultipleObjects
//   - TIMER     : ⚠ 由 updateTimerEvents() 单独处理（非阻塞轮询）
//   - READABLE  : ⚠ 轮询检测（isHandleReadable + Sleep(10)）
//
// 命名与实现存在落差。修正方向二选一：
//   A. 诚实标注（当前选择）
//      —— 改注释与日志文案，说明 READABLE 为轮询语义
//   B. 改用重叠 I/O（OVERLAPPED + 事件句柄）
//      —— 引入新复杂度
//
// 选择 A 的理由：
//   改重叠 I/O 与 fail-fast 语义无直接关系，且引入新复杂度；
//   诚实标注可消除名实不符，成本最低。
//
// 依据：详见文档4 §9.3、文档5 §6.6、文档13 §C6。
// ================================================================
enum class EventType {
    READABLE,        // 句柄可读（管道有数据）—— 轮询语义，详见上方说明
    TIMER,           // 定时器到期
    SIGNAL_EVENT,    // 手动触发信号
    ERROR_EVENT,     // 错误
    WAITABLE         // 通用句柄等待（进程退出、事件等）
};

// ================================================================
// 事件回调函数类型
// ================================================================
using EventCallback = std::function<void(EventType type, void* user_data)>;

// ================================================================
// 事件注册句柄
// ================================================================
struct EventHandle {
    uint64_t id = 0;
    bool active = false;
};

// ================================================================
// 事件循环
//
// 设计意图：基于 Windows WaitForMultipleObjects 的事件驱动主循环。
//
// 实际实现（C6 诚实标注）：
//   - WAITABLE / SIGNAL 真正进入 WaitForMultipleObjects
//   - TIMER 由 updateTimerEvents() 非阻塞轮询处理
//   - READABLE 由 isHandleReadable() 轮询检测，非事件驱动
//
// 对外命名与日志仍称 "event-driven main loop"，但 READABLE 部分
// 属于名实不符。详见文件头 EventType 注释。
// ================================================================
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // 禁止拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // ============================================================
    // 事件注册
    // ============================================================

    // 注册可读事件
    //
    // 语义：当 handle 有数据可读时触发回调。
    //
    // 实现说明（C6 诚实标注）：
    //   本接口的实际实现是**轮询**——每次 processEvents() 调用
    //   isHandleReadable() 检测 + Sleep(10) 等待，并非事件驱动。
    //   命名保留 registerReadable 是为与 WAITABLE / SIGNAL 风格统一，
    //   但语义上它是"轮询注册"，不是"等待句柄 signaled"。
    //
    // 若未来改为重叠 I/O，本接口语义不变，内部实现改为
    // OVERLAPPED + 事件句柄；届时可移除本说明。
    //
    // @param handle    Windows 句柄（如命名管道）
    // @param callback  回调函数
    // @param user_data 用户数据
    // @return          事件句柄
    EventHandle registerReadable(HANDLE handle, EventCallback callback, void* user_data = nullptr);

    // 注册定时器事件：经过 interval 毫秒后触发
    // @param interval  间隔时间（毫秒）
    // @param callback  回调函数
    // @param user_data 用户数据
    // @param oneshot   是否只触发一次
    // @return          事件句柄
    EventHandle registerTimer(uint64_t interval, EventCallback callback,
                              void* user_data = nullptr, bool oneshot = false);

    // 注册信号事件：手动触发（由外部调用 triggerSignal()）
    // @param callback  回调函数
    // @param user_data 用户数据
    // @return          事件句柄
    EventHandle registerSignal(EventCallback callback, void* user_data = nullptr);

    // ----- 注册通用等待句柄（进程句柄、事件句柄等）-----
    //
    // 语义：一次性等待
    //   当 handle 变为 signaled 状态时，触发回调一次，随后该事件自动失效
    //   （内部 active 置为 false，但对象保留在 items_ 中直至 unregister）。
    //
    // 设计原因：
    //   Windows 进程句柄等 signaled 后不会回退，若事件持续 active，
    //   会在每轮 processEvents 中反复触发回调（实测约 2ms 一次）。
    //   因此语义定义为"触发一次即失效"，避免调用方需要自行防抖。
    //
    // 使用约束：
    //   1. 回调中【不得】调用 unregister() 移除自身或其他 WAITABLE 项；
    //      若需在回调中清理事件，应在回调返回后由外部线程/下一次事件处理。
    //      （原因：processEvents 的 item_map 持有裸指针，回调中 erase 会悬空。）
    //   2. 若需"持续等待"语义（如等待多次事件），当前未提供；
    //      调用方可在回调返回后重新 registerWaitable，或未来新增
    //      registerWaitablePersistent（未实现，按需添加）。
    //
    // @param handle    等待句柄（如进程句柄、事件句柄）
    // @param callback  回调函数
    // @param user_data 用户数据
    // @return          事件句柄
    EventHandle registerWaitable(HANDLE handle, EventCallback callback, void* user_data = nullptr);

    // ============================================================
    // 取消注册
    // ============================================================

    // 取消注册并释放资源
    // 注意：可在事件循环外调用；若在事件循环回调中调用，需满足以下条件：
    //   - 不得移除当前正在执行的 WAITABLE / SIGNAL 事件自身；
    //   - 不得移除同一轮 processEvents 中 item_map 所引用的其他事件。
    bool unregister(EventHandle& handle);

    // ============================================================
    // 运行与控制
    // ============================================================

    void run();
    void stop();
    bool isRunning() const { return running_; }
    bool triggerSignal(uint64_t event_id);

    // ============================================================
    // 静态辅助：检查 handle 是否可读（非阻塞）
    //
    // 实现：PeekNamedPipe 检查可读字节数；不消耗数据。
    //
    // 说明（C6 诚实标注）：
    //   这是**轮询**检测函数，registerReadable 内部通过反复调用本函数
    //   实现"可读事件"。它不是事件驱动——没有句柄 signaled 通知。
    // ============================================================
    static bool isHandleReadable(HANDLE handle);

private:
    // 内部事件项
    struct EventItem {
        enum class Kind { READABLE, TIMER, SIGNAL, WAITABLE } kind;
        HANDLE handle = INVALID_HANDLE_VALUE;   // 句柄（用于 READABLE 和 WAITABLE）
        EventCallback callback;
        void* user_data = nullptr;
        uint64_t id = 0;
        bool active = true;
        // 定时器相关
        uint64_t interval_ms = 0;
        bool oneshot = false;
        std::chrono::steady_clock::time_point next_time;
        // 信号事件
        bool triggered = false;
        HANDLE signal_event = INVALID_HANDLE_VALUE;  // 用于信号触发
    };

    std::vector<std::unique_ptr<EventItem>> items_;
    uint64_t next_id_ = 1;
    bool running_ = false;
    HANDLE stop_event_ = INVALID_HANDLE_VALUE;

    // 内部辅助
    void processEvents(DWORD timeout_ms);
    void updateTimerEvents();
    EventItem* findItem(uint64_t id);
};

} // namespace event
} // namespace dream_machine