// platform/dm_base/constants.h
#pragma once

#include <string>

namespace dream_machine {

// ============================================================================
// 超时常量（毫秒）
// ============================================================================

namespace timeout {
    // 管道操作
    constexpr int PIPE_READ_MS      = 3000;
    constexpr int PIPE_WRITE_MS     = 100;
    constexpr int PIPE_CONNECT_MS   = 500;

    // 进程管理
    constexpr int PROCESS_START_MS      = 2000;
    constexpr int PROCESS_SHUTDOWN_MS   = 5000;

    // 脚本执行
    constexpr int SCRIPT_EXECUTE_MS = 5000;

    // 初始化列表
    constexpr int INIT_LIST_ACK_MS = 3000;

    // 看门狗
    constexpr int WATCHDOG_INTERVAL_MS = 5000;
    constexpr int WATCHDOG_TIMEOUT_MS  = 15000;
}

// ============================================================================
// 路径常量
// ============================================================================

namespace paths {
    inline const std::string DATA_DIR       = "./data";
    inline const std::string LOGS_DIR       = "./logs";
    inline const std::string PLUGINS_DIR    = "./plugins";
    inline const std::string SESSIONS_DIR   = "./data/sessions";

    inline const std::string SESSION_INDEX_FILE  = "session_index.json";
    inline const std::string PLUGIN_MANIFEST_FILE = "plugin_manifest.json";
    inline const std::string CLEAN_EXIT_FILE     = ".clean_exit";
}

// ============================================================================
// 管道名称前缀
// ============================================================================

namespace pipe_names {
    inline const std::string PREFIX = "\\\\.\\pipe\\DreamMachine_";

    // 基础管道名（用于各进程自身的服务端）
    inline std::string launcher()   { return PREFIX + "Launcher"; }
    inline std::string monitor()    { return PREFIX + "Monitor"; }
    inline std::string executor()   { return PREFIX + "Executor"; }
    inline std::string gui()        { return PREFIX + "Gui"; }
    inline std::string core_engine(const std::string& session_id) {
        return PREFIX + "Core_" + session_id;
    }

    // ============================================================
    // launcher 为每个子进程创建的独立管道名
    // ============================================================

    inline std::string launcher_monitor() {
        return PREFIX + "Launcher_Monitor";
    }

    inline std::string launcher_executor() {
        return PREFIX + "Launcher_Executor";
    }

    inline std::string launcher_gui() {
        return PREFIX + "Launcher_Gui";
    }
}

// ============================================================================
// 互斥量名称前缀
// ============================================================================

namespace mutex_names {
    inline const std::string PREFIX = "Global\\DreamMachine_";

    inline std::string launcher()   { return PREFIX + "Launcher_Mutex"; }
    inline std::string monitor()    { return PREFIX + "Monitor_Mutex"; }
    inline std::string executor()   { return PREFIX + "Executor_Mutex"; }
    inline std::string gui()        { return PREFIX + "Gui_Mutex"; }
    inline std::string core_engine(const std::string& session_id) {
        return PREFIX + "Core_" + session_id + "_Mutex";
    }
}

} // namespace dream_machine