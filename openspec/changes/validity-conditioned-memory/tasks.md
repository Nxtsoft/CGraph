## 1. Anchoring and grading

- [x] 1.1 Add a failing test in `tests/smoke/daemon_ops_test.cpp` with real source files: two
      functions in one file plus a location-less module. Cover valid on write, one body edited
      (only that link `changed`), a shifted but identical span (`valid`), and the location-less
      touch (`unanchored`).
- [x] 1.2 Implement `span_sha256` (uncapped span read through `SnapshotSourceReader`), store
      `anchor_sha256` on each resolved `concerns` edge in `remember_checkpoint`, and report
      `anchored`.
- [x] 1.3 Grade links in `recall_checkpoints` (`touch_validity`) and roll them up into the
      checkpoint `validity`.

## 2. Gone touches

- [x] 2.1 Extend the test: delete a touched function, rebuild and overlay the sidecars. The touch
      is listed under `gone` and the checkpoint is `stale`. A second overlay leaves node and edge
      counts and `gone` unchanged. Restoring the function clears `gone`.
- [x] 2.2 Return dropped edges from `rebind_memory_concerns`, and have `overlay_memory_fragments`
      rewrite `gone_touches` on each overlaid checkpoint.

## 3. Surfaces

- [x] 3.1 Update the `graph_remember` / `graph_recall` MCP descriptions,
      `integrations/skills/cgraph/SKILL.md` and `docs/session-memory.md`.

## 4. Verification

- [x] 4.1 Mutation check: hashing the whole file instead of the span fails the test (exit 214).
      Skipping the gone record fails it (exit 216).
- [x] 4.2 Full default suite: `ctest --test-dir build/default`.
- [x] 4.3 Real flow: a live `graphd` via `cgraph-client` on a scratch project. remember, then edit
      one function, then recall (`changed` next to `valid`). Delete a function, then recall
      (`gone`). Restart the daemon, then recall (anchors survive through the sidecar).
- [x] 4.4 `openspec validate validity-conditioned-memory --strict`.
