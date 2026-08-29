// platform/dm_process/process.h
#pragma once

#include <windows.h>
#include <string>
#include <optional>

namespace dream_machine {

    struct ProcessStartOptions {
        std::wstring executable;
        std::wstring args;
        bool inherit_handles = true;
        // 移除：pipe_handle 和 pipe_handle_arg
    };

    class Process {
    public:
        Process() = default;
        ~Process();

        Process(const Process&) = delete;
        Process& operator=(const Process&) = delete;

        Process(Process&& other) noexcept;
        Process& operator=(Process&& other) noexcept;

        bool start(const std::wstring& executable,
                   const std::wstring& args = L"",
                   bool inherit_handles = false);

        bool start(const ProcessStartOptions& options);

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

        static std::wstring buildCommandLine(const ProcessStartOptions& options);
    };

} // namespace dream_machine