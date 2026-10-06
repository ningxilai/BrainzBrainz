# Current Phase

## Status

Active

## Phase

Phase 9: MusicBrainz Bridge — type-driven `src/mb_bridge.cpp` benchmarking
`musicbrainz-api` (`lookup`/`search` + per-entity inc sets + kebab-case
entity shapes). Old C++ libmusicbrainz route abandoned; repo relocated from
`~/emacs-stdio-jsonrpc` to `~/.local/src/libmusicbrainz`.

## Context

2026-05-27 evidence entry reveals: the founding assumption (Elisp Newsticker needs
C++ offloading because it's slow) is empirically false. Elisp+libxml2 processes
10k-item RSS in ~208ms — imperceptible. C++ full pipeline takes 2.147s (10× slower).
The JSON-RPC stdio infrastructure library (`jsonrpc.hpp`) is sound; the Newsticker
offloading premise is not.

## Goal

Reposition `emacs-stdio-jsonrpc` as a **general-purpose C++ JSON-RPC stdio bridge**
for Emacs. The existing newsticker integration code is a functional demo but should
not be the project's identity. The project's actual value:
- `jsonrpc.hpp` — clean, portable C++ JSON-RPC 2.0 library with LSP-style framing
- `emacs-stdio-jsonrpc.el` — Emacs client for subprocess lifecycle + bidirectional RPC
- Thread-safe queue, async handlers, waker mechanism, clean shutdown on POSIX

## Out of Scope

- Finding a replacement "killer app" use case for C++ offloading
- Removing or breaking existing newsticker integration code
- Any new feature development

## Acceptance Gate

- [x] evidence.md updated with the assumption re-evaluation
- [x] evidence.md updated with benchmark verification (Elisp 0.118s confirmed, C++ unverifiable)
- [x] current.md updated with the new phase direction
- [x] roadmap.md updated with Phase 8 entry
- [x] README.org updated to reflect library's actual positioning
- [x] emacs-stdio-jsonrpc.el cleaned: removed process-feed/benchmark/feed_chunk/legacy singleton API
- [x] bridge_test_server extended with add/emacs_multiply/ping methods
- [x] test-emacs-stdio-jsonrpc.el rewritten to test multi-instance API (9/9 pass)
- [x] AGENTS.md rewritten: project root cause, removed Newsticker/benchmark sections
- [ ] `.phrase/evidence.md` audited for unverifiable entries → archive

## Next

After repositioning, the project is a stable infrastructure library. Future
direction depends on finding a real use case where C++ offloading provides
clear, measurable benefit over Elisp.
