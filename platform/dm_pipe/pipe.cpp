// platform/dm_pipe/pipe.cpp
#include "pipe.h"

#include "logger.h"

#include <cstring>
#include <limits>
#include <sddl.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")

namespace dream_machine {

// ================================================================
// 安全描述符辅助
// ================================================================
SECURITY_ATTRIBUTES* NamedPipe::createSecureSecurityAttributes(
    SECURITY_ATTRIBUTES& sa_out,
    std::unique_ptr<void, decltype(&LocalFree)>& desc_out) {

    const wchar_t* sddl = L"D:(A;;GA;;;IU)(A;;GA;;;SY)";

    PSECURITY_DESCRIPTOR p_sec_desc = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &p_sec_desc, nullptr)) {
        LOG_ERROR("ConvertStringSecurityDescriptorToSecurityDescriptorW failed: " +
                  std::to_string(GetLastError()));
        return nullptr;
    }

    desc_out.reset(p_sec_desc);

    sa_out.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa_out.lpSecurityDescriptor = p_sec_desc;
    sa_out.bInheritHandle = TRUE;

    return &sa_out;
}

// ================================================================
// 构造函数 / 析构函数
// ================================================================

NamedPipe::NamedPipe() {
    buffer_.resize(BUFFER_SIZE);
}

NamedPipe::~NamedPipe() {
    close();
}

NamedPipe::NamedPipe(NamedPipe&& other) noexcept
    : handle_(other.handle_)
    , is_server_(other.is_server_)
    , security_desc_(std::move(other.security_desc_))
    , buffer_(std::move(other.buffer_))
    , buffer_pos_(other.buffer_pos_)
    , buffer_valid_(other.buffer_valid_) {
    other.handle_ = INVALID_HANDLE_VALUE;
    other.is_server_ = false;
    other.buffer_pos_ = 0;
    other.buffer_valid_ = 0;
}

NamedPipe& NamedPipe::operator=(NamedPipe&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        is_server_ = other.is_server_;
        security_desc_ = std::move(other.security_desc_);
        buffer_ = std::move(other.buffer_);
        buffer_pos_ = other.buffer_pos_;
        buffer_valid_ = other.buffer_valid_;
        other.handle_ = INVALID_HANDLE_VALUE;
        other.is_server_ = false;
        other.buffer_pos_ = 0;
        other.buffer_valid_ = 0;
    }
    return *this;
}

// ================================================================
// 缓冲区管理
// ================================================================

void NamedPipe::resetBuffer() {
    buffer_pos_ = 0;
    buffer_valid_ = 0;
}

// ================================================================
// 从缓冲区提取一行
// ================================================================

bool NamedPipe::extractLineFromBuffer(std::string& out_line) {
    out_line.clear();

    if (buffer_pos_ >= buffer_valid_) {
        return false;
    }

    for (size_t i = buffer_pos_; i < buffer_valid_; ++i) {
        if (buffer_[i] == '\n') {
            size_t len = i - buffer_pos_;
            out_line.assign(buffer_.data() + buffer_pos_, len);
            buffer_pos_ = i + 1;

            if (buffer_pos_ >= buffer_valid_) {
                buffer_pos_ = 0;
                buffer_valid_ = 0;
            }

            return true;
        }
    }

    return false;
}

// ================================================================
// 填充缓冲区（批量读取）
// ================================================================

