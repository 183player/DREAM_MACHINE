// platform/dm_base/constants.cpp
#include "constants.h"

#include <sstream>
#include <iomanip>
#include <cctype>
#include <algorithm>
#include <functional>

namespace dream_machine::pipe_names {

// ================================================================
// 内部辅助：对管道名组件进行安全编码（内部链接）
// ================================================================
namespace {

bool isSafeChar(char ch) {
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_' ||
           ch == '-';
}

std::string sanitizeComponent(const std::string& input) {
    if (input.empty()) {
        return "sid_empty";
    }

    std::ostringstream result;
    bool has_alpha = false;

    for (unsigned char ch : input) {
        if (isSafeChar(static_cast<char>(ch))) {
            result << static_cast<char>(ch);
            if (std::isalpha(static_cast<unsigned char>(ch))) {
                has_alpha = true;
            }
        } else {
            result << '_' << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(ch);
        }
    }

    std::string sanitized = result.str();

    if (!sanitized.empty() && std::isdigit(static_cast<unsigned char>(sanitized[0]))) {
        sanitized = "sid_" + sanitized;
    }

    if (!has_alpha && !sanitized.empty()) {
        sanitized = "sid_" + sanitized;
    }

    constexpr size_t MAX_COMPONENT_LENGTH = 64;
    if (sanitized.length() > MAX_COMPONENT_LENGTH) {
        std::string truncated = sanitized.substr(0, 48);
        size_t hash_val = std::hash<std::string>{}(sanitized);
        std::ostringstream hash_oss;
        hash_oss << std::hex << (hash_val & 0xFFFFFFFF);
        sanitized = truncated + "_" + hash_oss.str();
        if (sanitized.length() > MAX_COMPONENT_LENGTH) {
            sanitized = sanitized.substr(0, MAX_COMPONENT_LENGTH);
        }
    }

    return sanitized;
}

} // namespace

// ================================================================
// 公开接口实现
// ================================================================

std::string monitor_core(const std::string& session_id) {
    std::string safe_id = sanitizeComponent(session_id);
    return R"(\\.\pipe\DreamMachine_Monitor_Core_)" + safe_id;
}

std::string executor_core() {
    return R"(\\.\pipe\DreamMachine_Executor_Core)";
}

} // namespace dream_machine::pipe_names