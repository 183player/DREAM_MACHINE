// platform/dm_pipe/pipe.h
#pragma once

#include <windows.h>
#include <string>
#include <optional>

namespace dream_machine {

enum class PipeResult {
    OK,              // 操作成功
    TIMEOUT,         // 超时
    BROKEN,          // 管道断开（对端已关闭）
    CLOSED,          // 管道已关闭（本端）
    WOULD_BLOCK,     // 非阻塞操作无数据
    NOT_CONNECTED,   // 未连接
    INVALID_PARAM,   // 参数无效
    SYSTEM_ERROR,    // 系统调用失败
    BUSY,            // 管道繁忙（WaitNamedPipe 返回 ERROR_PIPE_BUSY）
};

class NamedPipe {
public:
    NamedPipe() = default;
    ~NamedPipe();

    NamedPipe(const NamedPipe&) = delete;
    NamedPipe& operator=(const NamedPipe&) = delete;

    NamedPipe(NamedPipe&& other) noexcept;
    NamedPipe& operator=(NamedPipe&& other) noexcept;

    // ============================================================
    // 服务端模式
    // ============================================================

    bool createServer(const std::wstring& pipe_name, DWORD max_instances = 1);
    PipeResult waitForClient(DWORD timeout_ms = INFINITE);

    // ============================================================
    // 客户端模式
    // ============================================================

    bool connect(const std::wstring& pipe_name, DWORD timeout_ms = 5000);

    // ============================================================
    // 读写操作
    // ============================================================

    PipeResult readLine(std::string& out_line, DWORD timeout_ms = 3000);
    PipeResult writeLine(const std::string& line, DWORD timeout_ms = 100);
    PipeResult peekAvailable(DWORD& out_bytes) const;

    // ============================================================
    // 控制操作
    // ============================================================

    bool cancelPendingIO();
    void close();
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] HANDLE getHandle() const { return pipe_handle_; }
    [[nodiscard]] HANDLE release();

private:
    HANDLE pipe_handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_ = {};
    bool io_pending_ = false;
    bool is_server_ = false;
    std::string read_buffer_;

    static PipeResult mapLastError(DWORD error);
};

} // namespace dream_machine