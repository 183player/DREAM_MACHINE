// platform/dm_event/event_loop.cpp
#include "event_loop.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

namespace dream_machine::event {

// ================================================================
// 构造函数 / 析构函数
// ================================================================

EventLoop::EventLoop() {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        LOG_ERROR("EventLoop: failed to create stop event");
    }
}

EventLoop::~EventLoop() {
    stop();
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = INVALID_HANDLE_VALUE;
    }
}

// ================================================================
// 辅助：检查句柄是否可读
// ================================================================

bool EventLoop::isHandleReadable(HANDLE handle) {
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD bytes_available = 0;
    if (PeekNamedPipe(handle, nullptr, 0, nullptr, &bytes_available, nullptr)) {
        return bytes_available > 0;
    }
    DWORD err = GetLastError();
    return err != ERROR_BROKEN_PIPE && err != ERROR_NO_DATA;
}

// ================================================================
// 注册事件
// ================================================================

EventHandle EventLoop::registerReadable(HANDLE handle, EventCallback callback, void* user_data) {
    EventHandle result{0, false};

    if (!handle || handle == INVALID_HANDLE_VALUE) {
        LOG_ERROR("EventLoop: invalid handle for readable event");
        return result;
    }

    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        LOG_ERROR("EventLoop: failed to create event for readable");
        return result;
    }

    auto item = std::make_unique<EventItem>();
    item->kind = EventItem::Kind::READABLE;
    item->handle = handle;
    item->callback = std::move(callback);
    item->user_data = user_data;
    item->id = next_id_++;
    item->active = true;
    item->signal_event = hEvent;

    uint64_t id = item->id;
    items_.push_back(std::move(item));

    result.id = id;
    result.active = true;
    return result;
}

EventHandle EventLoop::registerTimer(uint64_t interval, EventCallback callback,
                                     void* user_data, bool oneshot) {
    EventHandle result{0, false};

    if (interval == 0) {
        LOG_ERROR("EventLoop: invalid timer interval");
        return result;
    }

    auto item = std::make_unique<EventItem>();
    item->kind = EventItem::Kind::TIMER;
    item->callback = std::move(callback);
    item->user_data = user_data;
    item->id = next_id_++;
    item->active = true;
    item->interval_ms = interval;
    item->oneshot = oneshot;
    item->next_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval);

    uint64_t id = item->id;
    items_.push_back(std::move(item));

    result.id = id;
    result.active = true;
    return result;
}

EventHandle EventLoop::registerSignal(EventCallback callback, void* user_data) {
    EventHandle result{0, false};

    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        LOG_ERROR("EventLoop: failed to create signal event");
        return result;
    }

    auto item = std::make_unique<EventItem>();
    item->kind = EventItem::Kind::SIGNAL;
    item->callback = std::move(callback);
    item->user_data = user_data;
    item->id = next_id_++;
    item->active = true;
    item->triggered = false;
    item->signal_event = hEvent;

    uint64_t id = item->id;
    items_.push_back(std::move(item));

    result.id = id;
    result.active = true;
    return result;
}

// ================================================================
// 新增：注册通用等待句柄
// ================================================================

EventHandle EventLoop::registerWaitable(HANDLE handle, EventCallback callback, void* user_data) {
    EventHandle result{0, false};

    if (!handle || handle == INVALID_HANDLE_VALUE) {
        LOG_ERROR("EventLoop: invalid handle for waitable event");
        return result;
    }

    auto item = std::make_unique<EventItem>();
    item->kind = EventItem::Kind::WAITABLE;
    item->handle = handle;          // 直接存储句柄
    item->callback = std::move(callback);
    item->user_data = user_data;
    item->id = next_id_++;
    item->active = true;
    // WAITABLE 不需要 signal_event，直接用 handle 本身等待

    uint64_t id = item->id;
    items_.push_back(std::move(item));

    result.id = id;
    result.active = true;
    return result;
}

// ================================================================
// 取消注册
// ================================================================

bool EventLoop::unregister(EventHandle& handle) {
    if (!handle.active || handle.id == 0) {
        return false;
    }

    auto it = std::find_if(items_.begin(), items_.end(),
        [&](const std::unique_ptr<EventItem>& item) {
            return item && item->id == handle.id;
        });

    if (it == items_.end()) {
        return false;
    }

    (*it)->active = false;

    if ((*it)->signal_event && (*it)->signal_event != INVALID_HANDLE_VALUE) {
        CloseHandle((*it)->signal_event);
        (*it)->signal_event = INVALID_HANDLE_VALUE;
    }

    items_.erase(it);

    handle.active = false;
    handle.id = 0;
    return true;
}

// ================================================================
// 内部查找
// ================================================================

EventLoop::EventItem* EventLoop::findItem(uint64_t id) {
    for (auto& item : items_) {
        if (item && item->id == id && item->active) {
            return item.get();
        }
    }
    return nullptr;
}

