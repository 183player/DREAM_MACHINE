// platform/dm_pipe/pipe.cpp
#include "pipe.h"

#include "logger.h"

#include <cstring>
#include <string>

namespace dream_machine {

// ============================================================================
// 构造 / 析构
// ============================================================================

NamedPipe::NamedPipe(HANDLE handle, bool owns_handle)
    : pipe_handle_(handle)
    , owns_handle_(owns_handle) {
    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
    LOG_INFO("NamedPipe created with handle: " + std::to_string(reinterpret_cast<uintptr_t>(handle)) +
             ", owns_handle=" + (owns_handle ? "true" : "false"));
}

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
    , owns_handle_(other.owns_handle_)
    , read_buffer_(std::move(other.read_buffer_)) {
    other.pipe_handle_ = INVALID_HANDLE_VALUE;
    other.io_pending_ = false;
    other.is_server_ = false;
    other.owns_handle_ = true;
    std::memset(&other.overlapped_, 0, sizeof(OVERLAPPED));
}

NamedPipe& NamedPipe::operator=(NamedPipe&& other) noexcept {
    if (this != &other) {
        close();

        pipe_handle_ = other.pipe_handle_;
        overlapped_ = other.overlapped_;
        io_pending_ = other.io_pending_;
        is_server_ = other.is_server_;
        owns_handle_ = other.owns_handle_;
        read_buffer_ = std::move(other.read_buffer_);

        other.pipe_handle_ = INVALID_HANDLE_VALUE;
        other.io_pending_ = false;
        other.is_server_ = false;
        other.owns_handle_ = true;
        std::memset(&other.overlapped_, 0, sizeof(OVERLAPPED));
    }
    return *this;
}

// ============================================================================
// adopt() - 接管外部传入句柄
// ============================================================================

NamedPipe NamedPipe::adopt(uintptr_t handle_value) {
    auto h = reinterpret_cast<HANDLE>(handle_value);

    // 验证句柄有效性
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        LOG_ERROR("adopt() called with invalid handle value: " + std::to_string(handle_value));
        return NamedPipe(INVALID_HANDLE_VALUE, true);
    }

    // 尝试通过 PeekNamedPipe 验证句柄是否有效（不消耗数据）
    DWORD bytes_available = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &bytes_available, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            LOG_WARN("adopt() got broken pipe handle: " + std::to_string(handle_value));
        } else {
            LOG_WARN("adopt() handle validation failed: " + std::to_string(err));
        }
        // 仍然接管句柄，让上层决定如何处理
    }

    LOG_INFO("adopt()接管外部句柄: " + std::to_string(handle_value) +
             " (owns_handle=false)");
    return NamedPipe(h, false);  // owns_handle = false，析构时不关闭
}

// ============================================================================
// 服务端模式
// ============================================================================

    bool NamedPipe::createServer(const std::wstring& pipe_name, DWORD max_instances) {
    close();

    // ============================================================
    // 关键修复：设置安全属性，使句柄可继承
    // ============================================================
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;  // 允许子进程继承此句柄

    HANDLE hPipe = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        max_instances,
        4096,
        4096,
        0,
        &sa  // 传入安全属性，不再是 nullptr
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::string name_str(pipe_name.begin(), pipe_name.end());
        LOG_ERROR("CreateNamedPipeW failed for " + name_str + ": error " + std::to_string(err));
        return false;
    }

    pipe_handle_ = hPipe;
    is_server_ = true;
    owns_handle_ = true;
    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));

    std::string name_str(pipe_name.begin(), pipe_name.end());
    LOG_INFO("Named pipe server created: " + name_str + " (handle: " +
             std::to_string(reinterpret_cast<uintptr_t>(hPipe)) + ", inheritable=true)");
    return true;
}

PipeResult NamedPipe::waitForClient(DWORD timeout_ms) {
    if (pipe_handle_ == INVALID_HANDLE_VALUE || !is_server_) {
        LOG_WARN("waitForClient called on non-server pipe");
        return PipeResult::NOT_CONNECTED;
    }

    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
    overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    BOOL connected = ConnectNamedPipe(pipe_handle_, &overlapped_);
    DWORD err = GetLastError();

    if (connected) {
        CloseHandle(overlapped_.hEvent);
        std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
        LOG_INFO("Client connected to server pipe");
        return PipeResult::OK;
    }

    if (err == ERROR_IO_PENDING) {
        DWORD wait_result = WaitForSingleObject(overlapped_.hEvent, timeout_ms);
        CloseHandle(overlapped_.hEvent);
        std::memset(&overlapped_, 0, sizeof(OVERLAPPED));

        if (wait_result == WAIT_OBJECT_0) {
            LOG_INFO("Client connected to server pipe");
            return PipeResult::OK;
        }
        if (wait_result == WAIT_TIMEOUT) {
            CancelIoEx(pipe_handle_, &overlapped_);
            LOG_WARN("waitForClient timeout");
            return PipeResult::TIMEOUT;
        }
        LOG_ERROR("waitForClient failed: " + std::to_string(GetLastError()));
        return mapLastError(GetLastError());
    }

    if (err == ERROR_PIPE_CONNECTED) {
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
// 客户端模式（通过名称连接）
// ============================================================================

bool NamedPipe::connect(const std::wstring& pipe_name, DWORD timeout_ms) {
    close();

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
    owns_handle_ = true;
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
            return PipeResult::BROKEN;
        }

        buffer[bytes_read] = '\0';
        read_buffer_.append(buffer, bytes_read);

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

        if (read_buffer_.size() > 1024 * 1024) {
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
        if (err != ERROR_NOT_FOUND) {
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

        // 只有 owns_handle_ = true 时才关闭句柄
        if (owns_handle_) {
            LOG_INFO("close() closing handle: " + std::to_string(reinterpret_cast<uintptr_t>(pipe_handle_)));
            CloseHandle(pipe_handle_);
        } else {
            LOG_INFO("close() skipping CloseHandle (owns_handle=false)");
        }

        pipe_handle_ = INVALID_HANDLE_VALUE;
        is_server_ = false;
        owns_handle_ = true;
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
        // 其他错误可能是临时状态，乐观返回 true（上层会通过后续读写发现）
        return true;
    }

    return true;
}

HANDLE NamedPipe::release() {
    HANDLE h = pipe_handle_;
    pipe_handle_ = INVALID_HANDLE_VALUE;
    is_server_ = false;
    owns_handle_ = true;
    read_buffer_.clear();
    std::memset(&overlapped_, 0, sizeof(OVERLAPPED));
    return h;
}

// ============================================================================
// 辅助方法
// ============================================================================

bool NamedPipe::isHandleValid() const {
    return pipe_handle_ != INVALID_HANDLE_VALUE && pipe_handle_ != nullptr;
}

PipeResult NamedPipe::mapLastError(DWORD error) {
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