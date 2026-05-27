# SPEC-AGENTS v3：证据校准的 Agent 工作流

## 项目根因

本库是 C++ JSON-RPC 2.0 stdio 绑定库，为 Emacs `jsonrpc.el` 与 C++ 子进程之间提供标准、轻量的通信层。纯头文件（`include/jsonrpc.hpp`），C++20，零外部依赖（JSON 处理使用 bundled `nlohmann/json`）。

核心能力：Emacs 通过 stdio 启动 C++ 子进程，以 Content-Length 帧格式交换 JSON-RPC 2.0 消息。库提供 Reader 线程、线程安全队列、同步/异步方法注册、双向 RPC、notification、以及 POSIX 可中断 shutdown。

---

在处理任何请求之前，先识别用户意图，然后按最轻可行协议执行。

核心原则：

> 最小上下文，证据驱动阶段，验证后执行，只保留长期有价值的决策。

SPEC-AGENTS 不再要求读取和维护完整历史文档。默认只读取当前决策所需的最小上下文，并让上一阶段证据决定下一阶段计划。

---

## 1. 意图识别

### 🌱 启动 / 立项 / 模糊想法

触发条件：用户想开启新项目、新方向、新阶段，或只有模糊想法。

行动：
- 先读取 `.phrase/decision.md`、`.phrase/roadmap.md`、`.phrase/current.md`。
- 如果方向不清，扫描 `.phrase/modules/pr_faq.md` 的 YAML 元数据；匹配后再完整加载。
- 访谈目标是澄清当前 phase 的决策框架、证据规则、范围和验收门槛。
- 不要把远期工作拆成任务清单。

### 🔨 编码 / 重构 / 审查

触发条件：用户请求实现、修 Bug、重构、审查。

行动：
- 默认读取 `.phrase/decision.md`、`.phrase/roadmap.md`、`.phrase/current.md`。
- 只有当当前问题需要历史依据时，才读取 `.phrase/evidence.md` 或 `.phrase/archive/`。
- 执行当前 phase 的最小任务切片，验证后记录 evidence delta。
- 如需代码判断，可扫描 `.phrase/modules/linus_coding.md` 的 YAML 元数据；匹配后再完整加载。

### ✍️ 文案 / 营销 / 文档

触发条件：用户需要 README、发布说明、产品介绍、营销文案或文档改写。

行动：
- 扫描 `.phrase/modules/copywriting.md` 的 YAML 元数据；匹配后再完整加载。
- 输出仍要遵守当前 phase 的边界和证据规则。

### 🌐 浏览器 / 网页自动化 / 爬虫

触发条件：用户需要访问网页、抓取数据、截图、测试 Web UI 或填写表单。

行动：
- 扫描 `.phrase/modules/agent-browser.md` 的 YAML 元数据；匹配且依赖可用后再完整加载。
- 浏览器结果如果会改变后续判断，应写入 `.phrase/evidence.md`。

### 📋 默认任务执行

触发条件：用户给出明确任务。

行动：执行下方的 EDPP v3 工作流。

### 📝 会话收尾：`/done`

触发条件：用户输入 `/done` 或明确表示结束会话。

行动：
- 读取 `.phrase/commands/done.md`。
- 只记录实际发生的内容。
- 若本次会话产生会影响下一步的事实，优先更新 `.phrase/evidence.md`，不要只写会话流水账。

### 🚀 启动阶段：`/start-phase`

触发条件：用户输入 `/start-phase` 或明确表示要开启新阶段。

行动：
- 读取 `.phrase/commands/start-phase.md`。
- 用上一阶段 evidence 生成新的 `.phrase/current.md`。
- 只规划当前 phase，不预拆远期任务。

### 🔁 旧项目迁移：`/migrate-v3`

触发条件：项目已有旧版 `.phrase/phases/`、`spec_*`、`plan_*`、`task_*`、`change_*` 或 `issue_*` 流程。

行动：
- 读取 `.phrase/commands/migrate-v3.md`。
- 将旧材料归档到 `.phrase/archive/legacy-v2/`。
- 只把长期规则、当前 phase、未解决 blocker、验证结果和下一阶段建议提升到 v3 文件。
- 不做机械格式转换，不让旧文档继续成为默认上下文。

---

## 2. 默认读取规则

普通工作开始时只读：

```text
.phrase/decision.md
.phrase/roadmap.md
.phrase/current.md
```

读取 `.phrase/evidence.md` 的情况：

- 选择下一阶段
- 判断计划是否被新事实推翻
- 查 blocker / risk 的分类依据
- 验证 phase 是否可以关闭

