// platform/dm_process/process.cpp
#include "process.h"

#include "logger.h"
#include "common_utils.h"

#include <string>
#include <memory>

// 用于 QueryInformationJobObject
#include <winternl.h>

#pragma comment(lib, "ntdll.lib")

namespace dream_machine {

// ================================================================
// 构造函数 / 析构函数 / 移动语义
// ================================================================

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

// ================================================================
// 内部辅助：构建命令行
// ================================================================

std::wstring Process::buildCommandLine(const ProcessStartOptions& options) {
    std::wstring cmd_line;

    // 可执行文件路径（如有空格则加引号）
    if (options.executable.find(L' ') != std::wstring::npos) {
        cmd_line = L"\"" + options.executable + L"\"";
    } else {
        cmd_line = options.executable;
    }

    // 追加参数
    if (!options.args.empty()) {
        cmd_line += L" " + options.args;
    }

    // 注意：不再追加 --pipe-handle 参数（名称约定方案已废弃句柄传递）
    return cmd_line;
}

// ================================================================
// 内部辅助：获取进程真实句柄（用于作业查询）
// ================================================================

HANDLE Process::getRealHandle() const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    // 尝试以 QUERY_LIMITED_INFORMATION 权限打开进程
    if (HANDLE h_query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid_)) {
        return h_query;
    }

    return handle_;
}

// ================================================================
// 启动 API
// ================================================================

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

    if (options.executable.empty()) {
        LOG_ERROR("start: executable path is empty");
        return false;
    }

    std::wstring cmd_line = buildCommandLine(options);

    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION pi = {nullptr, nullptr, 0, 0};

    auto creation_flags = static_cast<DWORD>(options.creation_flags);

    BOOL inherit = options.inherit_handles ? TRUE : FALSE;

    BOOL success = CreateProcessW(
        options.executable.c_str(),
        cmd_line.data(),
        nullptr,
        nullptr,
        inherit,
        creation_flags,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!success) {
        DWORD err = GetLastError();
        // C4 编码 helper（阶段 1.7）：
        //   原 std::string(exe.begin(), exe.end()) 仅对 ASCII 有效；
        //   wideToUtf8 保证非 ASCII 正确转换。
        const std::string exe_str = common::wideToUtf8(options.executable);
        LOG_ERROR("CreateProcessW failed for " + exe_str + ": error " + std::to_string(err));
        return false;
    }

    handle_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    CloseHandle(pi.hThread);

    // C4 编码 helper（阶段 1.7）：同上
    const std::string exe_str = common::wideToUtf8(options.executable);
    LOG_INFO("Process started: " + exe_str + " (PID: " + std::to_string(pid_) + ")");

    // ============================================================
    // 如果指定了 Job Object，将进程加入
    // ============================================================
    if (options.job_handle != nullptr && options.job_handle != INVALID_HANDLE_VALUE) {
        if (!assignToJob(options.job_handle)) {
            LOG_WARN("Failed to assign process (PID: " + std::to_string(pid_) +
                     ") to Job Object, continuing anyway");
        }
    }

    // 等待进程进入可运行状态（可选）
    if (options.timeout_ms > 0) {
        DWORD wait_result = WaitForInputIdle(handle_, options.timeout_ms);
        if (wait_result == WAIT_TIMEOUT) {
            LOG_WARN("Process (PID: " + std::to_string(pid_) +
                     ") did not become idle within " + std::to_string(options.timeout_ms) + "ms");
        } else if (wait_result == WAIT_OBJECT_0) {
            LOG_INFO("Process (PID: " + std::to_string(pid_) + ") is ready");
        }
    }

    return true;
}

// ================================================================
// Job Object 管理
// ================================================================

