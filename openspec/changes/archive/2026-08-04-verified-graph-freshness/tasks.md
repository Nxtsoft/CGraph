## 1. Content Fingerprint and Cache Semantics

- [x] 1.1 Add failing `file_cache_test` cases for deterministic Merkle roots, path/content/file-set changes, and content validation of an equal-length preserved-mtime rewrite.
- [x] 1.2 Implement `ContentRoot`, domain-separated Merkle construction, and explicit metadata/content cache-validation modes; make the focused tests pass.
- [x] 1.3 Add a failing `file_watcher_test` for a same-size rewrite with restored mtime but changed POSIX file token, then implement the centralized device/inode/change-time token on macOS and Linux.

## 2. Verified Rescan and Persistence

- [x] 2.1 Add failing incremental rescan/update tests proving full verification reuses byte-identical extractions, re-extracts preserved-metadata content changes, removes deleted leaves, and publishes the expected root.
- [x] 2.2 Implement content-mode full rescans and watcher-event validation, compute the root from the canonical incremental cache, and publish it on `GraphSnapshot`.
- [x] 2.3 Add failing index/daemon persistence tests for root round-trip, missing/mismatched roots, and preserved-mtime startup invalidation.
- [x] 2.4 Persist the root in the versioned manifest, bump the index logic version, and require a complete content-verified tree/root match before fast-load.

## 3. Daemon Freshness Contract

- [x] 3.1 Add failing daemon-op tests for freshness metadata on status and graph reads, matching root pins, and fail-closed mismatched pins.
- [x] 3.2 Implement freshness result decoration and `expected_content_root` validation against the immutable snapshot selected for query/path/explain/impact/context.
- [x] 3.3 Add a failing real-daemon test for equal-length preserved-mtime edit → update barrier → changed root → pinned query, including rejection of the old root.
- [x] 3.4 Extend the update handler response with content-root identity and verification work counts; make the real-daemon flow pass without changing node-link exports.

## 4. MCP and Host Integration

- [x] 4.1 Add failing MCP server tests for the verified `graph_update` contract and `expected_content_root` on every graph read schema/forwarding path.
- [x] 4.2 Implement MCP descriptions/parameters and update the bundled CGraph skill and user-facing daemon/tool documentation with the synchronize-then-pin workflow.

## 5. End-to-End Verification and Delivery

- [x] 5.1 Configure and build the isolated worktree, then run all focused cache/watcher/incremental/persistence/daemon/MCP tests and record pass counts.
- [x] 5.2 Run the full default and sanitizer suites on macOS, and verify CI covers the same platform-sensitive code on Ubuntu.
- [x] 5.3 Run a real temporary-repository daemon flow for edit/add/delete, content-verified update, root-pinned query, persistence, and restart; clean up every created process and fixture.
- [x] 5.4 Benchmark verified update work (files, bytes, wall time) and warmed ordinary query latency, confirming ordinary reads do not perform the full-content audit.
- [x] 5.5 Review the implementation against every OpenSpec scenario, update all task checkboxes, and prepare evidence for the PR.
