// platform/dm_process/process.h
#pragma once

#include <windows.h>
#include <string>
#include <optional>

namespace dream_machine {

// ================================================================
// 进程创建标志（使用 PROC_ 前缀避免与 Windows 宏冲突）
// ================================================================
enum class ProcessCreationFlags : DWORD {
    PROC_NONE = 0,
    PROC_NO_WINDOW = CREATE_NO_WINDOW,           // 不创建控制台窗口
    PROC_BREAKAWAY_FROM_JOB = CREATE_BREAKAWAY_FROM_JOB, // 脱离父作业对象
    PROC_SUSPENDED = CREATE_SUSPENDED,           // 创建时挂起
    PROC_NEW_CONSOLE = CREATE_NEW_CONSOLE        // 创建新控制台
};

// 按位或运算符重载（允许组合）
inline ProcessCreationFlags operator|(ProcessCreationFlags a, ProcessCreationFlags b) {
    return static_cast<ProcessCreationFlags>(
        static_cast<DWORD>(a) | static_cast<DWORD>(b)
    );
}

inline ProcessCreationFlags operator|=(ProcessCreationFlags& a, ProcessCreationFlags b) {
    a = a | b;
    return a;
}

inline bool hasFlag(ProcessCreationFlags flags, ProcessCreationFlags flag) {
    return (static_cast<DWORD>(flags) & static_cast<DWORD>(flag)) != 0;
}

// ================================================================
// 进程启动选项
// ================================================================
struct ProcessStartOptions {
    std::wstring executable;          // 可执行文件路径
    std::wstring args;                // 命令行参数
    bool inherit_handles = true;      // 是否继承句柄
    HANDLE job_handle = nullptr;      // 关联的 Job Object（可选）
    ProcessCreationFlags creation_flags = ProcessCreationFlags::PROC_NO_WINDOW; // 创建标志
    DWORD timeout_ms = 5000;          // 等待进程初始化的超时（ms）
};

// ================================================================
// Process 类
// ================================================================
class Process {
public:
    Process() = default;
    ~Process();

    // 禁用拷贝
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // 移动语义
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;

    // ============================================================
    // 启动 API
    // ============================================================

    // 简化启动接口（兼容旧代码）
    bool start(const std::wstring& executable,
               const std::wstring& args = L"",
               bool inherit_handles = false);

    // 完整启动接口
    bool start(const ProcessStartOptions& options);

    // ============================================================
    // Job Object 管理
    // ============================================================

    // 将进程分配到指定的 Job Object
    // @param job_handle  作业对象句柄
    // @return            成功返回 true
    bool assignToJob(HANDLE job_handle) const;

    // 检查进程是否属于某个 Job Object
    // @param out_job_id  输出作业对象 ID（可选）
    // @return            属于作业返回 true
    bool isInJob(std::optional<DWORD>* out_job_id = nullptr) const;

    // 从当前 Job Object 中脱离（需要 CREATE_BREAKAWAY_FROM_JOB 权限）
    bool breakawayFromJob() const;

    // ============================================================
    // 进程控制
    // ============================================================

    // 等待进程退出
    // @param timeout_ms  超时（毫秒），INFINITE 表示无限等待
    // @return            进程已退出返回 true
    bool waitForExit(DWORD timeout_ms = INFINITE) const;

    // 强制终止进程
    // @param exit_code   退出码（默认 1）
    // @return            成功返回 true
    bool terminate(DWORD exit_code = 1) const;

    // ============================================================
    // 状态查询
    // ============================================================

    // 检查进程是否正在运行
    [[nodiscard]] bool isRunning() const;

    // 获取进程 ID
    [[nodiscard]] DWORD getPid() const { return pid_; }

    // 获取进程句柄（只读）
    [[nodiscard]] HANDLE getHandle() const { return handle_; }

    // 获取进程退出码（如果已退出）
    [[nodiscard]] std::optional<DWORD> getExitCode() const;

    // ============================================================
    // 句柄释放（转移所有权）
    // ============================================================

    // 释放进程句柄所有权（调用者负责关闭）
    [[nodiscard]] HANDLE release();

private:
    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;

    // 内部辅助：构建命令行
    static std::wstring buildCommandLine(const ProcessStartOptions& options);

    // 内部辅助：获取进程真实句柄（用于作业查询）
    HANDLE getRealHandle() const;
};

} // namespace dream_machine