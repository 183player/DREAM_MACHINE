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
// 私有辅助：构建命令行
// ============================================================================

std::wstring Process::buildCommandLine(const ProcessStartOptions& options) {
    std::wstring cmd_line;

    // 可执行文件路径（含引号保护）
    if (options.executable.find(L' ') != std::wstring::npos) {
        cmd_line = L"\"" + options.executable + L"\"";
    } else {
        cmd_line = options.executable;
    }

    // 追加用户自定义参数
    if (!options.args.empty()) {
        cmd_line += L" " + options.args;
    }

    // 追加管道句柄参数（如果提供了句柄值）
    if (options.pipe_handle != 0) {
        cmd_line += L" " + options.pipe_handle_arg + L"=" + std::to_wstring(options.pipe_handle);
        LOG_INFO("buildCommandLine: added --pipe-handle=" + std::to_string(options.pipe_handle));
    }

    return cmd_line;
}

// ============================================================================
// 启动进程（原有签名，保持向后兼容）
// ============================================================================

bool Process::start(const std::wstring& executable,
                    const std::wstring& args,
                    bool inherit_handles) {
    ProcessStartOptions options;
    options.executable = executable;
    options.args = args;
    options.inherit_handles = inherit_handles;
    options.pipe_handle = 0;  // 默认不传递句柄
    return start(options);
}

// ============================================================================
// 启动进程（核心实现）
// ============================================================================

bool Process::start(const ProcessStartOptions& options) {
    // 关闭之前持有的句柄
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = nullptr;
        pid_ = 0;
    }

    // 构建完整命令行
    std::wstring cmd_line = buildCommandLine(options);

    // 准备启动信息
    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION pi = {nullptr, nullptr, 0, 0};

    // 创建进程
    BOOL success = CreateProcessW(
        options.executable.c_str(),
        cmd_line.data(),
        nullptr,
        nullptr,
        options.inherit_handles ? TRUE : FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!success) {
        DWORD err = GetLastError();
        std::string exe_str(options.executable.begin(), options.executable.end());
        LOG_ERROR("CreateProcessW failed for " + exe_str + ": error " + std::to_string(err));
        return false;
    }

    handle_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    CloseHandle(pi.hThread);

    std::string exe_str(options.executable.begin(), options.executable.end());
    if (options.pipe_handle != 0) {
        LOG_INFO("Process started: " + exe_str + " (PID: " + std::to_string(pid_) +
                 ", pipe-handle: " + std::to_string(options.pipe_handle) + ")");
    } else {
        LOG_INFO("Process started: " + exe_str + " (PID: " + std::to_string(pid_) + ")");
    }
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
    }
    if (result == WAIT_TIMEOUT) {
        return false;
    }
    LOG_ERROR("WaitForSingleObject failed: " + std::to_string(GetLastError()));
    return false;
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