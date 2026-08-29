// platform/dm_process/process.cpp
#include "process.h"

#include "logger.h"

#include <string>

namespace dream_machine {

Process::~Process() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

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

std::wstring Process::buildCommandLine(const ProcessStartOptions& options) {
    std::wstring cmd_line;
    if (options.executable.find(L' ') != std::wstring::npos) {
        cmd_line = L"\"" + options.executable + L"\"";
    } else {
        cmd_line = options.executable;
    }
    if (!options.args.empty()) {
        cmd_line += L" " + options.args;
    }
    // 不再追加 --pipe-handle
    return cmd_line;
}

bool Process::start(const std::wstring& executable,
                    const std::wstring& args,
                    bool inherit_handles) {
    ProcessStartOptions options;
    options.executable = executable;
    options.args = args;
    options.inherit_handles = inherit_handles;
    return start(options);
}

bool Process::start(const ProcessStartOptions& options) {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = nullptr;
        pid_ = 0;
    }

    std::wstring cmd_line = buildCommandLine(options);

    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION pi = {nullptr, nullptr, 0, 0};

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
    LOG_INFO("Process started: " + exe_str + " (PID: " + std::to_string(pid_) + ")");
    return true;
}

bool Process::waitForExit(DWORD timeout_ms) const {
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

bool Process::terminate(DWORD exit_code) const {
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

bool Process::isRunning() const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    return WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
}

HANDLE Process::release() {
    HANDLE h = handle_;
    handle_ = nullptr;
    pid_ = 0;
    return h;
}

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