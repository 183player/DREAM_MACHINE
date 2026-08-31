# Dream Machine 改进清单（规划文档）

> 本文档为**规划性质**文档，不修改任何源码。目的是把当前已识别的缺陷、优先级，以及"改动时必须遵守的既有一致性约定"整理成可执行清单，供后续开发按序落地。
>
> 所有条目均给出**相对路径 + 函数名/行号**（行号基于 `dev` 分支当前版本，代码变动后以函数名为准）。

---

## 1. 背景与定性修正

### 1.1 "无自动容错/重连"是有意设计，不列为缺陷

经与作者确认，系统当前**没有运行时自动容错、自动重连、自动重启**，这是**有意的 fail-fast 设计**，由三层机制共同构成：

| 机制 | 位置 | 说明 |
| --- | --- | --- |
| 级联退出 | `src/launcher/main.cpp` → `onProcessExit()`（约 212-221 行） | 任一被管理子进程退出即置 `g_shutdown_requested = true` 并 `g_event_loop->stop()`，整树随之收敛 |
| 管道断裂即停机 | `src/core_engine/main.cpp` → `processMonitorMessage()`（约 37-43 行）、`processExecutorMessage()`（约 101-107 行）的 `isBroken()` 分支 | 对端消失即停止事件循环，不做重连 |
| Job Object 兜底 | `src/launcher/main.cpp` → `main()`（约 413-428 行）配置 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`；`src/monitor/main.cpp` → `main()`（约 334-347 行）同样配置 | 即使父进程异常终止，内核也会清理整棵进程树，杜绝孤儿 |
| 每进程独立日志 | `platform/dm_logger/logger.cpp`；各进程 `main()` 首行 `Logger::instance().setProcessName(...)` | 崩溃后可按进程独立追溯 |

**结论：不要把"缺少重连/重试"当作缺陷来修。** 后续任何改动都必须保持"故障即快速、可预期地退出，并由 Job Object 兜底"的语义。

### 1.2 原子布尔的语义澄清

`src/launcher/main.cpp` 的 `g_shutdown_requested`（约 60 行）、`src/monitor/main.cpp`/`src/executor/main.cpp`/`src/core_engine/main.cpp` 的 `g_should_stop`（分别约 56 / 27 / 25 行）是**进程内的"优雅停机闸门"**，用于让回调尽快短路返回；它们**不是跨进程崩溃信号**，跨进程不可见。

**崩溃察觉**实际依赖两条通道：

| 通道 | 位置 |
| --- | --- |
| 句柄等待（`WaitForMultipleObjects`） | `src/launcher/main.cpp` → `main()` 中 `event_loop.registerWaitable(proc.getHandle(), onProcessExit, &proc)`（约 542-549 行）；底层见 `platform/dm_event/event_loop.cpp` → `EventLoop::processEvents()`（约 288 行） |
| 管道断裂检测（`isBroken()`） | `src/monitor/main.cpp` → `pollCorePipes()`（约 269-295 行，500ms 定时轮询）；`platform/dm_pipe/pipe.h` → `NamedPipe::isBroken()` |

### 1.3 已建模但休眠 / 纯脚手架的能力

| 能力 | 位置 | 现状 |
| --- | --- | --- |
| 崩溃分类（`SessionState::CRASHED` / `SessionEndReason::CRASHED`） | `src/monitor/main.cpp` 约 29-48 行 | 已建模，但因 `REQUEST_ENGINE` 未实现、`sessions_` 永远为空而**休眠** |
| `cleanupCrashedSession()` | `src/monitor/main.cpp` 约 107-132 行 | 逻辑完整，但同样因无会话入库而不会被触发 |
| 结构化错误 `ErrorContext` / `ErrorSeverity` | `platform/dm_base/error_codes.h` 全文（13-79 行），配合 `platform/dm_base/constants.h` 的 `ErrorCode`/`errorCodeToString()` | **纯脚手架**：全仓仅在这两个头文件出现，**无任何运行时调用点** |
| 条件性重拉起 | —— | **完全未实现**，且按 §1.1 的定性，不作为默认目标 |

---

## 2. 第一类：核心业务链路（最高优先级）

主链路 `GUI → launcher → monitor → core_engine → executor` 目前在三处断开，导致系统无法完成任何一次真实业务。

| 编号 | 缺口 | 位置 | 需要实现的内容 |
| --- | --- | --- | --- |
| A1 | monitor `REQUEST_ENGINE` 未实现 | `src/monitor/main.cpp` → `processLauncherMessage()` 约 224-233 行（仅有"超限拒绝"分支，主路径只打 `LOG_WARN("REQUEST_ENGINE not yet implemented")`） | 见下方展开 |
| A2 | executor `RUN_SCRIPT` 未实现 | `src/executor/main.cpp` → `processLauncherMessage()` 约 108-110 行 | 见下方展开 |
| A3 | core_engine 丢弃 executor 结果 | `src/core_engine/main.cpp` → `processExecutorMessage()` 约 127-129 行（`// TODO: 处理操作结果（STEP_*, OP_DONE, OP_ABORT）`） | 见下方展开 |

