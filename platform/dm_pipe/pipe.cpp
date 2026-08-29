// platform/dm_pipe/pipe.cpp
#include "pipe.h"

#include "logger.h"

#include <cstring>
#include <limits>
#include <sddl.h>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace dream_machine {

// ================================================================
// RAII 辅助：自动关闭句柄
// ================================================================
namespace {
struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};
} // namespace

// ================================================================
// 构造函数 / 析构函数
// ================================================================
NamedPipe::~NamedPipe() {
    close();
}

NamedPipe::NamedPipe(NamedPipe&& other) noexcept
    : handle_(other.handle_)
    , is_server_(other.is_server_)
    , security_desc_(std::move(other.security_desc_)) {
    other.handle_ = INVALID_HANDLE_VALUE;
    other.is_server_ = false;
}

NamedPipe& NamedPipe::operator=(NamedPipe&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        is_server_ = other.is_server_;
        security_desc_ = std::move(other.security_desc_);
        other.handle_ = INVALID_HANDLE_VALUE;
        other.is_server_ = false;
    }
    return *this;
}

// ================================================================
// 安全描述符创建（仅当前用户可访问）
// ================================================================
SECURITY_ATTRIBUTES* NamedPipe::createSecureSecurityAttributes(
    SECURITY_ATTRIBUTES& sa_out,
    std::unique_ptr<void, decltype(&LocalFree)>& desc_out) {

    // SDDL: 仅交互式用户 (IU) 和本地系统 (SY) 完全访问
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
// 服务端 API
// ================================================================
bool NamedPipe::createServer(const std::wstring& pipe_name,
                             int max_instances,
                             bool secure) {
    close();

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

    // 使用 if 初始化，采纳 modernize 建议
    if (BOOL connected = ConnectNamedPipe(handle_, nullptr)) {
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

// ================================================================
// 连接辅助工具（子进程专用）
// ================================================================
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
            return true;
        }
    }

    std::string name(pipe_name.begin(), pipe_name.end());
    LOG_ERROR("Failed to connect to " + name + " after " +
              std::to_string(max_retries + 1) + " attempts");
    return false;
}

// ================================================================
// 读写 API
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

PipeResult NamedPipe::readLine(std::string& out_message, DWORD timeout_ms) const
{
    if (handle_ == INVALID_HANDLE_VALUE) {
        return PipeResult::PIPE_NOT_CONNECTED;
    }

    if (isBroken()) {
        return PipeResult::PIPE_BROKEN;
    }

    return readLineInternal(out_message, timeout_ms);
}

PipeResult NamedPipe::readLineInternal(std::string& out_message, DWORD timeout_ms) const
{
    out_message.clear();

    DWORD start_time = GetTickCount();
    char ch = 0;

    while (true) {
        // 检查超时
        if (GetTickCount() - start_time >= timeout_ms) {
            return PipeResult::PIPE_TIMEOUT;
        }

        // 检查管道是否有数据
        DWORD bytes_available = 0;
        if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &bytes_available, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                return PipeResult::PIPE_BROKEN;
            }
            return PipeResult::PIPE_ERROR;
        }

        if (bytes_available == 0) {
            // 检查管道是否断开
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

        // 防止消息过大
        if (out_message.size() > 1024 * 1024) {
            LOG_ERROR("readLine: message exceeds 1MB limit");
            return PipeResult::PIPE_ERROR;
        }
    }
}

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

// ================================================================
// 状态检测 API
// ================================================================
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

// ================================================================
// 关闭
// ================================================================
void NamedPipe::close() {
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