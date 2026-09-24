// platform/dm_base/common_utils.h
#pragma once

#include "logger.h"
#include <string>
#include <cstdint>
#include <atomic>
#include <windows.h>
#include <tlhelp32.h>

namespace dream_machine {
namespace common {

// ================================================================
// 解析命令行参数（键值对，如 --key value）
// ================================================================
inline std::string getArgValue(int argc, char* argv[], const std::string& key) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == key) {
            return argv[i + 1];
        }
    }
    return {};
}

// ================================================================
// 验证父进程 PID，防止进程被独立启动
// ================================================================
inline bool verifyParentPid(DWORD expected_parent_pid) {
    if (expected_parent_pid == 0) {
        LOG_ERROR("Missing --parent-pid argument, refusing to run standalone");
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateToolhelp32Snapshot failed: error " + std::to_string(GetLastError()));
        return false;
    }

    PROCESSENTRY32W pe = {sizeof(PROCESSENTRY32W)};
    DWORD current_pid = GetCurrentProcessId();
    DWORD real_parent_pid = 0;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == current_pid) {
                real_parent_pid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);

    if (real_parent_pid == 0) {
        LOG_ERROR("Failed to determine real parent PID");
        return false;
    }

    if (real_parent_pid != expected_parent_pid) {
        LOG_ERROR("Parent PID mismatch: expected " + std::to_string(expected_parent_pid) +
                  ", actual " + std::to_string(real_parent_pid) + ", refusing to run");
        return false;
    }

    LOG_INFO("Parent PID verification passed (PID: " + std::to_string(real_parent_pid) + ")");
    return true;
}

// ================================================================
// 编码转换：UTF-8 ↔ UTF-16
//
// 用途：Win32 API 使用 UTF-16（wchar_t 在 Windows 上为 2 字节），
//       而项目内部字符串统一使用 UTF-8（std::string）。
//       在调用 CreateNamedPipeW / CreateFileW 等宽字符 API 前，
//       需要先做转换。
//
// 保守设计：
//   - 仅提供两个函数，不引入其他编码相关能力
//   - 转换失败返回空字符串（调用方按"无效输入"处理）
//   - 不使用 std::filesystem::path 互转（当前无此需求）
//   - 不做编码检测（当前所有输入均为有效 UTF-8）
//
// 注：项目此前的 "std::wstring(str.begin(), str.end())" 是逐字节扩展，
//     仅对 ASCII 有效。本 helper 保证非 ASCII（中文路径等）正确转换。
// ================================================================

// UTF-8 → UTF-16
// 空输入返回空字符串（非错误）
inline std::wstring utf8ToWide(const std::string& utf8_str) {
    if (utf8_str.empty()) {
        return std::wstring();
    }

    const int wide_len = MultiByteToWideChar(
        CP_UTF8, 0,
        utf8_str.data(), static_cast<int>(utf8_str.size()),
        nullptr, 0);
    if (wide_len <= 0) {
        return std::wstring();
    }

    std::wstring result(static_cast<size_t>(wide_len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, 0,
        utf8_str.data(), static_cast<int>(utf8_str.size()),
        result.data(), wide_len);
    if (written != wide_len) {
        return std::wstring();
    }
    return result;
}

// UTF-16 → UTF-8
// 空输入返回空字符串（非错误）
inline std::string wideToUtf8(const std::wstring& wide_str) {
    if (wide_str.empty()) {
        return std::string();
    }

    const int utf8_len = WideCharToMultiByte(
        CP_UTF8, 0,
        wide_str.data(), static_cast<int>(wide_str.size()),
        nullptr, 0,
        nullptr, nullptr);
    if (utf8_len <= 0) {
        return std::string();
    }

    std::string result(static_cast<size_t>(utf8_len), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, 0,
        wide_str.data(), static_cast<int>(wide_str.size()),
        result.data(), utf8_len,
        nullptr, nullptr);
    if (written != utf8_len) {
        return std::string();
    }
    return result;
}

// ================================================================
// 应用根目录与路径解析
//
// 背景：所有运行时资源（logs/ / plugins/ / data/）应位于可执行文件
//       同级目录，保证 bin/ 整体挪走后路径仍正确。
//       不能依赖当前工作目录（CWD），因为从其他目录启动时 CWD 会变。
//
// 实现：使用 Win32 GetModuleFileNameW 获取 exe 绝对路径，不依赖 Qt
//       或 CWD。任何时机（含 QApplication 创建前）调用都安全。
//
// 保守设计：
//   - 只提供两个函数（根路径 + 相对路径拼接）
//   - 失败返回空字符串，由调用方处理
//   - 不做规范化（如 .. 折叠）—— 当前无此需求
// ================================================================

// 获取应用根目录（可执行文件所在目录，绝对路径，UTF-8）
// 失败时返回空字符串
inline std::string getAppRootPath() {
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        LOG_ERROR("GetModuleFileNameW failed: " + std::to_string(GetLastError()));
        return {};
    }

    std::wstring full_path(buffer, len);
    const size_t pos = full_path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        LOG_ERROR("No path separator in exe path");
        return {};
    }
    return wideToUtf8(full_path.substr(0, pos));
}

// 拼接应用根目录下的相对路径
// relative 为空时返回根目录本身
// 根目录获取失败时返回空字符串
inline std::string pathFromRoot(const std::string& relative) {
    std::string root = getAppRootPath();
    if (root.empty()) {
        return {};
    }
    if (relative.empty()) {
        return root;
    }

    // 规范化分隔符（避免出现 "\\" 与 "/" 混用）
    if (root.back() == '\\' || root.back() == '/') {
        root.pop_back();
    }
    if (relative.front() == '\\' || relative.front() == '/') {
        return root + relative;
    }
    return root + "\\" + relative;
}

// ================================================================
// request_id 统一生成器
//
// 用途：为"请求-响应关联"场景提供进程内唯一 ID。例如：
//   - 未来的 REQUEST_ENGINE 会话创建请求需关联响应
//
// 保守设计：
//   - 不强制使用：现有 FULL_SYNC 保留自己的生成方式
//   - 单点定义：所有新调用方从此处获取，避免各进程各自造号
//   - 单调递增：fetch_add 保证唯一性
//   - 跨 TU 唯一：inline 函数中的 static 局部变量在 C++ 中唯一
//
// 起始值 = 1（0 保留为"未设置"语义，与现有 FullSyncRequestMessage 一致）
//
// 线程安全：使用 std::atomic，即使未来引入业务内聚线程也无需修改。
// ================================================================
inline int64_t nextRequestId() {
    static std::atomic<int64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace common
} // namespace dream_machine