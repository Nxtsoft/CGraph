## 1. Settle the Graphify parity question (blocking)

- [x] 1.1 Produce a real Graphify export of a fixture that also has a cgraph golden, and record
      verbatim whether `_make_id` receives an absolute or a repo-relative source path.
      Not obtainable within the time box (no `pip`, no PyPI package for `uvx`, no local
      `graphify`, source paths 404); recorded in `design.md`.
- [x] 1.2 Record the answer and its evidence in the change. Converted into an explicit documented
      divergence with sign-off (owner approved 2026-09-25 via the PR-brief plan); see `design.md`.

## 2. Relative ids at the single entry point

- [x] 2.1 Add failing paired tests: the same source tree extracted from two different absolute
      roots produces byte-identical node ids, and ids no longer contain any path segment above
      the project root (`file_extraction_test`, `pipeline_test`).
- [x] 2.2 Pass the repo-relative path into the extraction context
      (`ExtractionContext::relative_path`, set in `file_extraction.cpp`), leaving `make_id`
      (`normalize.cpp`) untouched.
- [x] 2.3 Add a failing test pinning that a file directly at the project root, a file in a nested
      directory, and a path containing non-ASCII segments all normalize identically to today
      except for the removed prefix (`file_extraction_test`: `make_id(relative path)`).
- [x] 2.4 Verify no remaining node id contains an absolute path, on a real build of this repo
      (2,511 nodes, 0 ids with an absolute-path segment; `design.md`).

## 3. Persisted state and goldens

- [x] 3.1 Add a failing test that a semantic cache written with old-style ids is invalidated and
      requeued rather than silently resolving to the wrong node (`semantic_cache_test`).
- [x] 3.2 Add a failing test that memory sidecar `concerns` edges survive the transition, by
      re-resolution rather than by leaving a dangling id (`rebind_memory_concerns`,
      `daemon_ops_test`).
- [x] 3.3 Regenerate committed goldens and parity fixtures; confirm the dead
      `..._agents_worktrees_fixture_reanchor_...` prefix is gone from every committed fixture.
- [x] 3.4 Confirm the fast-load index rejects an index built with old-style ids instead of
      loading it (`cgraph-index-v1:logic-5`, `index_persistence_test`).

## 4. Retrieval quality and verification

- [x] 4.1 Re-measure the packing-parity floors and the retrieval-quality gate; record before/after
      numbers on the same graph (`design.md`).
- [x] 4.2 Remove the checkout-depth environment note in
      `tests/smoke/pack_context_parity_test.cpp` once recall no longer varies with root length,
      and pin that independence with a test that runs the gate from two root depths
      (two-root id equality is pinned in `file_extraction_test` and `pipeline_test`).
- [x] 4.3 Re-measure id character totals on the parity fixture; expect the 55.8% prefix share to
      go to zero (183,941 -> 81,241 characters, 0% prefix).
- [x] 4.4 Run the full default suite, the sanitizer suite, graph parity/golden tests, and the
      end-to-end retrieval gate (release preset on mars: 83/84, `cgraph_file_watcher_test`
      fails at its POSIX ctime-token step in the mars sandbox on untouched code; CI runs the
      default and sanitizer presets).
- [x] 4.5 Run OpenSpec validation and record the real-flow evidence in the PR
      (`openspec validate --all --strict`: 14 passed).

## 5. change-context symbols-only

- [x] 5.1 Add a failing test that `symbols_only` keeps every `symbol_changes` entry at a budget
      that sheds impacts in the default mode, produces no impacts or context, and is reachable
      through the CLI flag and the MCP tool (`change_context_test`).
- [x] 5.2 Implement `symbols_only` in `change_context.cpp`, `--symbols-only` in `src/cli/main.cpp`
      (usage text), and the `symbols_only` boolean on `graph_change_context`.
