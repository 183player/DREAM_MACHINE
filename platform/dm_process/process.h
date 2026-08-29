// platform/dm_process/process.h
#pragma once

#include <windows.h>
#include <string>
#include <optional>
#include <cstdint>

namespace dream_machine {

// ============================================================================
// 进程启动选项（扩展：支持传递管道句柄）
// ============================================================================

struct ProcessStartOptions {
    std::wstring executable;                      // 可执行文件路径
    std::wstring args;                            // 额外命令行参数（可选）
    bool inherit_handles = true;                  // 是否继承句柄
    uintptr_t pipe_handle = 0;                    // 要传递给子进程的管道句柄值（0 表示不传递）
    std::wstring pipe_handle_arg = L"--pipe-handle";  // 命令行参数名
};

// ============================================================================
// Process 类
// ============================================================================

class Process {
public:
    Process() = default;
    ~Process();

    // 禁止拷贝
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // 移动语义
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;

    // ============================================================
    // 启动进程
    // ============================================================

    // 方式一：简单启动（向后兼容）
    bool start(const std::wstring& executable,
               const std::wstring& args = L"",
               bool inherit_handles = false);

    // 方式二：使用 ProcessStartOptions（支持传递管道句柄）
    bool start(const ProcessStartOptions& options);

    // ============================================================
    // 进程控制
    // ============================================================

    bool waitForExit(DWORD timeout_ms = INFINITE) const;
    bool terminate(DWORD exit_code = 1) const;
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] DWORD getPid() const { return pid_; }
    [[nodiscard]] HANDLE getHandle() const { return handle_; }
    [[nodiscard]] HANDLE release();
    [[nodiscard]] std::optional<DWORD> getExitCode() const;

private:
    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;

    // 内部辅助：构建完整命令行
    static std::wstring buildCommandLine(const ProcessStartOptions& options);
};

} // namespace dream_machine