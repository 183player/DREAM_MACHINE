// platform/dm_base/init_list_utils.cpp
#include "init_list_utils.h"

#include "logger.h"
#include "plugin_types.h"

namespace dream_machine {
    namespace init_list_utils {

        // ================================================================
        // processInitList 实现
        //
        // 骨架内容与 monitor 的 handleInitList 完全一致；
        // executor 的差异（g_script_paths.clear() / stored N script paths）
        // 通过 Hooks 注入。
        //
        // 返回值：
        //   true  → 解析成功（调用方应发送 ACK）
        //   false → 解析失败（骨架已输出 ERROR 日志，调用方不应发送 ACK）
        // ================================================================
        bool processInitList(const std::string& payload, const Hooks& hooks) {
            LOG_INFO("Processing INIT_LIST...");

            auto list = plugin::initListFromJson(payload);
            if (!list.has_value()) {
                LOG_ERROR("Failed to parse INIT_LIST payload");
                return false;
            }

            // ---- on_parsed 钩子（executor 用它清空 script_paths） ----
            if (hooks.on_parsed) {
                hooks.on_parsed();
            }

            // ---- 统一遍历日志 ----
            for (const auto& entry : list->entries) {
                if (entry.type == plugin::ModificationType::REPLACE) {
                    LOG_INFO("REPLACE: target=" + entry.target_file +
                             ", winner=" + entry.winner_plugin_id);
                } else if (entry.type == plugin::ModificationType::EXTEND) {
                    LOG_INFO("EXTEND: container=" + entry.container_id +
                             ", plugin=" + entry.plugin_id);
                }
                if (!entry.rule_file.empty()) {
                    LOG_INFO("  rule_file: " + entry.rule_file);
                }
                if (!entry.trigger.empty()) {
                    LOG_INFO("  trigger: " + entry.trigger);
                }
            }

            // ---- 完成日志：钩子优先，默认兜底 ----
            if (hooks.on_completed) {
                hooks.on_completed();
            } else {
                LOG_INFO("INIT_LIST processing complete");
            }

            return true;
        }

    } // namespace init_list_utils
} // namespace dream_machine