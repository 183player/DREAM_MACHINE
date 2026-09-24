// platform/dm_signal/signal_strings.h
#pragma once

// ================================================================
// dm_signal —— 枚举 ↔ 字符串映射
//
// 用途：
//   1. 正向（enum → string）：供 Logger 订阅者写日志、序列化写 JSON
//   2. 反向（string → enum）：供序列化解析 JSON
//
// 设计原则：
//   - 零依赖：仅 include signal_types.h + 标准库
//   - 正向返回 const char*：字面量无拷贝；与 constants.h::errorCodeToString 风格一致
//   - 反向返回 std::optional：失败用 nullopt；与 messages.cpp::parseXxx 风格一致
//   - 穷尽 switch + default 兜底：新增枚举值未更新映射时返回 "unknown"
//   - 大小写敏感：协议内部使用，不需容错
//
// 字符串值规则：
//   - 使用枚举值去掉前缀后的**小写**形式
//   - 例：SGT_OP_CONTEXT → "op_context"
//         SGL_INFO       → "info"
//         OPT_READ_FILE  → "read_file"
//         OPS_PREPARING  → "preparing"
//
// 与契约 #27 的关系：
//   "抛出系统用枚举，日志系统用字符串"——本文件即"单一映射层"。
//   程序自用路径（判定）用枚举；序列化 / 日志路径经本层转字符串。
// ================================================================

#include "signal_types.h"

#include <optional>
#include <string>

namespace dream_machine::signal {

// ================================================================
// 正向映射：枚举 → 字符串
//
// 保证返回非空 const char*。
// 未知枚举值返回 "unknown"（不返回 nullptr）。
// ================================================================

[[nodiscard]] const char* signal_type_to_string(SignalType type);
[[nodiscard]] const char* signal_level_to_string(SignalLevel level);
[[nodiscard]] const char* operation_type_to_string(OperationType type);
[[nodiscard]] const char* operation_state_to_string(OperationState state);

// ================================================================
// 反向映射：字符串 → 枚举
//
// 输入大小写敏感；不匹配时返回 std::nullopt。
// ================================================================

[[nodiscard]] std::optional<SignalType>     string_to_signal_type(const std::string& str);
[[nodiscard]] std::optional<SignalLevel>    string_to_signal_level(const std::string& str);
[[nodiscard]] std::optional<OperationType>  string_to_operation_type(const std::string& str);
[[nodiscard]] std::optional<OperationState> string_to_operation_state(const std::string& str);

} // namespace dream_machine::signal