### A1 · monitor 实现 `REQUEST_ENGINE`

在 `src/monitor/main.cpp` 中新增 `handleRequestEngine(const std::string& payload, NamedPipe& launcher_pipe)`，并在 `processLauncherMessage()` 的分发处调用。要求按序完成：

1. `parseRequestEngine(payload)` 取出 `session_id`（结构体 `RequestEngineMessage`，见 `platform/dm_base/messages.h` 约 94-96 行）；
2. 先执行 `checkMaxSessionsReached()`（约 135-145 行）；超限则回 `EngineFailedMessage{session_id, "max_sessions_reached"}`（注意**当前代码把 `session_id` 硬编码为 `"unknown"`（约 229 行），实现时必须改成真实 session_id**）；
3. 以 `pipe_names::monitor_core(session_id)`（`platform/dm_base/constants.h` 约 94 行）建**服务端**管道（`NamedPipe::createServer`，见 `platform/dm_pipe/pipe.h`）；
4. 拉起 `core_engine.exe`（`platform/dm_process/process.h` → `Process::start()`），参数须带 `--session-id <id>` 与 `--parent-pid <monitor pid>`——两者都是 core_engine 启动时的硬性校验，见 `src/core_engine/main.cpp` → `main()` 约 165-179 行；并挂入 `g_monitor_job_`（约 52 行）；
5. `waitForClient()` 后等待 core_engine 主动发来的 `REGISTER_SESSION`（core_engine 侧发送点：`src/core_engine/main.cpp` 约 213-222 行）；
6. 成功：回 `ENGINE_ASSIGNED`（`EngineAssignedMessage{session_id, pipe_name}`），把 `Session` 存入 `sessions_`（约 50 行）并置 `SessionState::RUNNING`，同时 `sendSessionStateToLauncher(..., "running", pipe_name)`（约 59-72 行）；
7. 失败（建管道失败 / 拉起失败 / 等待注册超时）：回 `ENGINE_FAILED`，清理已创建的管道与进程句柄，不留半成品 `Session`。

> 落地 A1 后，§1.3 中"休眠"的崩溃分类与 `cleanupCrashedSession()` 会自动被 `pollCorePipes()` 激活——这是接线其余能力的前提。

### A2 · executor 实现 `RUN_SCRIPT`

复用既有 `RunScriptMessage`（`platform/dm_base/messages.h` 约 207-211 行）与 `ScriptResultMessage`（约 213-217 行）：

1. `parseRunScript(payload)` → 得到 `script_path` / `params` / 可选 `session_id`；
2. 执行脚本（沿用 `platform/dm_process/process.h` 的 `Process`，超时使用 `constants::SCRIPT_EXECUTE_TIMEOUT_MS`，见 `platform/dm_base/constants.h` 约 109 行）；
3. 回报 `serializeScriptResult(...)`：成功填 `result`，失败填 `error`；
4. 与 `handleInitList()`（约 33-59 行）中已解析但尚未使用的 `g_script_paths`（约 30 行）打通——目前该向量被 `clear()` 后从未写入，属于半成品，须一并补齐或显式移除。

### A3 · core_engine 处理 executor 回执

