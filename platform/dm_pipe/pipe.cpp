// platform/dm_pipe/pipe.cpp
#include "pipe.h"

#include "logger.h"
#include "constants.h"
#include "error_codes.h"

#include <algorithm>
#include <cstring>

namespace dream_machine {

// ============================================================================
// 构造 / 析构
// ============================================================================

NamedPipe::~NamedPipe() {
    close();
}

// ============================================================================
// 移动语义
// ============================================================================

NamedPipe::NamedPipe(NamedPipe&& other) noexcept
    : pipe_handle_(other.pipe_handle_)
    , overlapped_(other.overlapped_)
    , io_pending_(other.io_pending_)
    , is_server_(other.is_server_)
    , read_buffer_(std::move(other.read_buffer_)) {
    other.pipe_handle_ = INVALID_HANDLE_VALUE;
    other.io_pending_ = false;
    other.is_server_ = false;
    std::memset(&other.overlapped_, 0, sizeof(OVERLAPPED));
}

NamedPipe& NamedPipe::operator=(NamedPipe&& other) noexcept {
    if (this != &other) {
        close();
        pipe_handle_ = other.pipe_handle_;
        overlapped_ = other.overlapped_;
        io_pending_ = other.io_pending_;
        is_server_ = other.is_server_;
        read_buffer_ = std::move(other.read_buffer_);
        other.pipe_handle_ = INVALID_HANDLE_VALUE;
        other.io_pending_ = false;
        other.is_server_ = false;
        std::memset(&other.overlapped_, 0, sizeof(OVERLAPPED));
    }
    return *this;
}

// ============================================================================
// 服务端模式
// ============================================================================

bool NamedPipe::createServer(const std::wstring& pipe_name, DWORD max_instances) {
    close();

    HANDLE hPipe = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        max_instances,
        4096,   // 输出缓冲区大小
        4096,   // 输入缓冲区大小
        0,      // 默认超时
        nullptr // 默认安全属性
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::string name_str(pipe_name.begin(), pipe_name.end());
        LOG_ERROR("CreateNamedPipeW failed for " + name_str + ": error " + std::to_string(err));
        return false;
    }

    pipe_handle_ = hPipe;
    is_server_ = true;
    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));

    std::string name_str(pipe_name.begin(), pipe_name.end());
    LOG_INFO("Named pipe server created: " + name_str);
    return true;
}

PipeResult NamedPipe::waitForClient(DWORD timeout_ms) {
    if (pipe_handle_ == INVALID_HANDLE_VALUE || !is_server_) {
        LOG_WARN("waitForClient called on non-server pipe");
        return PipeResult::NOT_CONNECTED;
    }

    // 重置 OVERLAPPED
    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
    overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    BOOL connected = ConnectNamedPipe(pipe_handle_, &overlapped_);
    DWORD err = GetLastError();

    if (connected) {
        // 同步连接成功
        CloseHandle(overlapped_.hEvent);
        std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
        LOG_INFO("Client connected to server pipe");
        return PipeResult::OK;
    }

    if (err == ERROR_IO_PENDING) {
        // 等待连接完成
        DWORD wait_result = WaitForSingleObject(overlapped_.hEvent, timeout_ms);
        CloseHandle(overlapped_.hEvent);
        std::memset(&overlapped_, 0, sizeof(OVERLAPPED));

        if (wait_result == WAIT_OBJECT_0) {
            LOG_INFO("Client connected to server pipe");
            return PipeResult::OK;
        } else if (wait_result == WAIT_TIMEOUT) {
            CancelIoEx(pipe_handle_, &overlapped_);
            LOG_WARN("waitForClient timeout");
            return PipeResult::TIMEOUT;
        } else {
            LOG_ERROR("waitForClient failed: " + std::to_string(GetLastError()));
            return mapLastError(GetLastError());
        }
    }

    if (err == ERROR_PIPE_CONNECTED) {
        // 客户端已连接
        CloseHandle(overlapped_.hEvent);
        std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
        LOG_INFO("Client already connected to server pipe");
        return PipeResult::OK;
    }

    CloseHandle(overlapped_.hEvent);
    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
    LOG_ERROR("ConnectNamedPipe failed: " + std::to_string(err));
    return mapLastError(err);
}

// ============================================================================
// 客户端模式
// ============================================================================

