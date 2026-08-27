// platform/dm_process/process.cpp
#include "process.h"

#include "logger.h"

#include <string>
#include <vector>

namespace dream_machine {

// ============================================================================
// 构造 / 析构
// ============================================================================

Process::~Process() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

// ============================================================================
// 移动语义
// ============================================================================

Process::Process(Process&& other) noexcept
    : handle_(other.handle_)
    , pid_(other.pid_) {
    other.handle_ = nullptr;
    other.pid_ = 0;
}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = other.handle_;
        pid_ = other.pid_;
        other.handle_ = nullptr;
        other.pid_ = 0;
    }
    return *this;
}

// ============================================================================
// 启动进程
// ============================================================================

bool Process::start(const std::wstring& executable,
                    const std::wstring& args,
                    bool inherit_handles) {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = nullptr;
        pid_ = 0;
    }

    std::wstring cmd_line;
    if (executable.find(L' ') != std::wstring::npos) {
        cmd_line = L"\"" + executable + L"\"";
    } else {
        cmd_line = executable;
    }
    if (!args.empty()) {
        cmd_line += L" " + args;
    }

    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION pi = {nullptr, nullptr, 0, 0};

    BOOL success = CreateProcessW(
        executable.c_str(),
        cmd_line.data(),
        nullptr,
        nullptr,
        inherit_handles,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!success) {
        DWORD err = GetLastError();
        std::string exe_str(executable.begin(), executable.end());
        LOG_ERROR("CreateProcessW failed for " + exe_str + ": error " + std::to_string(err));
        return false;
    }

    handle_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    CloseHandle(pi.hThread);

    std::string exe_str(executable.begin(), executable.end());
    LOG_INFO("Process started: " + exe_str + " (PID: " + std::to_string(pid_) + ")");
    return true;
}

// ============================================================================
// 等待退出
// ============================================================================

bool Process::waitForExit(DWORD timeout_ms) const
{
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        LOG_WARN("waitForExit called on invalid handle");
        return true;
    }

    DWORD result = WaitForSingleObject(handle_, timeout_ms);
    if (result == WAIT_OBJECT_0) {
        return true;
    } else if (result == WAIT_TIMEOUT) {
        return false;
    } else {
        LOG_ERROR("WaitForSingleObject failed: " + std::to_string(GetLastError()));
        return false;
    }
}

// ============================================================================
// 强制终止
// ============================================================================

bool Process::terminate(DWORD exit_code) const
{
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        LOG_WARN("terminate called on invalid handle");
        return false;
    }

    if (!TerminateProcess(handle_, exit_code)) {
        DWORD err = GetLastError();
        LOG_ERROR("TerminateProcess failed (PID: " + std::to_string(pid_) + "): error " + std::to_string(err));
        return false;
    }

    LOG_WARN("Process terminated (PID: " + std::to_string(pid_) + ") with exit code " + std::to_string(exit_code));
    return true;
}

// ============================================================================
// 存活检测
// ============================================================================

bool Process::isRunning() const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    return WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
}

// ============================================================================
// 释放句柄
// ============================================================================

HANDLE Process::release() {
    HANDLE h = handle_;
    handle_ = nullptr;
    pid_ = 0;
    return h;
}

// ============================================================================
// 获取退出码
// ============================================================================

std::optional<DWORD> Process::getExitCode() const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    DWORD result = WaitForSingleObject(handle_, 0);
    if (result != WAIT_OBJECT_0) {
        return std::nullopt;
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(handle_, &exit_code)) {
        LOG_ERROR("GetExitCodeProcess failed (PID: " + std::to_string(pid_) + "): error " + std::to_string(GetLastError()));
        return std::nullopt;
    }

    return exit_code;
}

} // namespace dream_machine