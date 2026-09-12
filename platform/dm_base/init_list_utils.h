// platform/dm_base/init_list_utils.h
#pragma once

#include <functional>
#include <string>

namespace dream_machine {
    namespace init_list_utils {

        // ================================================================
        // InitList 处理骨架（阶段 1.7 C2）
        //
        // 用途：消除 monitor 与 executor 的 handleInitList 重复样板。
        //
        // 骨架内容（各进程完全相同）：
        //   1. 输出 "Processing INIT_LIST..."
        //   2. 解析 payload（plugin::initListFromJson）
        //   3. 解析失败 → 输出 ERROR 日志 → 返回 false
        //   4. 遍历每个 entry，输出统一的 4 类日志
        //      （REPLACE / EXTEND / rule_file / trigger）
        //   5. 输出完成日志（可被 on_completed 钩子替换）
        //   6. 返回 true
        //
        // 各进程的差异点通过 Hooks 注入：
        //   - on_parsed   : 解析成功后、遍历前调用（executor 用它清空 script_paths）
        //   - on_completed: 遍历完成后调用，替代默认 "INIT_LIST processing complete"
        //                   （executor 用它输出 "stored N script paths"）
        //
        // ACK 发送不在骨架内：
        //   为避免 dm_base → dm_pipe 循环依赖，骨架只做解析+日志；
        //   各进程的 handler 拿到返回值后自行发送 INIT_LIST_ACK。
        //
        // 回退方式：
        //   若未来发现骨架不合适，删除本文件与 .cpp，各进程恢复自己
        //   的 handleInitList 实现即可。
        //
        // 保守设计：
        //   - 只提供两个钩子，不做 on_entry（当前无进程需要）
        //   - 不做模板参数，避免复杂度
        //   - 不做日志级别配置（统一 INFO）
        // ================================================================

        struct Hooks {
            // 解析成功后、遍历 entries 前调用（可选）
            std::function<void()> on_parsed;

            // 遍历完成后调用（可选）
            // 若未设置，骨架输出默认日志 "INIT_LIST processing complete"
            // 若设置，骨架**不输出**默认日志，由钩子自行输出
            std::function<void()> on_completed;
        };

        // 处理 INIT_LIST payload。
        //
        // 返回值：
        //   true  → 解析成功，调用方应发送 INIT_LIST_ACK
        //   false → 解析失败（骨架已输出 ERROR 日志），调用方不应发送 ACK
        [[nodiscard]] bool processInitList(const std::string& payload,
                                            const Hooks& hooks = {});

    } // namespace init_list_utils
} // namespace dream_machine