读取 `.phrase/archive/` 的情况：

- 当前文件明确链接到某个归档项
- 回归问题需要历史对比
- 用户明确要求追溯旧上下文

不要默认加载完整历史。降低 token 消耗是协议目标之一。

---

## 3. 文件权威顺序

当文件冲突时，按以下顺序处理：

1. `.phrase/decision.md`、`.phrase/adr/`、`.phrase/protocol/`
2. 新鲜 evidence
3. `.phrase/current.md`
4. `.phrase/roadmap.md`
5. `.phrase/archive/`

如果新 evidence 和当前 phase 冲突，更新 `current.md`。如果新 evidence 挑战长期边界，显式更新 `decision.md`、ADR 或 protocol，不要在实现里偷偷改变规则。

---

## 4. EDPP v3 工作流

1. **确认决策框架。**
   明确证据规则、长期边界、验证标准和 phase gate。

2. **维护 roadmap。**
   roadmap 只写阶段方向、状态、入口条件和验收门槛，不写远期实现细节。

3. **从 evidence 选择当前 phase。**
   依据上一阶段结果决定下一步，不因为旧计划写过就继续执行。

4. **更新 current phase brief。**
   `current.md` 必须说明目标、范围、out of scope、验收门槛、当前任务切片、验证方式和已知 blocker。

5. **不确定时先 discovery。**
   用最小实验、trace、prototype、benchmark、audit、用户测试或 harness 暴露真实阻塞。

6. **先分类 blocker，再实现。**
   按项目语境分类：本地修复、共享机制、工作流边界、平台差异、产品歧义、运营依赖、数据质量等。

7. **只执行当前测量过的切片。**
   不顺手扩张到相邻问题。无关发现写入 evidence，留给后续 phase。

8. **验证。**
   运行 phase gate 要求的证明；影响面大时补更广验证。

9. **记录 evidence delta。**
   只记录会影响后续判断的事实：验证结果、失败假设、剩余 blocker、拒绝路径、下一阶段建议。

10. **必要时更新长期决策。**
    只有长期规则或边界变化时，才更新 `decision.md`、ADR 或 protocol。

11. **准备下一阶段。**
    用最新 evidence 更新 roadmap/current。过期 phase-local 细节进入 archive。

---

## 5. 最小文件结构

```text
.phrase/
  decision.md
  roadmap.md
  current.md
  evidence.md
  archive/

  adr/          # 可选：长期决策
  protocol/     # 可选：稳定接口和边界
  runbooks/     # 可选：重复手工流程
  modules/      # 可选：意图模块
  commands/     # 可选：命令说明
```

### `decision.md`

长期原则、证据规则、稳定边界、验证标准、phase gate、需要 ADR/protocol 的条件、不要重复探索的拒绝路径。

### `roadmap.md`

阶段级方向。只写 phase goal、status、entry condition、acceptance gate 和 major out-of-scope。

### `current.md`

默认上下文。只保留当前 phase 所需内容，必须短到每次会话都能读。

### `evidence.md`

证据增量。不是流水账，不是完整 changelog。区分 observation、interpretation、recommended next action。

### `archive/`

旧 phase、旧 spec、旧 task、历史 notes。默认不读。

---

## 6. 任务规则

任务只服务当前 phase。不要为 roadmap 里的远期阶段预拆任务。

推荐格式：

```text
taskNNN [ ] goal:<可观察结果> | scope:<文件或区域> | verify:<证明方式>
```

如果任务执行中暴露出不同 blocker 类型，停止扩张实现，更新 evidence，再决定是否改 phase。

---

## 7. 完成条件

声称 phase 或任务完成前，必须满足：

- acceptance gate 已检查
- verification evidence 存在
- 剩余 blocker 已记录
- 下一阶段建议已写入
- 如长期规则变化，已更新 decision/ADR/protocol
- 过期 local context 已归档或标记 stale

---

## 8. 提交与安全

- 提交信息说明为什么改、验证了什么、剩余风险是什么。
- 不要求每个提交绑定 `taskNNN`，但必须能追溯到当前 phase 和 evidence。
- 禁止提交密钥、token、证书、真实用户数据。
- 对权限、配置、外部 API、数据迁移等风险，必须在 `current.md` 或 `decision.md` 中写清边界和验证方式。

---

## 9. 协作表达

- 解释方案时先说当前 phase、证据、下一步。
- 引用文档时说文件名和小节，不复述整篇。
- 提供选项时说明它属于当前 phase、后续 phase，还是长期决策。

---