bool NamedPipe::connect(const std::wstring& pipe_name, DWORD timeout_ms) {
    close();

    // 等待管道可用
    if (!WaitNamedPipeW(pipe_name.c_str(), timeout_ms)) {
        DWORD err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) {
            LOG_WARN("WaitNamedPipeW timeout for " + std::string(pipe_name.begin(), pipe_name.end()));
            return false;
        }
        LOG_ERROR("WaitNamedPipeW failed: " + std::to_string(err));
        return false;
    }

    HANDLE hPipe = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::string name_str(pipe_name.begin(), pipe_name.end());
        LOG_ERROR("CreateFileW failed for " + name_str + ": error " + std::to_string(err));
        return false;
    }

    pipe_handle_ = hPipe;
    is_server_ = false;
    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));

    std::string name_str(pipe_name.begin(), pipe_name.end());
    LOG_INFO("Connected to named pipe: " + name_str);
    return true;
}

// ============================================================================
// 读取一行
// ============================================================================

PipeResult NamedPipe::readLine(std::string& out_line, DWORD timeout_ms) {
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::NOT_CONNECTED;
    }

    // 先检查缓冲区中是否已有完整行
    size_t pos = read_buffer_.find('\n');
    if (pos != std::string::npos) {
        out_line = read_buffer_.substr(0, pos);
        // 移除已读取的部分（包括 \n）
        if (pos + 1 < read_buffer_.size()) {
            read_buffer_ = read_buffer_.substr(pos + 1);
        } else {
            read_buffer_.clear();
        }
        // 去除末尾 \r（Windows CRLF 兼容）
        if (!out_line.empty() && out_line.back() == '\r') {
            out_line.pop_back();
        }
        return PipeResult::OK;
    }

    // 需要从管道读取更多数据
    char buffer[4096];
    DWORD bytes_read = 0;

    while (true) {
        std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL success = ReadFile(pipe_handle_, buffer, sizeof(buffer) - 1, nullptr, &overlapped_);
        DWORD err = GetLastError();

        if (!success && err == ERROR_IO_PENDING) {
            DWORD wait_result = WaitForSingleObject(overlapped_.hEvent, timeout_ms);
            if (wait_result == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(pipe_handle_, &overlapped_, &bytes_read, FALSE)) {
                    CloseHandle(overlapped_.hEvent);
                    return mapLastError(GetLastError());
                }
            } else if (wait_result == WAIT_TIMEOUT) {
                CancelIoEx(pipe_handle_, &overlapped_);
                CloseHandle(overlapped_.hEvent);
                return PipeResult::TIMEOUT;
            } else {
                CloseHandle(overlapped_.hEvent);
                return mapLastError(GetLastError());
            }
        } else if (success) {
            bytes_read = 0;
            if (!GetOverlappedResult(pipe_handle_, &overlapped_, &bytes_read, TRUE)) {
                CloseHandle(overlapped_.hEvent);
                return mapLastError(GetLastError());
            }
        } else {
            CloseHandle(overlapped_.hEvent);
            if (err == ERROR_BROKEN_PIPE) {
                return PipeResult::BROKEN;
            }
            return mapLastError(err);
        }

        CloseHandle(overlapped_.hEvent);

        if (bytes_read == 0) {
            // EOF：对端关闭了管道
            return PipeResult::BROKEN;
        }

        // 追加到缓冲区
        buffer[bytes_read] = '\0';
        read_buffer_.append(buffer, bytes_read);

        // 检查是否有完整行
        pos = read_buffer_.find('\n');
        if (pos != std::string::npos) {
            out_line = read_buffer_.substr(0, pos);
            if (pos + 1 < read_buffer_.size()) {
                read_buffer_ = read_buffer_.substr(pos + 1);
            } else {
                read_buffer_.clear();
            }
            if (!out_line.empty() && out_line.back() == '\r') {
                out_line.pop_back();
            }
            return PipeResult::OK;
        }

        // 缓冲区过大保护（防止恶意数据撑爆内存）
        if (read_buffer_.size() > 1024 * 1024) { // 1MB 上限
            LOG_ERROR("Pipe read buffer exceeded 1MB limit");
            read_buffer_.clear();
            return PipeResult::SYSTEM_ERROR;
        }
    }
}

// ============================================================================
// 写入一行
// ============================================================================

