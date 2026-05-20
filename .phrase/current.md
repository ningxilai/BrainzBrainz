# Current Phase

## Status

Complete

## Phase

Phase 7: Newsticker Plainview Pager (implicit)

## Goal

Plainview 缓冲区只显示一页 feed 条目，翻页时从 C++ `feed_reader` 请求下一页。
所有现有 Newsticker 键位不变——`n`/`p` 在页边界自动加载下一页/上一页，
`f`/`F` 切换到相邻 feed。不阻塞 Emacs 主线程，内存只装一页。

## Summary

**设计**：

- 集成在 `emacs-stdio-jsonrpc-newsticker.el` 中，没有独立文件
- `emacs-stdio-jsonrpc-newsticker-mode` 启用时自动尝试启动 feed_reader
- 如果 feed_reader 启动成功，安装 pager advice；否则标准 Plainview 行为不变
- Advice 方式：
  - `newsticker--buffer-insert-all-items` → 只插入第一 feed 第一页
  - `newsticker-next-item` / `newsticker-previous-item` → 边界检测，自动翻页
  - `newsticker-next-feed` / `newsticker-previous-feed` → 从 feed_reader 加载
- 翻页时 `newsticker--buffer-goto` 检测是否有下一项；没有则加载下一页
- feed 详情（title 等）来自 in-memory cache，条目来自 SQLite feed_reader

**未实现/简化**：
- mark-read 不同步到 SQLite（用户可通过 `M-x newsticker-buffer-force-update` 刷新）

## Acceptance Gate

- [x] `feed_reader` 启动并端到端可用（已验证 307 feeds, 13862 items）
- [x] byte-compile 通过
- [x] `newsticker-buffer-update` 只渲染第一页
- [x] `n`/`p` 在边界自动翻页
- [x] `f`/`F` 在 pager 模式下加载相邻 feed
- [x] 所有 16 项现有 tests 依旧通过（5 newsticker + 11 sqlite）
- [x] feed_reader 不可用时优雅降级（不安装 pager advice）

## Relevant Files

- `emacs-stdio-jsonrpc-newsticker.el` — 全部集成（sentinel + pager advice）
- `src/feed_reader.cpp` — C++ SQLite reader
- `emacs-stdio-jsonrpc-newsticker-sqlite.el` — SQLite 缓存后端

## Next

（无——项目核心管线已完整）