PipeResult NamedPipe::fillBuffer(DWORD timeout_ms) {
    if (handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::PIPE_NOT_CONNECTED;
    }

    if (isBroken()) {
        return PipeResult::PIPE_BROKEN;
    }

    if (buffer_valid_ >= BUFFER_SIZE) {
        LOG_ERROR("Buffer full without newline, message too large");
        return PipeResult::PIPE_ERROR;
    }

    if (buffer_pos_ > 0 && buffer_valid_ > buffer_pos_) {
        size_t remaining = buffer_valid_ - buffer_pos_;
        std::memmove(buffer_.data(), buffer_.data() + buffer_pos_, remaining);
        buffer_valid_ = remaining;
        buffer_pos_ = 0;
    } else if (buffer_pos_ > 0) {
        buffer_pos_ = 0;
        buffer_valid_ = 0;
    }

    size_t space_left = BUFFER_SIZE - buffer_valid_;
    if (space_left == 0) {
        LOG_ERROR("Buffer full, no newline found");
        return PipeResult::PIPE_ERROR;
    }

    DWORD start_time = GetTickCount();

    while (true) {
        if (GetTickCount() - start_time >= timeout_ms) {
            return PipeResult::PIPE_TIMEOUT;
        }

        if (isBroken()) {
            return PipeResult::PIPE_BROKEN;
        }

        DWORD bytes_available = 0;
        if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &bytes_available, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                return PipeResult::PIPE_BROKEN;
            }
            Sleep(10);
            continue;
        }

        if (bytes_available == 0) {
            Sleep(10);
            continue;
        }

        DWORD bytes_to_read = static_cast<DWORD>(std::min<size_t>(bytes_available, space_left));
        DWORD bytes_read = 0;

        if (!ReadFile(handle_, buffer_.data() + buffer_valid_, bytes_to_read,
                      &bytes_read, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                return PipeResult::PIPE_BROKEN;
            }
            LOG_ERROR("ReadFile failed: error " + std::to_string(err));
            return PipeResult::PIPE_ERROR;
        }

        if (bytes_read > 0) {
            buffer_valid_ += bytes_read;
            space_left -= bytes_read;
            LOG_INFO("Read " + std::to_string(bytes_read) + " bytes into buffer, total: " +
                     std::to_string(buffer_valid_));
            return PipeResult::PIPE_OK;
        }

        Sleep(10);
    }
}

// ================================================================
// 服务端 API
// ================================================================

bool NamedPipe::createServer(const std::wstring& pipe_name,
                             int max_instances,
                             bool secure) {
    close();
    resetBuffer();

    if (pipe_name.empty() || max_instances < 1) {
        LOG_ERROR("createServer: invalid parameters");
        return false;
    }

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    SECURITY_ATTRIBUTES* p_sa = nullptr;

    if (secure) {
        p_sa = createSecureSecurityAttributes(sa, security_desc_);
        if (!p_sa) {
            LOG_ERROR("createServer: failed to create security descriptor, continuing without security");
        }
    }

    HANDLE h = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        static_cast<DWORD>(max_instances),
        4096,
        4096,
        0,
        p_sa ? p_sa : &sa
    );

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::string name(pipe_name.begin(), pipe_name.end());
        LOG_ERROR("CreateNamedPipeW failed for " + name + ": error " + std::to_string(err));
        return false;
    }

    handle_ = h;
    is_server_ = true;
    resetBuffer();

    std::string name(pipe_name.begin(), pipe_name.end());
    LOG_INFO("Named pipe server created: " + name + " (handle: " +
             std::to_string(reinterpret_cast<uintptr_t>(handle_)) + ", secure=" +
             (secure ? "true" : "false") + ")");
    return true;
}

PipeResult NamedPipe::waitForClient(DWORD timeout_ms) const {
    if (!is_server_ || handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::PIPE_NOT_CONNECTED;
    }

    BOOL connected = ConnectNamedPipe(handle_, nullptr);
    if (connected) {
        LOG_INFO("Client connected to server pipe");
        return PipeResult::PIPE_OK;
    }

    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
        LOG_INFO("Client already connected to server pipe");
        return PipeResult::PIPE_OK;
    }

    if (err == ERROR_IO_PENDING) {
        OVERLAPPED ov = {0};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            LOG_ERROR("waitForClient: CreateEvent failed");
            return PipeResult::PIPE_ERROR;
        }

        HANDLE h_event = ov.hEvent;
        bool completed = false;
        DWORD wait_result = WaitForSingleObject(h_event, timeout_ms);
        if (wait_result == WAIT_OBJECT_0) {
            DWORD bytes = 0;
            if (GetOverlappedResult(handle_, &ov, &bytes, FALSE)) {
                completed = true;
            } else {
                err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED) {
                    completed = true;
                }
            }
        } else if (wait_result == WAIT_TIMEOUT) {
            CancelIoEx(handle_, &ov);
            CloseHandle(h_event);
            LOG_WARN("waitForClient timeout after " + std::to_string(timeout_ms) + "ms");
            return PipeResult::PIPE_TIMEOUT;
        }

        CloseHandle(h_event);
        if (completed) {
            LOG_INFO("Client connected to server pipe");
            return PipeResult::PIPE_OK;
        }

        LOG_ERROR("waitForClient: GetOverlappedResult failed: " + std::to_string(GetLastError()));
        return PipeResult::PIPE_ERROR;
    }

    LOG_ERROR("ConnectNamedPipe failed: error " + std::to_string(err));
    return PipeResult::PIPE_ERROR;
}

