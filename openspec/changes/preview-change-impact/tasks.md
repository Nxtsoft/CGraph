## 0. Gate (no engine code)

- [ ] 0.1 Replay merged multi-commit CGraph PRs through `change_context` (first commit as the proposal), measure the catch rate of later-needed code files against same-size same-directory and co-change baselines, and record it under `research/area3-preview/results.md`.
- [ ] 0.2 Decision recorded in this file. Proceed only if the preview beats both baselines with a CI95 excluding zero; otherwise close the change.

## 1. Shared diff handling

- [ ] 1.1 Move the unified-diff parser and patch application from `change_context.cpp` into `unified_diff.{hpp,cpp}` with `unified_diff_test.cpp` (hunk mismatch, overlap, add, delete, rename). `change_context`'s existing tests must pass unchanged.
- [ ] 1.2 Add `extract_detected_source` beside `extract_detected_file`, sharing one body. Test: identical `ExtractionResult` for the same bytes read from disk or passed in.
- [ ] 1.3 Expose `rebuild_graph` for an index overlay (header plus test).

## 2. Preview op

- [ ] 2.1 Failing tests first: parity with `change_context`, nothing written or published, rejections (mismatch, not caught up, outside root), one-file re-extraction counts, and path checks.
- [ ] 2.2 Implement `preview` in the daemon (`DaemonOp::Preview`, additive op-stats) plus the MCP `graph_preview` and client passthrough.
- [ ] 2.3 Mutation check: without the overlay re-extraction parity fails; without the freshness gate the rejection test fails.

## 3. Verification for PR 1

- [ ] 3.1 Full default suite.
- [ ] 3.2 Real flow on a live `graphd` via `cgraph-client`.
- [ ] 3.3 Non-author review.
- [ ] 3.4 Latency: one-file preview on this repository against an incremental update, before and after, in the PR description.

## 4. Obligation ledger

- [ ] 4.1 Failing tests first: record, lifecycle (`proposed`, `open`, `revisited`, `gone`), resolve, `stale` after resolve, restart survival, inertness.
- [ ] 4.2 Implement record, derived states, and resolve, reusing the checkpoint sidecar and anchor machinery.
- [ ] 4.3 MCP `graph_obligations` and `graph_obligation_resolve`, the host skill section, and `docs/obligations.md`.

## 5. Verification for PR 2

- [ ] 5.1 Full default suite, mutation check, non-author review.
- [ ] 5.2 Real flow: preview with record, apply the diff with `patch`, fix one caller, list obligations (one `revisited`, the rest `open`), resolve one, restart, and list again.
- [ ] 5.3 `openspec validate preview-change-impact --strict`.
- [ ] 5.4 Optional: `graph_recall` shows open obligations recorded in the same task.
