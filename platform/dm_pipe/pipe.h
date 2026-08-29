// platform/dm_pipe/pipe.h
#pragma once

#include <windows.h>
#include <string>
#include <memory>

namespace dream_machine {

// ================================================================
// 管道连接结果枚举（使用 PIPE_ 前缀避免与 Windows 宏冲突）
// ================================================================
enum class PipeResult {
    PIPE_OK = 0,              // 成功
    PIPE_ERROR = 1,           // 一般错误
    PIPE_TIMEOUT = 2,         // 超时
    PIPE_BROKEN = 3,          // 对端已断开
    PIPE_NOT_CONNECTED = 4,   // 未连接
    PIPE_INVALID_PARAM = 5    // 无效参数
};

// ================================================================
// NamedPipe 类
// ================================================================
class NamedPipe {
public:
    NamedPipe() = default;
    ~NamedPipe();

    // 禁用拷贝
    NamedPipe(const NamedPipe&) = delete;
    NamedPipe& operator=(const NamedPipe&) = delete;

    // 移动语义
    NamedPipe(NamedPipe&& other) noexcept;
    NamedPipe& operator=(NamedPipe&& other) noexcept;

    // ============================================================
    // 服务端 API
    // ============================================================

    // 创建命名管道服务端
    // @param pipe_name      管道名称（如 \\.\pipe\DreamMachine_XXX）
    // @param max_instances  最大实例数（通常为 1，一对一通信）
    // @param secure         是否启用安全描述符（默认 true，仅允许当前用户连接）
    // @return               成功返回 true
    bool createServer(const std::wstring& pipe_name,
                      int max_instances = 1,
                      bool secure = true);

    // 等待客户端连接（const，不修改对象状态）
    // @param timeout_ms     超时时间（毫秒），INFINITE 表示无限等待
    // @return               连接结果
    PipeResult waitForClient(DWORD timeout_ms = INFINITE) const;

    // ============================================================
    // 客户端 API
    // ============================================================

    // 连接到已存在的命名管道服务端
    // @param pipe_name      管道名称
    // @param timeout_ms     连接超时（毫秒）
    // @return               成功返回 true
    bool connect(const std::wstring& pipe_name, DWORD timeout_ms = 5000);

    // ============================================================
    // 连接辅助工具（子进程专用）
    // ============================================================

    // 带重试的连接（供子进程使用）
    // @param pipe_name      管道名称
    // @param max_retries    最大重试次数（默认 3）
    // @param delay_ms       重试间隔（毫秒，默认 500）
    // @return               成功返回 true，失败返回 false（调用者应退出）
    bool connectWithRetry(const std::wstring& pipe_name,
                          int max_retries = 3,
                          int delay_ms = 500);

    // ============================================================
    // 读写 API（JSON 行协议）
    // ============================================================

    // 发送一行数据（自动追加 \n）
    // @param message        消息内容（不含换行符）
    // @return               操作结果
    PipeResult writeLine(const std::string& message) const;

    // 读取一行数据（以 \n 结尾）
    // @param out_message    输出消息（不含换行符）
    // @param timeout_ms     读取超时（毫秒）
    // @return               操作结果
    PipeResult readLine(std::string& out_message, DWORD timeout_ms = 3000) const;

    // 检查管道中是否有待读数据
    // @param out_bytes      输出待读字节数
    // @return               操作结果
    PipeResult peekAvailable(DWORD& out_bytes) const;

    // ============================================================
    // 状态检测 API（增强版）
    // ============================================================

    // 检查管道是否有效（句柄存在且未关闭）
    bool isValid() const;

    // 检查管道是否已连接（服务端和客户端已握手）
    bool isConnected() const;

    // 可靠检测管道是否已断开（对端关闭）
    // 内部通过 PeekNamedPipe 主动检测，返回 true 表示已断开
    bool isBroken() const;

    // 关闭管道
    void close();

    // 获取原生句柄（仅供特殊调试使用）
    HANDLE getHandle() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool is_server_ = false;

    // 安全描述符相关（仅 createServer 使用）
    std::unique_ptr<void, decltype(&LocalFree)> security_desc_{nullptr, LocalFree};

    // 创建安全描述符（仅限当前用户访问）
    static SECURITY_ATTRIBUTES* createSecureSecurityAttributes(
        SECURITY_ATTRIBUTES& sa_out,
        std::unique_ptr<void, decltype(&LocalFree)>& desc_out);

    // 内部读取实现（逐字节读取直到换行符）
    PipeResult readLineInternal(std::string& out_message, DWORD timeout_ms) const;
};

} // namespace dream_machine