在 `src/core_engine/main.cpp` → `processExecutorMessage()`（约 127 行 TODO 处）用 `parseBaseMessage` 分发，并复用既有结构体：

| 消息类型（`msg_types`） | 结构体 | 处理要点 |
| --- | --- | --- |
| `STEP_START` / `STEP_OK` / `STEP_ERR` | `StepMessage`（`messages.h` 约 194-198 行） | 记录步骤进度；`STEP_ERR` 需带 `error` 上行 |
| `OP_DONE` | `OpResultMessage`（约 200-204 行） | 标记操作完成，结果上报 monitor |
| `OP_ABORT` | `OpResultMessage` | 终止当前操作，按 §4 接线 `ErrorNotifyMessage` |

---

## 3. 第二类：结构性 / 韧性缺口

| 编号 | 问题 | 位置 | 改进方向 |
| --- | --- | --- | --- |
| B1 | 故障爆炸半径 = 全系统 | `src/launcher/main.cpp` → `onProcessExit()` 约 212-221 行 | 回调**不看退出码**即触发全体停机。多会话场景下需把"会话级故障"从"全局 fail-stop"中拆出：正常退出（exit code 0）与崩溃应分流处理。可直接复用已实现但未被调用的 `Process::getExitCode()`（`platform/dm_process/process.h` 约 122 行，实现在 `platform/dm_process/process.cpp` 约 366 行）。**注意：§1.1 的 fail-fast 语义仍然保留**——拆分的目标是"隔离会话级故障"，不是引入自动重连 |
| B2 | `request_id` 写死为 0 | `src/monitor/main.cpp` → `processLauncherMessage()` 中 `MONITOR_GET_ACTIVE_SESSIONS` 分支约 236-250 行（`resp_msg.request_id = 0;` 约 238 行） | 建立请求/响应关联机制：沿用既有 `int64_t request_id` 类型与命名（`FullSyncRequestMessage`/`FullSyncResponseMessage`，`messages.h` 约 130-137 行），提供**统一生成点**（建议下沉到 `platform/dm_base`），禁止各进程各自造号 |
| B3 | 结构化错误未接线 | 崩溃/断裂路径：`src/launcher/main.cpp` `onProcessExit()`（212 行）/ `onPipeReadable()`（约 229-237、312-319 行）；`src/monitor/main.cpp` `pollCorePipes()`（269 行）、`cleanupCrashedSession()`（107 行）；`src/core_engine/main.cpp`（37/48/82/101/112/131 行的 `isBroken` 分支） | 这些路径现在只 `LOG_INFO` + `stop()`，对外**静默**。应主动 emit `ErrorNotifyMessage`（`messages.h` 约 220-225 行），其 `severity` 字段用 `severityToString()`（`error_codes.h` 约 68 行）从 `ErrorSeverity` 映射；崩溃原因应经 `EngineDiedMessage`（约 232-235 行）/ `SESSION_TERMINATED`（`msg_types` 约 25 行）通知 GUI |
| B4 | 消息协议无版本 / schema 校验 | `platform/dm_base/messages.h` → `parseBaseMessage()`（约 324-327 行，实现在 `platform/dm_base/messages.cpp`）；所有调用点失败后仅 `LOG_WARN("Failed to parse base message")` 后丢弃（`src/launcher/main.cpp` 约 310 行、`src/monitor/main.cpp` 约 253 行、`src/executor/main.cpp` 约 112 行、`src/core_engine/main.cpp` 约 79 行） | 引入协议版本字段与必填字段校验；解析失败应可区分"版本不兼容"与"消息损坏"，并按 B3 上报而非静默丢弃 |

---

## 4. 第三类：代码质量 / 打磨