## 10. C++ / Elisp 协作要点（本项目特有）

### 通信协议

- **传输层**：stdio（stdin / stdout），C++ 子进程通过标准输入输出与 Emacs 通信。
- **应用层**：JSON-RPC 2.0，完全兼容 Emacs 内置 `jsonrpc.el`。
- **帧格式**：HTTP-like Content-Length 头（LSP 兼容）
  ```
  Content-Length: <length>\r\n\r\n<body>
  ```
- **Content-Length 计算的关键问题（长度不同问题）**：
  - Emacs 发送时使用 `(string-bytes json)` 计算字节数；C++ 接收后用 `std::string::length()` 匹配
  - 两边必须使用完全相同的 `\r\n` 行尾约定
  - 任何一方如果启用了行尾转换（如 Windows 文本模式将 `\n` 展开为 `\r\n` 或收缩 `\r\n` 为 `\n`），会导致 Content-Length 与实际 body 字节数不一致
  - 解决方案：
    1. Emacs `make-process` 必须指定 `:coding 'binary`
    2. C++ 在 Windows 上必须在构造函数中调用 `_setmode(_fileno(stdin), _O_BINARY)` 和 `_setmode(_fileno(stdout), _O_BINARY)`
    3. C++ 在 Linux 上无需特殊设置（pipe 默认 binary）
    4. 两边明确构造 `\r\n` 而非依赖系统行尾转换

### Emacs 端（Elisp）

```elisp
(require 'jsonrpc)

;; 创建子进程，务必使用 :coding 'binary
(setq proc (make-process :name "myserver"
                         :command `(,path)
                         :coding 'binary))

;; 构造 jsonrpc-process-connection 实例
(setq rpc (make-instance 'jsonrpc-process-connection
                         :name "myserver"
                         :process proc))

;; 调用远程方法（请求-响应）
(jsonrpc-request rpc "add" [1 2])        ;; => 3.0

;; 发送通知（无响应）
(jsonrpc-notify rpc "exit" nil)
```

关键点：
- `:coding 'binary` 是必需的，否则 Emacs 的行结束转换会破坏 Content-Length 计算。
- `jsonrpc-process-connection` 是 Emacs 内置 `jsonrpc.el` 提供的类，自动处理 Content-Length 帧解析。

### C++ 端

- **Reader 线程**：独立线程阻塞读取 stdin，读取完整帧后解析为 `Request` / `Response` / `Error`，投递到 `ThreadSafeQueue`。
- **主线程消费**：通过 `process_queue()` 从队列取出消息并分发：
  - `Request` → 查找注册的 handler 处理，通过 `Context` 回复或异步延迟回复。
  - `Response` → 查找 `pending_callbacks_` 中的回调执行。
- **方法注册**：
  - `register_method(name, sync_handler)` — 同步方法，返回 `json`。
  - `register_async_method(name, async_handler)` — 异步方法，通过 `Context::reply()` / `Context::error()` 延迟回复。
  - `register_notification(name, handler)` — 通知处理（无 ID 的 Request），不回复。
- **唤醒机制**：Reader 线程投递消息后，通过用户提供的 Waker 回调通知主线程处理。平台相关：
  - Windows：`PostThreadMessage(main_thread_id, WM_JSONRPC_WAKEUP, 0, 0)`
  - Linux：可用 eventfd / pipe / signal 等方式实现 Waker。

### 生命周期

- **启动**：`server.start()` 启动 Reader 线程。
- **消息循环**：主线程通常在一个事件循环中调用 `server.process_queue()` + `server.is_running()` 检查。
- **优雅关闭**：
  - 停止主动方式：通过通知触发退出逻辑（如 `PostQuitMessage`）。
  - 强制终止 Reader 线程：Windows 用 `CancelIoEx(GetStdHandle(STD_INPUT_HANDLE), nullptr)` 取消 stdin 阻塞读。
  - Linux 上：Reader 线程在每次阻塞读前用 `poll()` 检测内部 shutdown pipe，`server.stop()` 向该 pipe 写入数据唤醒 reader，然后调用 `thread.join()` 回收资源。
  - 最后调用 `server.stop()` 回收资源。
- **关键设计**：POSIX 上使用 `poll(stdin_fd, ..., 100ms)` 超时轮询实现可中断阻塞读。Linux 子进程创建 `Conn` 时必须传入 `STDIN_FILENO` 作为 `input_fd`，库内部创建 shutdown pipe。在 `stop()` 时写入 pipe 唤醒 reader 线程，最终 `join()` 安全回收，无需 `detach()`。
