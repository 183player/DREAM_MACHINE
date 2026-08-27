// platform/dm_process/process.h
#pragma once

#include <windows.h>
#include <string>
#include <optional>

namespace dream_machine {

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

        // 启动进程
        bool start(const std::wstring& executable,
                   const std::wstring& args = L"",
                   bool inherit_handles = false);

        // 等待进程退出
        bool waitForExit(DWORD timeout_ms = INFINITE) const;

        // 强制终止进程
        bool terminate(DWORD exit_code = 1) const;

        // 检查进程是否仍在运行
        [[nodiscard]] bool isRunning() const;

        // 获取进程 ID
        [[nodiscard]] DWORD getPid() const { return pid_; }

        // 获取内部句柄
        [[nodiscard]] HANDLE getHandle() const { return handle_; }

        // 释放句柄所有权
        [[nodiscard]] HANDLE release();

        // 获取进程退出码
        [[nodiscard]] std::optional<DWORD> getExitCode() const;

    private:
        HANDLE handle_ = nullptr;
        DWORD pid_ = 0;
    };

} // namespace dream_machine