// ================================================================
// 客户端 API
// ================================================================

bool NamedPipe::connect(const std::wstring& pipe_name, DWORD timeout_ms) {
    close();
    resetBuffer();

    if (pipe_name.empty()) {
        LOG_ERROR("connect: empty pipe name");
        return false;
    }

    HANDLE h = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (h != INVALID_HANDLE_VALUE) {
        handle_ = h;
        is_server_ = false;
        resetBuffer();
        std::string name(pipe_name.begin(), pipe_name.end());
        LOG_INFO("Connected to named pipe: " + name);
        return true;
    }

    DWORD err = GetLastError();
    if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
        std::string name(pipe_name.begin(), pipe_name.end());
        LOG_ERROR("CreateFileW failed for " + name + ": error " + std::to_string(err));
        return false;
    }

    if (timeout_ms == 0) {
        return false;
    }

    DWORD start_time = GetTickCount();
    while (true) {
        if (WaitNamedPipeW(pipe_name.c_str(), std::min<DWORD>(timeout_ms, 200)) != 0) {
            h = CreateFileW(
                pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (h != INVALID_HANDLE_VALUE) {
                handle_ = h;
                is_server_ = false;
                resetBuffer();
                std::string name(pipe_name.begin(), pipe_name.end());
                LOG_INFO("Connected to named pipe: " + name);
                return true;
            }

            DWORD err2 = GetLastError();
            if (err2 != ERROR_PIPE_BUSY) {
                std::string name(pipe_name.begin(), pipe_name.end());
                LOG_ERROR("CreateFileW retry failed for " + name + ": error " + std::to_string(err2));
                return false;
            }
        }

        DWORD elapsed = GetTickCount() - start_time;
        if (elapsed >= timeout_ms) {
            std::string name(pipe_name.begin(), pipe_name.end());
            LOG_WARN("connect timeout for " + name + " after " + std::to_string(timeout_ms) + "ms");
            return false;
        }

        Sleep(50);
    }
}

bool NamedPipe::connectWithRetry(const std::wstring& pipe_name,
                                 int max_retries,
                                 int delay_ms) {
    if (max_retries < 0) {
        max_retries = 0;
    }

    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        if (attempt > 0) {
            std::string name(pipe_name.begin(), pipe_name.end());
            LOG_INFO("Retry " + std::to_string(attempt) + "/" + std::to_string(max_retries) +
                     " connecting to: " + name);
            Sleep(static_cast<DWORD>(delay_ms));
        }

        if (connect(pipe_name, 2000)) {
            resetBuffer();
            return true;
        }
    }

    std::string name(pipe_name.begin(), pipe_name.end());
    LOG_ERROR("Failed to connect to " + name + " after " +
              std::to_string(max_retries + 1) + " attempts");
    return false;
}

// ================================================================
// 写入
// ================================================================

PipeResult NamedPipe::writeLine(const std::string& message) const {
    if (handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::PIPE_NOT_CONNECTED;
    }

    std::string line = message + "\n";
    DWORD bytes_written = 0;

    if (!WriteFile(handle_, line.c_str(),
                   static_cast<DWORD>(line.size()),
                   &bytes_written, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
            return PipeResult::PIPE_BROKEN;
        }
        LOG_ERROR("WriteFile failed: error " + std::to_string(err));
        return PipeResult::PIPE_ERROR;
    }

    if (bytes_written != line.size()) {
        return PipeResult::PIPE_ERROR;
    }

    return PipeResult::PIPE_OK;
}

// ================================================================
// 读取（保留逐字节方式，用于兼容）
// ================================================================

PipeResult NamedPipe::readLine(std::string& out_message, DWORD timeout_ms) {
    if (handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::PIPE_NOT_CONNECTED;
    }

    if (isBroken()) {
        return PipeResult::PIPE_BROKEN;
    }

    return readLineInternal(out_message, timeout_ms);
}

