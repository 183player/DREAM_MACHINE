// platform/dm_signal/signal_strings.cpp
#include "signal_strings.h"

namespace dream_machine::signal {

// ================================================================
// 匿名命名空间：映射表
//
// 设计说明：
//   - 正向和反向映射共用同一份表——修改一处，双向生效
//   - 表是 constexpr 数组——编译期常量，零初始化开销
//   - 每个枚举独立一张表——互不干扰
//
// 维护约定（重要）：
//   新增枚举值时，必须同时更新对应表——否则双向都会漏该值。
//   编译期无法自动检测（表驱动牺牲了 -Wswitch 提示）。
// ================================================================
namespace {

// ---- SignalType ----
struct SignalTypeEntry {
    SignalType  value;
    const char* name;
};

constexpr SignalTypeEntry SIGNAL_TYPES[] = {
    { SignalType::SGT_OP_CONTEXT,            "op_context"            },
    { SignalType::SGT_ERROR_NOTIFY,          "error_notify"          },
    { SignalType::SGT_ENGINE_DIED,           "engine_died"           },
    { SignalType::SGT_SESSION_STATE_CHANGED, "session_state_changed" },
    { SignalType::SGT_PROCESS_STATE_CHANGED, "process_state_changed" },
    { SignalType::SGT_STARTUP,               "startup"               },
    { SignalType::SGT_SHUTDOWN_REQUESTED,    "shutdown_requested"    },
    { SignalType::SGT_CUSTOM,                "custom"                },
};

// ---- SignalLevel ----
struct SignalLevelEntry {
    SignalLevel value;
    const char* name;
};

constexpr SignalLevelEntry SIGNAL_LEVELS[] = {
    { SignalLevel::SGL_DEBUG, "debug" },
    { SignalLevel::SGL_INFO,  "info"  },
    { SignalLevel::SGL_WARN,  "warn"  },
    { SignalLevel::SGL_ERROR, "error" },
    { SignalLevel::SGL_FATAL, "fatal" },
};

// ---- OperationType ----
struct OperationTypeEntry {
    OperationType value;
    const char*   name;
};

constexpr OperationTypeEntry OPERATION_TYPES[] = {
    { OperationType::OPT_READ_FILE,       "read_file"       },
    { OperationType::OPT_WRITE_FILE,      "write_file"      },
    { OperationType::OPT_DELETE_FILE,     "delete_file"     },
    { OperationType::OPT_LIST_DIR,        "list_dir"        },
    { OperationType::OPT_ATOMIC_WRITE_L2, "atomic_write_l2" },
    { OperationType::OPT_CUSTOM,          "custom"          },
};

// ---- OperationState ----
struct OperationStateEntry {
    OperationState value;
    const char*    name;
};

constexpr OperationStateEntry OPERATION_STATES[] = {
    { OperationState::OPS_IDLE,      "idle"      },
    { OperationState::OPS_PREPARING, "preparing" },
    { OperationState::OPS_RUNNING,   "running"   },
    { OperationState::OPS_FINISHING, "finishing" },
    { OperationState::OPS_FAILED,    "failed"    },
};

} // namespace

// ================================================================
// 正向映射：枚举 → 字符串
//
// 遍历对应表；匹配返回表内的字面量；未匹配返回 "unknown"。
// 返回的是编译期字符串字面量，调用方不应修改。
// ================================================================

const char* signal_type_to_string(SignalType type) {
    for (const auto& entry : SIGNAL_TYPES) {
        if (entry.value == type) {
            return entry.name;
        }
    }
    return "unknown";
}

const char* signal_level_to_string(SignalLevel level) {
    for (const auto& entry : SIGNAL_LEVELS) {
        if (entry.value == level) {
            return entry.name;
        }
    }
    return "unknown";
}

const char* operation_type_to_string(OperationType type) {
    for (const auto& entry : OPERATION_TYPES) {
        if (entry.value == type) {
            return entry.name;
        }
    }
    return "unknown";
}

const char* operation_state_to_string(OperationState state) {
    for (const auto& entry : OPERATION_STATES) {
        if (entry.value == state) {
            return entry.name;
        }
    }
    return "unknown";
}

// ================================================================
// 反向映射：字符串 → 枚举
//
// 遍历对应表；匹配返回枚举值；未匹配返回 std::nullopt。
// 大小写敏感——协议内部使用。
// ================================================================

std::optional<SignalType> string_to_signal_type(const std::string& str) {
    for (const auto& entry : SIGNAL_TYPES) {
        if (str == entry.name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::optional<SignalLevel> string_to_signal_level(const std::string& str) {
    for (const auto& entry : SIGNAL_LEVELS) {
        if (str == entry.name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::optional<OperationType> string_to_operation_type(const std::string& str) {
    for (const auto& entry : OPERATION_TYPES) {
        if (str == entry.name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::optional<OperationState> string_to_operation_state(const std::string& str) {
    for (const auto& entry : OPERATION_STATES) {
        if (str == entry.name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

} // namespace dream_machine::signal