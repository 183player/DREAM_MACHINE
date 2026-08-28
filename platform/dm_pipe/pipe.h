// platform/dm_pipe/pipe.h
#pragma once

#include <windows.h>
#include <string>
#include <cstdint>

namespace dream_machine {

enum class PipeResult {
    OK,
    TIMEOUT,
    BROKEN,
    CLOSED,
    WOULD_BLOCK,
    NOT_CONNECTED,
    INVALID_PARAM,
    SYSTEM_ERROR,
    BUSY,
};

class NamedPipe {
public:
    NamedPipe() = default;
    ~NamedPipe();

    // 禁止拷贝
    NamedPipe(const NamedPipe&) = delete;
    NamedPipe& operator=(const NamedPipe&) = delete;

    // 移动语义
    NamedPipe(NamedPipe&& other) noexcept;
    NamedPipe& operator=(NamedPipe&& other) noexcept;

    // ============================================================
    // 创建/连接（原有方法不变）
    // ============================================================

    bool createServer(const std::wstring& pipe_name, DWORD max_instances = 1);
    PipeResult waitForClient(DWORD timeout_ms = INFINITE);
    bool connect(const std::wstring& pipe_name, DWORD timeout_ms = 5000);

    // ============================================================
    // 新增：接管外部传入句柄（方式一：命令行传递）
    // ============================================================

    // 将外部传入的句柄值包装为 NamedPipe 对象
    // handle_value: 从命令行解析的句柄值（uintptr_t）
    // 调用后本对象接管该句柄，但 owns_handle_ = false（析构时不关闭）
    static NamedPipe adopt(uintptr_t handle_value);

    // ============================================================
    // 读写操作（原有方法不变）
    // ============================================================

    PipeResult readLine(std::string& out_line, DWORD timeout_ms = 3000);
    PipeResult writeLine(const std::string& line, DWORD timeout_ms = 100);
    PipeResult peekAvailable(DWORD& out_bytes) const;

    // ============================================================
    // 控制操作（原有方法不变）
    // ============================================================

    bool cancelPendingIO();
    void close();
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] HANDLE getHandle() const { return pipe_handle_; }

    // 释放句柄所有权（调用后本对象不再持有句柄）
    // 用于：将句柄传递给子进程前，从父进程释放所有权
    [[nodiscard]] HANDLE release();

    // 检查本对象是否拥有句柄的所有权（控制析构行为）
    [[nodiscard]] bool ownsHandle() const { return owns_handle_; }

private:
    // 私有构造：用于 adopt() 内部创建
    explicit NamedPipe(HANDLE handle, bool owns_handle);

    HANDLE pipe_handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_ = {};
    bool io_pending_ = false;
    bool is_server_ = false;
    bool owns_handle_ = true;      // 析构时是否调用 CloseHandle
    std::string read_buffer_;

    static PipeResult mapLastError(DWORD error);
};

} // namespace dream_machine