PipeResult NamedPipe::readLineInternal(std::string& out_message, DWORD timeout_ms) {
    out_message.clear();

    DWORD start_time = GetTickCount();
    char ch = 0;

    while (true) {
        if (GetTickCount() - start_time >= timeout_ms) {
            return PipeResult::PIPE_TIMEOUT;
        }

        DWORD bytes_available = 0;
        if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &bytes_available, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                return PipeResult::PIPE_BROKEN;
            }
            return PipeResult::PIPE_ERROR;
        }

        if (bytes_available == 0) {
            if (isBroken()) {
                return PipeResult::PIPE_BROKEN;
            }
            Sleep(5);
            continue;
        }

        DWORD bytes_read = 0;
        if (!ReadFile(handle_, &ch, 1, &bytes_read, nullptr) || bytes_read != 1) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                return PipeResult::PIPE_BROKEN;
            }
            return PipeResult::PIPE_ERROR;
        }

        if (ch == '\n') {
            return PipeResult::PIPE_OK;
        }

        if (ch != '\r') {
            out_message.push_back(ch);
        }

        if (out_message.size() > 1024 * 1024) {
            LOG_ERROR("readLine: message exceeds 1MB limit");
            return PipeResult::PIPE_ERROR;
        }
    }
}

// ================================================================
// 缓冲读取（高效接口）
// ================================================================

PipeResult NamedPipe::readLineBuffered(std::string& out_message, DWORD timeout_ms) {
    out_message.clear();

    if (handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::PIPE_NOT_CONNECTED;
    }

    if (isBroken()) {
        return PipeResult::PIPE_BROKEN;
    }

    if (extractLineFromBuffer(out_message)) {
        return PipeResult::PIPE_OK;
    }

    DWORD start_time = GetTickCount();

    while (true) {
        DWORD elapsed = GetTickCount() - start_time;
        if (elapsed >= timeout_ms) {
            return PipeResult::PIPE_TIMEOUT;
        }

        if (isBroken()) {
            return PipeResult::PIPE_BROKEN;
        }

        DWORD remaining = timeout_ms - elapsed;
        PipeResult fill_result = fillBuffer(remaining);

        if (fill_result == PipeResult::PIPE_BROKEN) {
            return PipeResult::PIPE_BROKEN;
        }
        if (fill_result == PipeResult::PIPE_TIMEOUT) {
            return PipeResult::PIPE_TIMEOUT;
        }
        if (fill_result != PipeResult::PIPE_OK) {
            Sleep(5);
            continue;
        }

        if (extractLineFromBuffer(out_message)) {
            return PipeResult::PIPE_OK;
        }

        if (buffer_valid_ >= BUFFER_SIZE) {
            LOG_ERROR("Buffer full without newline, message too large");
            return PipeResult::PIPE_ERROR;
        }

        Sleep(5);
    }
}

// ================================================================
// 其他 API
// ================================================================

PipeResult NamedPipe::peekAvailable(DWORD& out_bytes) const {
    out_bytes = 0;

    if (handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::PIPE_NOT_CONNECTED;
    }

    if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &out_bytes, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
            return PipeResult::PIPE_BROKEN;
        }
        return PipeResult::PIPE_ERROR;
    }

    return PipeResult::PIPE_OK;
}

bool NamedPipe::isValid() const {
    return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
}

bool NamedPipe::isConnected() const {
    if (!isValid()) {
        return false;
    }

    DWORD bytes_available = 0;
    if (PeekNamedPipe(handle_, nullptr, 0, nullptr, &bytes_available, nullptr)) {
        return true;
    }

    DWORD err = GetLastError();
    return err != ERROR_BROKEN_PIPE && err != ERROR_NO_DATA;
}

bool NamedPipe::isBroken() const {
    if (!isValid()) {
        return true;
    }

    DWORD bytes_available = 0;
    if (PeekNamedPipe(handle_, nullptr, 0, nullptr, &bytes_available, nullptr)) {
        return false;
    }

    DWORD err = GetLastError();
    return (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA);
}

void NamedPipe::close() {
    resetBuffer();

    if (handle_ != INVALID_HANDLE_VALUE) {
        std::string msg = "close() closing handle: " +
                          std::to_string(reinterpret_cast<uintptr_t>(handle_));
        LOG_INFO(msg);

        if (is_server_) {
            FlushFileBuffers(handle_);
            DisconnectNamedPipe(handle_);
        }

        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        is_server_ = false;
    }

    security_desc_.reset();
}

} // namespace dream_machine