// ================================================================
// 更新定时器事件
// ================================================================

void EventLoop::updateTimerEvents() {
    auto now = std::chrono::steady_clock::now();

    std::vector<uint64_t> expired_ids;
    for (auto& item : items_) {
        if (!item || !item->active || item->kind != EventItem::Kind::TIMER) {
            continue;
        }

        if (now >= item->next_time) {
            expired_ids.push_back(item->id);
            item->next_time = now + std::chrono::milliseconds(item->interval_ms);
        }
    }

    for (uint64_t id : expired_ids) {
        auto item = findItem(id);
        if (item && item->active && item->callback) {
            item->callback(EventType::TIMER, item->user_data);
            if (item->oneshot) {
                item->active = false;
            }
        }
    }
}

// ================================================================
// 处理事件
// ================================================================

void EventLoop::processEvents(DWORD timeout_ms) {
    std::vector<HANDLE> wait_handles;
    std::vector<EventItem*> item_map;      // 与 wait_handles 对应（跳过 stop_event）

    wait_handles.push_back(stop_event_);

    // ----- 收集需要等待的句柄 -----
    for (auto& item : items_) {
        if (!item || !item->active) {
            continue;
        }

        if (item->kind == EventItem::Kind::READABLE) {
            // READABLE: 使用 Peek 直接检测，不进入等待循环
            if (isHandleReadable(item->handle)) {
                if (item->callback) {
                    item->callback(EventType::READABLE, item->user_data);
                }
            }
        } else if (item->kind == EventItem::Kind::WAITABLE) {
            // WAITABLE: 将句柄加入等待列表
            if (item->handle && item->handle != INVALID_HANDLE_VALUE) {
                wait_handles.push_back(item->handle);
                item_map.push_back(item.get());
            }
        } else if (item->kind == EventItem::Kind::SIGNAL) {
            // SIGNAL: 使用 signal_event 等待
            if (item->signal_event && item->signal_event != INVALID_HANDLE_VALUE) {
                wait_handles.push_back(item->signal_event);
                item_map.push_back(item.get());
            }
        }
        // TIMER 由 updateTimerEvents 单独处理
    }

    // 更新定时器（不阻塞）
    updateTimerEvents();

    // ----- 如果没有需要等待的句柄，短暂休眠 -----
    if (wait_handles.size() <= 1) {
        Sleep(10);
        return;
    }

    // ----- 等待多个句柄 -----
    DWORD result = WaitForMultipleObjects(
        static_cast<DWORD>(wait_handles.size()),
        wait_handles.data(),
        FALSE,
        timeout_ms
    );

    if (result == WAIT_OBJECT_0) {
        // 停止事件被触发
        return;
    }

    if (result >= WAIT_OBJECT_0 + 1 && result < WAIT_OBJECT_0 + wait_handles.size()) {
        // 某个句柄被触发
        size_t index = result - WAIT_OBJECT_0 - 1;  // 跳过 stop_event
        if (index < item_map.size()) {
            auto* item = item_map[index];
            if (item && item->active && item->callback) {
                // 判断是 WAITABLE 还是 SIGNAL
                if (item->kind == EventItem::Kind::WAITABLE) {
                    item->callback(EventType::WAITABLE, item->user_data);
                } else if (item->kind == EventItem::Kind::SIGNAL) {
                    item->callback(EventType::SIGNAL_EVENT, item->user_data);
                }
            }
        }
    } else if (result == WAIT_TIMEOUT) {
        // 超时，无事件
    } else if (result == WAIT_FAILED) {
        DWORD err = GetLastError();
        if (err != ERROR_INVALID_HANDLE) {
            LOG_WARN("WaitForMultipleObjects failed: " + std::to_string(err));
        }
    }
}

// ================================================================
// 运行 / 停止
// ================================================================

void EventLoop::run() {
    if (running_) {
        LOG_WARN("EventLoop already running");
        return;
    }

    running_ = true;
    LOG_INFO("EventLoop started");

    while (running_) {
        processEvents(100);
    }

    LOG_INFO("EventLoop stopped");
}

void EventLoop::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (stop_event_) {
        SetEvent(stop_event_);
    }
    LOG_INFO("EventLoop stop requested");
}

// ================================================================
// 触发信号
// ================================================================

bool EventLoop::triggerSignal(uint64_t event_id) {
    auto item = findItem(event_id);
    if (!item || !item->active || item->kind != EventItem::Kind::SIGNAL) {
        LOG_WARN("EventLoop: signal event not found or inactive");
        return false;
    }

    if (item->signal_event && item->signal_event != INVALID_HANDLE_VALUE) {
        SetEvent(item->signal_event);
        return true;
    }

    return false;
}

} // namespace dream_machine::event