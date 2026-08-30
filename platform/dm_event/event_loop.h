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
// ================================================================
enum class EventType {
    READABLE,        // 句柄可读（管道有数据）
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
// 事件循环（基于 Windows WaitForMultipleObjects）
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

    // 注册可读事件：当 handle 有数据可读时触发回调
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

    // 注册信号事件：手动触发（由外部调用 trigger()）
    EventHandle registerSignal(EventCallback callback, void* user_data = nullptr);

    // ----- 新增：注册通用等待句柄（进程句柄、事件句柄等）-----
    // 当 handle 变为有信号状态时触发回调
    // @param handle    等待句柄（如进程句柄、事件句柄）
    // @param callback  回调函数
    // @param user_data 用户数据
    // @return          事件句柄
    EventHandle registerWaitable(HANDLE handle, EventCallback callback, void* user_data = nullptr);

    // ============================================================
    // 取消注册
    // ============================================================

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