## 1. Settle the Graphify parity question (blocking)

- [ ] 1.1 Produce a real Graphify export of a fixture that also has a cgraph golden, and record
      verbatim whether `_make_id` receives an absolute or a repo-relative source path.
- [ ] 1.2 Record the answer and its evidence in the change. If Graphify passes an absolute path,
      stop: either withdraw this change, or convert it into an explicit documented divergence
      with sign-off. No task below starts until 1.1 is answered.

## 2. Relative ids at the single entry point

- [ ] 2.1 Add failing paired tests: the same source tree extracted from two different absolute
      roots produces byte-identical node ids, and ids no longer contain any path segment above
      the project root.
- [ ] 2.2 Pass the repo-relative path into `ExtractionContext::source_file`
      (`file_extraction.cpp:63`), leaving `make_id` (`normalize.cpp:111`) untouched.
- [ ] 2.3 Add a failing test pinning that a file directly at the project root, a file in a nested
      directory, and a path containing non-ASCII segments all normalize identically to today
      except for the removed prefix.
- [ ] 2.4 Verify no remaining node id contains an absolute path, on a real build of this repo.

## 3. Persisted state and goldens

- [ ] 3.1 Add a failing test that a semantic cache written with old-style ids is invalidated and
      requeued rather than silently resolving to the wrong node.
- [ ] 3.2 Add a failing test that memory sidecar `concerns` edges survive the transition, by
      re-resolution rather than by leaving a dangling id.
- [ ] 3.3 Regenerate committed goldens and parity fixtures; confirm the dead
      `..._agents_worktrees_fixture_reanchor_...` prefix is gone from every committed fixture.
- [ ] 3.4 Confirm the fast-load index rejects an index built with old-style ids instead of
      loading it.

## 4. Retrieval quality and verification

- [ ] 4.1 Re-measure the packing-parity floors and the retrieval-quality gate; record before/after
      numbers on the same graph.
- [ ] 4.2 Remove the checkout-depth environment note in
      `tests/smoke/pack_context_parity_test.cpp` once recall no longer varies with root length,
      and pin that independence with a test that runs the gate from two root depths.
- [ ] 4.3 Re-measure id character totals on the parity fixture; expect the 55.8% prefix share to
      go to zero.
- [ ] 4.4 Run the full default suite, the sanitizer suite, graph parity/golden tests, and the
      end-to-end retrieval gate.
- [ ] 4.5 Run OpenSpec validation and record the real-flow evidence in the PR.
