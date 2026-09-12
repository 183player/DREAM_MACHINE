// platform/dm_base/message_router.h
#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <utility>

namespace dream_machine {

// ================================================================
// MessageRouter：消息类型 → handler 的分发表
//
// 阶段 1.7 C1 保守设计：
//   - 只做 type_str → handler 的映射，不做其他
//   - handler 签名为 void(payload, context)：context 是 void*，
//     由调用方决定其含义（通常是 NamedPipe*）
//   - dispatch 返回 bool：true = 已处理，false = 未注册
//     调用方据此决定是否回退到 if-else 链（保守 fallback 机制）
//   - 无中间件、无优先级、无异步、无返回值扩展
//
// 使用模式：
//
//   MessageRouter router;
//   router.register_handler(msg_types::PLUGIN_IMPORT,
//       [](const std::string& payload, void* ctx) {
//           auto* pipe = static_cast<NamedPipe*>(ctx);
//           // 处理 payload
//       });
//
//   // 在 onPipeReadable 中：
//   if (router.dispatch(type_str, payload, &pipe)) {
//       return;  // 已处理
//   }
//   // fallback: 现有 if-else 链
//
// 回退方式：
//   若未来发现分发表不合适，删除本文件 + 各进程的 register_handler 调用
//   即可恢复纯 if-else 实现。
// ================================================================

class MessageRouter {
public:
    // handler 签名：
    //   payload = 解析后的 payload 字符串
    //   context = 调用方提供的上下文（通常 static_cast<NamedPipe*>(ctx)）
    using Handler = std::function<void(const std::string& payload, void* context)>;

    // 注册 handler。
    // 同名 type_str 重复注册会覆盖（后注册者生效）。
    // 空 type_str 或空 handler 会被静默忽略（防止误注册）。
    void register_handler(const std::string& type_str, Handler handler) {
        if (type_str.empty() || !handler) {
            return;
        }
        handlers_[type_str] = std::move(handler);
    }

    // 分发消息。
    //   返回 true  表示找到并执行了 handler；
    //   返回 false 表示未注册，调用方应自行处理（fallback）。
    bool dispatch(const std::string& type_str,
                  const std::string& payload,
                  void* context) const {
        auto it = handlers_.find(type_str);
        if (it == handlers_.end()) {
            return false;
        }
        it->second(payload, context);
        return true;
    }

    // 查询是否已注册（供调用方预判，一般不需要）。
    bool has_handler(const std::string& type_str) const {
        return handlers_.find(type_str) != handlers_.end();
    }

private:
    std::unordered_map<std::string, Handler> handlers_;
};

} // namespace dream_machine