bool Process::assignToJob(HANDLE job_handle) const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        LOG_ERROR("assignToJob: invalid process handle");
        return false;
    }

    if (job_handle == nullptr || job_handle == INVALID_HANDLE_VALUE) {
        LOG_ERROR("assignToJob: invalid job handle");
        return false;
    }

    if (!AssignProcessToJobObject(job_handle, handle_)) {
        DWORD err = GetLastError();
        LOG_ERROR("AssignProcessToJobObject failed (PID: " + std::to_string(pid_) +
                  "): error " + std::to_string(err));

        if (err == ERROR_ACCESS_DENIED) {
            LOG_ERROR("  Access denied: process may already be in a job without breakaway");
        } else if (err == ERROR_NOT_SUPPORTED) {
            LOG_ERROR("  Not supported: nested job not allowed");
        }
        return false;
    }

    LOG_INFO("Process (PID: " + std::to_string(pid_) + ") assigned to Job Object");
    return true;
}

bool Process::isInJob(std::optional<DWORD>* out_job_id) const {
    if (out_job_id) {
        out_job_id->reset();
    }

    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    BOOL is_in_job = FALSE;
    if (!IsProcessInJob(handle_, nullptr, &is_in_job)) {
        DWORD err = GetLastError();
        if (err == ERROR_INVALID_HANDLE) {
            return false;
        }
        LOG_WARN("IsProcessInJob failed (PID: " + std::to_string(pid_) +
                 "): error " + std::to_string(err));
        return false;
    }

    if (!is_in_job) {
        return false;
    }

    if (out_job_id) {
        if (HANDLE h_query = getRealHandle()) {
            *out_job_id = 0;
            if (h_query != handle_) {
                CloseHandle(h_query);
            }
        }
    }

    return true;
}

bool Process::breakawayFromJob() const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        LOG_ERROR("breakawayFromJob: invalid process handle");
        return false;
    }

    BOOL is_in_job = FALSE;
    if (!IsProcessInJob(handle_, nullptr, &is_in_job)) {
        DWORD err = GetLastError();
        LOG_ERROR("IsProcessInJob failed: error " + std::to_string(err));
        return false;
    }

    if (!is_in_job) {
        LOG_INFO("Process is not in any Job Object, breakaway not needed");
        return true;
    }

    HANDLE temp_job = CreateJobObjectW(nullptr, nullptr);
    if (!temp_job) {
        DWORD err = GetLastError();
        LOG_ERROR("CreateJobObject failed: error " + std::to_string(err));
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(temp_job, JobObjectExtendedLimitInformation,
                                 &job_info, sizeof(job_info))) {
        DWORD err = GetLastError();
        LOG_WARN("SetInformationJobObject failed: error " + std::to_string(err));
    }

    bool result = false;
    if (AssignProcessToJobObject(temp_job, handle_)) {
        LOG_INFO("Process (PID: " + std::to_string(pid_) + ") moved to temporary Job Object");
        result = true;
    } else {
        DWORD err = GetLastError();
        LOG_ERROR("AssignProcessToJobObject to temp job failed: error " + std::to_string(err));
    }

    CloseHandle(temp_job);
    return result;
}

// ================================================================
// 进程控制
// ================================================================

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
        LOG_ERROR("TerminateProcess failed (PID: " + std::to_string(pid_) +
                  "): error " + std::to_string(err));
        return false;
    }

    LOG_WARN("Process terminated (PID: " + std::to_string(pid_) +
             ") with exit code " + std::to_string(exit_code));
    return true;
}

// ================================================================
// 状态查询
// ================================================================

bool Process::isRunning() const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD wait_result = WaitForSingleObject(handle_, 0);
    return wait_result == WAIT_TIMEOUT;
}

std::optional<DWORD> Process::getExitCode() const {
    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    DWORD wait_result = WaitForSingleObject(handle_, 0);
    if (wait_result != WAIT_OBJECT_0) {
        return std::nullopt;
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(handle_, &exit_code)) {
        LOG_ERROR("GetExitCodeProcess failed (PID: " + std::to_string(pid_) +
                  "): error " + std::to_string(GetLastError()));
        return std::nullopt;
    }

    return exit_code;
}

// ================================================================
// 句柄释放
// ================================================================

HANDLE Process::release() {
    HANDLE h = handle_;
    handle_ = nullptr;
    pid_ = 0;
    return h;
}

} // namespace dream_machine