| 编号 | 问题 | 位置 | 改进方向 |
| --- | --- | --- | --- |
| C1 | 四进程各自维护 if-else 分发链 | `src/launcher/main.cpp` `onPipeReadable()` 约 251-311 行；`src/monitor/main.cpp` `processLauncherMessage()` 约 221-254 行；`src/executor/main.cpp` `processLauncherMessage()` 约 100-113 行；`src/core_engine/main.cpp` `processMonitorMessage()` 约 67-80 行 | 统一改为 `type_str → handler` 的分发表，下沉到公共模块（建议 `platform/dm_base`），四进程共用同一注册/查表实现 |
| C2 | `processLauncherMessage` / `handleInitList` 重复样板 | monitor（`processLauncherMessage` 183-264 行、`handleInitList` 75-104 行）与 executor（`processLauncherMessage` 62-123 行、`handleInitList` 33-59 行）几乎逐行重复；`isBroken`/`peekAvailable`/`readLineBuffered` 三段式在 core_engine 的两个回调中又各复制一份 | 抽取"读一行并分发"的公共回调骨架与 INIT_LIST 处理骨架到公共模块，进程侧只保留差异化 handler |
| C3 | 全局状态散落，难以测试 | `src/launcher/main.cpp` 约 57-79 行（`sessions_`/`sessions_mutex_`/`g_plugin_manager`/`g_managed_processes`/三个管道指针/`g_event_loop`，且已有一个**未被真正使用**的 `LauncherContext`/`g_context`）；`src/monitor/main.cpp` 约 50-56 行；`src/executor/main.cpp` 约 25-30 行；`src/core_engine/main.cpp` 约 22-26 行 | 收敛进上下文对象并通过回调的 `user_data` 传递（`EventLoop` 回调签名已预留 `void* user_data`），提升可测试性。launcher 已声明的 `LauncherContext` 应作为落地起点 |
| C4 | 窄转宽编码不安全 | 全仓 **8 处** `std::wstring(str.begin(), str.end())`：`src/launcher/main.cpp` 436/437/438 行、`src/monitor/main.cpp` 350 行、`src/executor/main.cpp` 160 行、`src/gui/main.cpp` 364 行、`src/core_engine/main.cpp` 185-186 行与 200-201 行 | 在 `platform/dm_base` 新增 UTF-8↔UTF-16 helper（如 `utf8ToWide()` / `wideToUtf8()`），**一次性替换全部 8 处**。反向的 `std::string(w.begin(), w.end())` 同样应收编（`src/launcher/main.cpp` 95/100/474 行、`platform/dm_process/process.cpp` 148/158 行、`platform/dm_pipe/pipe.cpp` 245/254/347/354/380/387/395/413/425 行） |
| C5 | GUI 资源路径定位分支混乱 | `src/gui/main.cpp` → `main()` 约 436-471 行（dev / 非 dev 两条分支，各自 `cdUp()` 若干次并带多级回退，最后还有一处硬编码相对路径 `"../src/gui/qml/main.qml"`，约 469 行） | 统一为单一"资源根目录解析"函数：一次确定 root，再由 root 拼接所有资源路径；去掉硬编码回退 |
| C6 | `EventLoop` 命名与实现落差 | `platform/dm_event/event_loop.cpp` → `EventLoop::processEvents()` 约 255-261 行：READABLE 类型实际是 `isHandleReadable()` **轮询**（无句柄时 `Sleep(10)`，约 282-285 行），仅 WAITABLE/SIGNAL 真正进入 `WaitForMultipleObjects`（约 288 行）；但对外命名与日志一律称 "event-driven main loop"（四个进程的 `main()` 均有此日志） | 二选一：在命名/注释中**诚实标注** READABLE 为轮询语义，或改用重叠 I/O（`OVERLAPPED` + 事件句柄）使其真正事件驱动。不要保留"名实不符" |

---

## 5. 第四类：工程保障

| 编号 | 事项 | 说明 |
| --- | --- | --- |
| D1 | 补可回归的冒烟测试 | 仓库当前**无任何测试目录/测试目标**（`CMakeLists.txt` 中无 `enable_testing()`）。重点覆盖两处：<br>① **级联退出链路**——"子进程崩溃 → `onProcessExit`（`src/launcher/main.cpp` 212 行）→ 全体退出 → 无孤儿进程（Job Object `KILL_ON_JOB_CLOSE` 生效）"；<br>② **管道拆包**——`platform/dm_pipe/pipe.cpp` → `NamedPipe::fillBuffer()`（约 126 行）与 `readLineBuffered()`（约 561 行调用点）的粘包/半包/超长行边界 |
| D2 | 跨进程日志关联 ID | 配合 B2 的 `request_id` 落地全链路追踪：`platform/dm_logger/logger.h`/`logger.cpp` 增加关联 ID 字段，各进程在处理同一请求时透传，使四进程日志可按 ID 串联 |

