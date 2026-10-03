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
// ================================================================

// UTF-8 → UTF-16
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
// ================================================================

// 获取应用根目录（可执行文件所在目录，绝对路径，UTF-8）
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
inline std::string pathFromRoot(const std::string& relative) {
    std::string root = getAppRootPath();
    if (root.empty()) {
        return {};
    }
    if (relative.empty()) {
        return root;
    }

    if (root.back() == '\\' || root.back() == '/') {
        root.pop_back();
    }
    if (relative.front() == '\\' || relative.front() == '/') {
        return root + relative;
    }
    return root + "\\" + relative;
}

// ================================================================
// getProjectRootPath：项目根目录推断（A.5.2）
//
// 用途：从 exe 所在目录向上逐级搜索，找到包含项目标志文件的目录。
//       不依赖 CWD，不依赖构建目录名（bin/ / out/ / build/ 均可）。
//
// 标志文件：src/default_plugin/manifest.json
//   - 由源码目录结构约定保证存在（详见文档7 §五）
//   - 若未来目录结构调整，同步更新本函数
//
// 搜索策略：
//   1. 从 exe 所在目录（GetModuleFileNameW 结果）开始
//   2. 每级检查 <当前>/src/default_plugin/manifest.json 是否存在
//   3. 存在 → 返回当前目录（UTF-8 绝对路径）
//   4. 不存在 → 向上一级；最多向上 MAX_UP_LEVELS 级
//   5. 全部失败 → 返回空字符串
//
// 适用场景：
//   - dev 模式下从源码目录读取资源（DM_DEV_MODE=1）
//   - 占位窗口等源文件路径解析
//   - theme_manager 系统模板路径解析
//
// 返回值：UTF-8 绝对路径；失败返回空字符串
//
// 依据：详见文档13 §1.3 C5、文档8 §八（不依赖 CWD）
// ================================================================
inline std::string getProjectRootPath() {
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        LOG_ERROR("getProjectRootPath: GetModuleFileNameW failed: " +
                  std::to_string(GetLastError()));
        return {};
    }

    std::wstring current(buffer, len);
    const size_t pos = current.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        LOG_ERROR("getProjectRootPath: no path separator in exe path");
        return {};
    }
    current = current.substr(0, pos);   // exe 所在目录

    constexpr int MAX_UP_LEVELS = 10;
    constexpr const wchar_t* MARKER = L"src\\default_plugin\\manifest.json";

    for (int level = 0; level < MAX_UP_LEVELS; ++level) {
        std::wstring candidate = current + L"\\" + MARKER;
        DWORD attrs = GetFileAttributesW(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return wideToUtf8(current);
        }

        const size_t up_pos = current.find_last_of(L"\\/");
        if (up_pos == std::wstring::npos) {
            break;
        }
        current = current.substr(0, up_pos);
    }

    LOG_WARN("getProjectRootPath: project root marker not found within " +
             std::to_string(MAX_UP_LEVELS) + " levels above exe");
    return {};
}

// ================================================================
// request_id 统一生成器
// ================================================================
inline int64_t nextRequestId() {
    static std::atomic<int64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace common
} // namespace dream_machine