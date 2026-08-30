// platform/dm_pipe/pipe.h
#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <vector>

namespace dream_machine {

// ================================================================
// 管道连接结果枚举
// ================================================================
enum class PipeResult {
    PIPE_OK = 0,
    PIPE_ERROR = 1,
    PIPE_TIMEOUT = 2,
    PIPE_BROKEN = 3,
    PIPE_NOT_CONNECTED = 4,
    PIPE_INVALID_PARAM = 5
};

// ================================================================
// NamedPipe 类
// ================================================================
class NamedPipe {
public:
    NamedPipe();
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

    bool createServer(const std::wstring& pipe_name,
                      int max_instances = 1,
                      bool secure = true);

    PipeResult waitForClient(DWORD timeout_ms = INFINITE) const;

    // ============================================================
    // 客户端 API
    // ============================================================

    bool connect(const std::wstring& pipe_name, DWORD timeout_ms = 5000);
    bool connectWithRetry(const std::wstring& pipe_name,
                          int max_retries = 3,
                          int delay_ms = 500);

    // ============================================================
    // 读写 API（JSON 行协议）
    // ============================================================

    // 发送一行数据（自动追加 \n）
    PipeResult writeLine(const std::string& message) const;

    // 读取一行数据（以 \n 结尾）—— 传统逐字节方式，保留兼容性
    PipeResult readLine(std::string& out_message, DWORD timeout_ms = 3000);

    // 【新增】缓冲批量读取——高效读取行，减少系统调用
    // 内部维护缓冲区，优先从缓冲区返回数据，不足时批量读取
    PipeResult readLineBuffered(std::string& out_message, DWORD timeout_ms = 3000);

    // 检查管道中是否有待读数据（不消耗数据）
    PipeResult peekAvailable(DWORD& out_bytes) const;

    // ============================================================
    // 状态检测 API
    // ============================================================

    bool isValid() const;
    bool isConnected() const;
    bool isBroken() const;

    void close();

    HANDLE getHandle() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool is_server_ = false;

    // 安全描述符相关
    std::unique_ptr<void, decltype(&LocalFree)> security_desc_{nullptr, LocalFree};

    // ----- 缓冲读取相关成员 -----
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<char> buffer_;          // 读取缓冲区
    size_t buffer_pos_ = 0;             // 当前读取位置
    size_t buffer_valid_ = 0;           // 缓冲区有效数据长度

    // 内部辅助
    static SECURITY_ATTRIBUTES* createSecureSecurityAttributes(
        SECURITY_ATTRIBUTES& sa_out,
        std::unique_ptr<void, decltype(&LocalFree)>& desc_out);

    // 内部读取实现（逐字节，保留原逻辑）
    PipeResult readLineInternal(std::string& out_message, DWORD timeout_ms);

    // 从缓冲区提取一行（不涉及系统调用）
    bool extractLineFromBuffer(std::string& out_line);

    // 从管道填充缓冲区（批量读取）
    PipeResult fillBuffer(DWORD timeout_ms);

    // 重置缓冲区状态
    void resetBuffer();
};

} // namespace dream_machine