---

## 6. 关键一致性约束（实施时必须遵守）

> 本节为**硬性约束**。任何一条被违反，都会造成协议分裂或风格分裂，代价高于功能本身。

1. **禁止另起并行协议。** 实现任何缺失功能，必须复用 `platform/dm_base/messages.h` 中已定义的消息结构体与 `msg_types` 命名空间常量（约 14-74 行）。已定义但尚未使用的类型（`RequestEngineMessage`、`EngineAssignedMessage`、`StepMessage`、`OpResultMessage`、`RunScriptMessage`、`ScriptResultMessage`、`ErrorNotifyMessage`、`EngineDiedMessage`）就是为这些缺口预留的——**先用，不要新造**。序列化一律走 `serializeXxx`/`parseXxx` 配对函数，消息封装一律走 `buildMessage()`（约 320-322 行）。

2. **错误模型只允许一条映射路径。** 必须建立 `ErrorContext`/`ErrorSeverity`（`platform/dm_base/error_codes.h`）→ `ErrorNotifyMessage`（`messages.h` 约 220-225 行）的**单一映射**：`ErrorContext::code` 经 `errorCodeToString()`（`constants.h` 约 117 行）落到 `message`/`details`，`ErrorContext::severity` 经 `severityToString()`（`error_codes.h` 约 68 行）落到 `severity` 字段。不允许出现第二套错误表达（如裸字符串错误码、自定义 severity 字面量）。

3. **会话状态字符串三端必须同步。** `"running"` / `"crashed"` 等字面量目前分散在三处：`src/monitor/main.cpp`（166、244 行写入 `"running"`；130 行写入 `"crashed"`）、`src/launcher/main.cpp` 的 `SessionState::state` 透传（约 131-171 行）、`src/gui/session_state_manager.h`（29、45 行比较 `"running"`）。新增或重命名任何状态，**必须三端同时改**；建议把状态字面量收敛为公共常量（同 `msg_types` 的做法）后再扩展。monitor 内部枚举 `SessionState`（约 29-34 行，含 `CREATING`/`SHUTTING_DOWN`）与对外字符串目前并非一一对应，接线时须先补齐映射函数。

4. **编码转换与分发表重构必须全仓一次性统一。** C1（分发表）与 C4（编码 helper）都属于"横切"改造：只改一半会导致同一仓库内两种风格并存，后续读者无法判断哪种是规范。要么整批替换（C4 的 8 处 + 反向转换点、C1 的 4 个分发链），要么暂不动手。

5. **保持 fail-fast 语义。** 任何改动不得引入静默重试、自动重连或"吞掉错误继续跑"的行为；B1 的会话级隔离也只是把停机范围从"全系统"收窄到"单会话"，不改变"故障必须被察觉并快速收敛"这一原则。

---

## 7. 推进顺序

| 阶段 | 内容 | 对应条目 |
| --- | --- | --- |
| 一 | **核心链路**：打通 REQUEST_ENGINE / RUN_SCRIPT / executor 回执 | A1 → A2 → A3 |
| 二 | **已建模能力接线**：会话状态机（激活 `SessionState`/`cleanupCrashedSession`）、`ErrorContext` → `ErrorNotifyMessage`、`request_id` 统一生成 | B3、B2 |
| 三 | **结构性韧性**：多会话故障隔离（`getExitCode()` 分流）、协议版本与 schema 校验 | B1、B4 |
| 四 | **打磨**：分发表下沉、重复样板去重、编码 helper 统一、全局状态收敛、GUI 路径统一、EventLoop 命名校正 | C1 → C2 → C4 → C3 → C5 → C6 |
| 五 | **工程保障**：冒烟测试、跨进程日志关联 ID | D1、D2 |

阶段一是其余一切的前提——在会话真正能被创建之前，第二、三阶段的大部分代码路径都无法被触发，也无法被验证。
