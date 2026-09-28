Build environment used in every command below (see CLAUDE.md "Build, test, run"):

```
export VCPKG_ROOT="$PWD/.vcpkg"
cmake --preset default && cmake --build build/default
```

## 0. Gate (no engine code)

- [x] 0.1 Replay merged PRs with at least 2 commits (`refs/pull/N/head`) through `change_context`, using the protocol fixed in `design.md` "Gates", and record the result in `research/area3-preview/results.md`. Command: `python3 research/area3-preview/replay.py --repo . --out research/area3-preview/results.json`.
- [x] 0.2 Record the decision here. Proceed only if the preview beats both baselines with a CI95 excluding zero over at least 15 PRs; otherwise close this change with the measurement attached.

**Decision (2026-09-28): gate failed; do not build the preview op.** The Phase 0 replay (`~/.agents/artifacts/2026-09-25/cgraph-context-engine-research/briefs/area3-preview/results.md`) covered 15 PRs with 147 later-needed code files, at depth 3. The first commit's impact set caught 14 of them. Average catch rate per PR, with the gain over each baseline and its CI95:

| arm | catch rate | preview's gain | CI95 |
|---|---|---|---|
| preview | 0.220 | | |
| same-directory | 0.072 | +0.147 | [+0.028, +0.304] |
| git co-change | 0.227 | −0.008 | [−0.106, +0.091] |

The preview beats same-directory but ties co-change, and the two methods largely find the same files (9 in common at depth 3). The gate required beating both baselines, so implementation tasks 1–5 are not started. Caveats: only 7 PRs had a non-empty impact set, one PR (#113) supplies half the hits, and later commits mix real obligations with review churn.

## 1. Shared pieces (no behaviour change)

- [ ] 1.1 Failing tests first, then `unified_diff.{hpp,cpp}`: the parser moved out of `change_context.cpp`, plus a new `apply_patch`. `unified_diff_test.cpp` covers mismatch, overlap, add, delete, rename, and `apply_patch` round-trips. `ctest --test-dir build/default -R 'cgraph_(unified_diff|change_context)_test' --output-on-failure`.
- [ ] 1.2 `change_evidence.{hpp,cpp}`: the `add_side` builder with a `SourceProvider`. `change_context` moves onto the disk provider, and its test must pass unchanged. `ctest --test-dir build/default -R 'cgraph_(change_evidence|change_context)_test'`.
- [ ] 1.3 `extract_detected_source` sharing one body with `extract_detected_file`, including the 8 MiB cap. The test asserts an identical `ExtractionResult` from disk and from memory.
- [ ] 1.4 Expose `rebuild_graph` for an index overlay, with an optional stats out-parameter, and move `is_test_module` out of the anonymous namespace. Tests in the matching `_test.cpp` files.

## 2. Preview op

- [ ] 2.1 Failing tests first in `preview_test.cpp`: parity after drift, nothing published, every typed rejection, `unassessed` pass-through, re-extraction counts, detection on added files, path checks, and the depth range.
- [ ] 2.2 Implement `preview.{hpp,cpp}`, the injected `preview_handler` in `daemon_server.cpp`, `DaemonOp::Preview` plus `kSubstantiveOps`, MCP `graph_preview`, the client passthrough, and the workspace fallback.
- [ ] 2.3 Mutation check: remove the overlay re-extraction (parity fails), the freshness gate (the out-of-hunk test fails), and the hydration guard (the fast-load test fails). Restore each from a backup copy and confirm the file is byte-identical.

## 3. Verification for PR 1

- [ ] 3.1 Full suite: `ctest --test-dir build/default -j4 --output-on-failure`. Report pass and fail counts.
- [ ] 3.2 Real flow, a live `graphd` on a scratch project:
      ```
      P=$(mktemp -d); cp -r tests/fixtures/<preview-fixture>/. "$P"
      C="build/default/src/client/cgraph-client --root $P --daemon build/default/src/daemon/graphd"
      $C update '{}'
      $C preview "$(jq -n --rawfile d tests/fixtures/<preview-fixture>/delete-function.diff '{diff: $d}')"
      $C status '{}' | jq '.result.ops.preview'
      ```
      Expected: the deleted function's callers appear as dependents; `status` counts one preview; no file changes under `$P` (`find "$P" -newer <marker>` is empty, excluding `cgraph-out/`).
- [ ] 3.3 Non-author review (`code-reviewer` subagent), findings addressed.
- [ ] 3.4 Latency: `for i in 1 2 3 4 5; do /usr/bin/time -f %e $C preview '<one-file diff>'; done` against `$C update '{}'` after touching the same file. Report medians in the PR, before and after.

## 4. Obligation ledger (after Phase 2 go-ahead)

- [ ] 4.1 Failing tests first in `obligations_test.cpp`: every precedence row, added and deleted file sentinels, branch-only and in-diff consumers, `pending`, record idempotence, atomic resolve, restart survival, inertness in `impact`/`path`, and no obligation about an obligation.
- [ ] 4.2 Implement `obligations.{hpp,cpp}`, reusing the checkpoint sidecar and anchor machinery, plus the ops, MCP tools, host-skill section and `docs/obligations.md`.

## 5. Verification for PR 2

- [ ] 5.1 `ctest --test-dir build/default -j4`, a mutation check on the state precedence, and a non-author review.
- [ ] 5.2 Real flow: `$C preview '{"diff": ..., "record": true}'`, then `patch -p1 -d "$P" < fixture.diff`, then edit one caller, then `$C obligations '{}'` (expected: one `revisited`, the rest `open`), then `$C obligation_resolve '{"id": ..., "status": "satisfied", "note": "..."}'`, `$C shutdown`, and `$C obligations '{}'` again (the resolution survives).
- [ ] 5.3 `openspec validate preview-change-impact --strict`.