PipeResult NamedPipe::writeLine(const std::string& line, DWORD timeout_ms) {
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::NOT_CONNECTED;
    }

    std::string data = line + "\n";
    DWORD bytes_written = 0;
    DWORD total_written = 0;

    while (total_written < data.size()) {
        auto to_write = static_cast<DWORD>(data.size() - total_written);
        std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL success = WriteFile(pipe_handle_,
                                 data.data() + total_written,
                                 to_write,
                                 nullptr,
                                 &overlapped_);
        DWORD err = GetLastError();

        if (!success && err == ERROR_IO_PENDING) {
            DWORD wait_result = WaitForSingleObject(overlapped_.hEvent, timeout_ms);
            if (wait_result == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(pipe_handle_, &overlapped_, &bytes_written, FALSE)) {
                    CloseHandle(overlapped_.hEvent);
                    return mapLastError(GetLastError());
                }
            } else if (wait_result == WAIT_TIMEOUT) {
                CancelIoEx(pipe_handle_, &overlapped_);
                CloseHandle(overlapped_.hEvent);
                return PipeResult::TIMEOUT;
            } else {
                CloseHandle(overlapped_.hEvent);
                return mapLastError(GetLastError());
            }
        } else if (success) {
            bytes_written = 0;
            if (!GetOverlappedResult(pipe_handle_, &overlapped_, &bytes_written, TRUE)) {
                CloseHandle(overlapped_.hEvent);
                return mapLastError(GetLastError());
            }
        } else {
            CloseHandle(overlapped_.hEvent);
            if (err == ERROR_BROKEN_PIPE) {
                return PipeResult::BROKEN;
            }
            return mapLastError(err);
        }

        CloseHandle(overlapped_.hEvent);

        if (bytes_written == 0) {
            return PipeResult::BROKEN;
        }

        total_written += bytes_written;
    }

    return PipeResult::OK;
}

// ============================================================================
// 非阻塞探测
// ============================================================================

PipeResult NamedPipe::peekAvailable(DWORD& out_bytes) const
{
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::NOT_CONNECTED;
    }

    DWORD bytes_available = 0;
    if (!PeekNamedPipe(pipe_handle_, nullptr, 0, nullptr, &bytes_available, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            return PipeResult::BROKEN;
        }
        return mapLastError(err);
    }

    out_bytes = bytes_available;
    return (bytes_available > 0) ? PipeResult::OK : PipeResult::WOULD_BLOCK;
}

// ============================================================================
// 控制操作
// ============================================================================

bool NamedPipe::cancelPendingIO() {
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    if (!CancelIoEx(pipe_handle_, &overlapped_)) {
        DWORD err = GetLastError();
        if (err != ERROR_NOT_FOUND) { // 没有挂起的 IO 是正常情况
            LOG_WARN("CancelIoEx failed: " + std::to_string(err));
            return false;
        }
    }

    io_pending_ = false;
    return true;
}

void NamedPipe::close() {
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        cancelPendingIO();

        if (is_server_) {
            DisconnectNamedPipe(pipe_handle_);
        }

        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
        is_server_ = false;
        read_buffer_.clear();
        std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
    }
}

bool NamedPipe::isConnected() const {
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    // 使用 PeekNamedPipe 检测管道状态
    DWORD bytes_available = 0;
    if (!PeekNamedPipe(pipe_handle_, nullptr, 0, nullptr, &bytes_available, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            return false;
        }
        return true; // 其他错误可能是临时状态，乐观判断
    }

    return true;
}

HANDLE NamedPipe::release() {
    HANDLE h = pipe_handle_;
    pipe_handle_ = INVALID_HANDLE_VALUE;
    is_server_ = false;
    read_buffer_.clear();
    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
    return h;
}

// ============================================================================
// 辅助方法
// ============================================================================

    PipeResult NamedPipe::mapLastError(DWORD error)
    {
    switch (error) {
    case ERROR_SUCCESS:
        return PipeResult::OK;
    case ERROR_SEM_TIMEOUT:
        return PipeResult::TIMEOUT;
    case ERROR_BROKEN_PIPE:
    case ERROR_PIPE_NOT_CONNECTED:
        return PipeResult::BROKEN;
    case ERROR_NO_DATA:
        return PipeResult::WOULD_BLOCK;
    case ERROR_PIPE_BUSY:
        return PipeResult::BUSY;
    case ERROR_INVALID_PARAMETER:
        return PipeResult::INVALID_PARAM;
    default:
        return PipeResult::SYSTEM_ERROR;
    }
}

} // namespace dream_machine