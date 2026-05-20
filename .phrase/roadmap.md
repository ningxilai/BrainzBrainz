# Roadmap

## Goal

Keep long-term direction visible at phase granularity while letting short-term
implementation follow evidence.

## Planning Rule

Roadmap entries describe phase direction, entry conditions, acceptance gates,
and major out-of-scope boundaries. Detailed implementation tasks belong only in
`.phrase/current.md`.

## Phases

### Phase 1: Infrastructure Validation

**Status**: Complete

**Goal**: Validate that `jsonrpc.hpp` works end-to-end with Emacs `jsonrpc.el`
on Linux: stdio subprocess, Content-Length framing, request/response,
notification, and async callback.

**Entry Condition**: Repository exists with build system and compiled library.

**Acceptance Gate**:
- [x] C++ test server runs as subprocess from Emacs
- [x] Emacs `jsonrpc-request` receives correct response
- [x] Emacs `jsonrpc-notify` triggers C++ handler without response
- [x] C++ async method replies after delay
- [x] Clean shutdown works (internal shutdown pipe + join)
- [x] Linux example covers pipe-based waker + STDIN_FILENO input_fd

**Major Out-of-Scope**:
- Feed parsing or RSS/Atom awareness
- Newsticker integration or Elisp glue code
- Performance benchmarking or optimization

---

### Phase 2: C++ Feed Processor

**Status**: Complete

**Goal**: Build a C++ server that receives RSS/Atom feed XML from Emacs via
JSON-RPC, parses it in-memory, and delivers structured results in
configurable-sized chunks via notifications.

**Entry Condition**: Phase 1 validated; libxml2 available (v2.15.3).

**Acceptance Gate**:
- [x] RSS 2.0 feed parsed: title, description, link, pubDate extracted per item
- [x] Atom feed parsed: equivalently correct extraction
- [x] Multiple feeds can be queued for batch processing
- [x] Results delivered as JSON-RPC notifications in chunks (e.g., 10 items per notification)
- [x] No heap overflow or crash on malformed XML
- [x] Tested with sample feeds (TechCrunch, Wired, etc.)

**Major Out-of-Scope**:
- HTTP feed retrieval (Emacs will send XML, C++ won't fetch)
- Emacs buffer rendering or text properties
- Performance benchmarking

---

### Phase 3: Emacs Integration

**Status**: Complete

**Goal**: Build an Emacs Lisp library (`emacs-stdio-jsonrpc.el`) that manages the
feed_processor subprocess lifecycle and provides a clean API for sending feed
XML and receiving chunked results. Demonstrate the full pipeline with a
benchmark against baseline Newsticker.

**Entry Condition**: Phase 2 complete (feed_processor binary exists, tests pass).

**Acceptance Gate**:
- [x] `emacs-stdio-jsonrpc.el` starts/stops feed_processor subprocess
- [x] `emacs-stdio-jsonrpc-process-feed` sends XML, receives structured chunks
- [x] Notification dispatcher correctly accumulates feed_chunk messages
- [x] Batch processing: multiple feeds processed in sequence
- [x] Benchmark: large feed (10k items) benchmark recorded — Elisp avg 0.208s, C++ pure parse 0.045s, C++ full pipeline 2.147s
- [x] All existing tests still pass

**Major Out-of-Scope**:
- Newsticker plainview backend rewrite
- Full HTML rendering (shr/w3m) offloading
- Incremental buffer update (full rebuild remains)

---

### Phase 3c: Standalone Package Extraction

**Status**: Complete

**Goal**: Extract `emacs-stdio-jsonrpc.el` into a standalone MELPA-compatible
package for general Emacs ↔ C++ JSON-RPC communication.

**Entry Condition**: Phase 3 complete.

**Acceptance Gate**:
- [x] MELPA metadata present (Package-Requires, Version, URL, Commentary)
- [x] `package-install-file` succeeds
- [x] All existing tests pass

**Major Out-of-Scope**:
- Newsticker plainview rewrite
- Performance optimization
- Windows packaging

---

### Phase 4: Newsticker Plainview Enhancement

**Status**: Complete (archived)

**Goal**: Replace the Newsticker plainview rendering pipeline to use C++
feed_processor for HTML stripping and incremental buffer updates.

**Entry Condition**: Phase 3c complete.

**Acceptance Gate**:
- [x] `strip_html` in C++ feed_parser — tested for various HTML patterns
- [x] HTML stripping applied before JSON-RPC response
- [x] `emacs-stdio-jsonrpc-newsticker-view` inserts items incrementally per chunk
- [x] Chunk progress shown during processing
- [x] All 40 tests pass

**Major Out-of-Scope**:
- Incremental cache updates
- Full Newsticker buffer replacement
- Windows packaging

---

### Phase 5: Abstract Bridge Layer (deno-bridge 概念移植)

**Status**: Complete (archived)

**Goal**: 将 deno-bridge 的抽象层概念移植到 C++ JSON-RPC 基础设施上，实现通用
Emacs ↔ C++ 双向通信桥接库。支持多实例、C++ 调用 Emacs 函数、简化生命周期管理。

**Entry Condition**: Phase 4b 归档；jsonrpc.hpp + emacs-stdio-jsonrpc.el 可用。

**Acceptance Gate**:
- [x] 多实例支持（app-name 命名空间）
- [x] C++ → Emacs 双向 RPC（C++ 调用 Emacs 函数得返回值）
- [x] 简化 Elisp API（类似 `deno-bridge-call`）
- [x] 所有 44 项测试通过

**Major Out-of-Scope**:
- WebSocket 传输层
- feed_processor / Newsticker 集成
- Windows 打包

---

### Phase 6: Newsticker Cache SQLite 迁移

**Status**: Complete

**Goal**: 将 Newsticker 的 `prin1`/`read` 文件缓存替换为 Emacs 内置 `sqlite.el`，
消除大条目文件的序列化开销。SQLite DB 同时作为 C++ 子进程的共享持久化层。

**Entry Condition**: Phase 5 complete。

**Acceptance Gate**:
- [x] SQLite 数据库模式设计定稿
- [x] 持久化函数（save/read/save-feed）全部实现 sqlite 版本
- [x] 旧 prin1 缓存自动导入
- [x] 11 项单元测试通过（mode 开关、roundtrip、时间精度、迁移等）

**Major Out-of-Scope**:
- C++ 参与任何 Newsticker 逻辑
- 全文搜索
- Newsticker UI 改动

---

### Phase 7: Newsticker Plainview Pager (LazyCat 窗口裁切)

**Status**: Complete

**Goal**: Plainview 缓冲区只显示一页 feed 条目（~20 条），翻页时从
C++ `feed_reader` 读取 SQLite 获取下一页。实现 LazyCat 理念：
Emacs 保持轻量渲染，C++ 负责全部数据管理。

**Entry Condition**: Phase 6 complete；`feed_reader` 二进制通过端到端测试。

**Acceptance Gate**:
- [ ] `emacs-stdio-jsonrpc-newsticker-pager.el` byte-compiles clean
- [ ] `newsticker-pager-mode` 替换 buffer-insert-all-items，只渲染第一页
- [ ] `]`/`[` 翻页正确加载/卸载页面
- [ ] `f`/`F` 在 pager 模式下切换到相邻 feed
- [ ] 用户可通过 `newsticker-buffer-force-update` 回退到全量渲染

**Major Out-of-Scope**:
- item mark-read 双向同步（pager 不拦截 mark-read 操作）
- 全文搜索
- Windows 打包
