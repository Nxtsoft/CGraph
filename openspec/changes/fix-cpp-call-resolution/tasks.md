# Tasks

Prerequisite `/opsx:archive verified-graph-freshness` is already done (commit `b7eba6f`).

## 1. Baseline first — this is the before-number

- [ ] 1.1 Add a `release` configure/build/test preset to `CMakePresets.json`
      (inherit `default`, `CMAKE_BUILD_TYPE=Release`). Add `release` to the CI
      matrix at `.github/workflows/ci.yml:18`. Point the README build steps
      (`README.md:131`) at it.
- [ ] 1.2 Record in `docs/benchmark-report.md`: one-shot wall and per-phase
      `phase_ms` for Debug vs Release, and that `graph.json` is byte-identical
      between them. (Measured while planning: 449 ms -> 123 ms, byte-identical.)
- [ ] 1.3 Capture the pre-change graph baseline, with commands, into the change
      dir: `CALLS` edge count; call-target kind histogram; function nodes with no
      incoming `CALLS`; function nodes with zero call/ref/import edge; count of
      `class` nodes labelled `cgraph`; share of `method` edges originating at one;
      top-10 nodes by degree; and the share of 300 random connected function pairs
      whose shortest path crosses a namespace-as-class node.

## 2. Cause D first — remove the god node (smallest change, clears the noise)

Doing D before A/B means the call-edge numbers are measured against a graph that
is no longer dominated by namespace containment.

- [x] 2.1 Remove `namespace_definition` from `class_node_types` in `cpp_config()`
      (`src/engine/configured_extractors.cpp:54`). Nothing else references it —
      it is the only occurrence in `src/` or `tests/`.
- [x] 2.2 No new kind and no `analysis.cpp` guard are needed: with no namespace
      node emitted, `label_for_node`'s documented skip path applies
      (`extractor.cpp:69-77`) and members attach to their file with `contains`.
      Decision and rationale recorded in `design.md` Open Questions.
- [x] 2.3 Re-census. Expect: 96 fewer `class` nodes, ~416 `method` edges become
      `contains` on the file node, `class 'cgraph'` off the top-degree list, and
      the 91% path-through-namespace share collapsing.
- [x] 2.4 Confirm no symbol was lost: the function/class/field node count must not
      drop. Losing a symbol to the skip path would be a defect.
- [x] 2.5 No golden update was needed -- nothing asserted on namespace nodes, and
      all 64 tests stayed green. Added a regression test instead
      (`tests/smoke/cpp_extractor_test.cpp`): a namespace emits no class node, its
      members attach to their file with `contains`, and a real class still owns its
      methods with `method`. Verified the test fails on all three when
      `namespace_definition` is put back.

## 3. Cause A — bare C/C++ function labels

- [x] 3.1 Exposed `declarator_name` through a `ResolveFunctionName`-shaped public
      wrapper `cpp_function_name` rather than promoting the raw walker, so the
      public surface is exactly the hook the config needs. It needed one fix:
      tree-sitter-cpp's `reference_declarator` holds its inner declarator as an
      unnamed child, not a `declarator` field, so a field lookup alone left
      `& lock_map_mutex()` unnamed. Added a named-child scan gated on the
      `_declarator` suffix.
- [x] 3.2 Add a `ResolveFunctionName`-shaped wrapper and set
      `config.resolve_function_name` in `c_config()`
      (`src/engine/configured_extractors.cpp:42-44`), mirroring
      `javascript_extractor.cpp:496`. The hook is consulted first at
      `extractor.cpp:59-60`, so nothing else needs to change.
- [x] 3.3 Confirm the fallthrough still works: if `declarator_name` returns empty
      for some construct, `label_for_node` must still reach the `name_fields` path
      rather than dropping the node (`extractor.cpp:59-66`). A silently dropped
      symbol is worse than an ugly label.
- [x] 3.4 Verify labels for: free function, method, constructor, destructor
      (`~Foo`), `operator==`, function returning a reference (today
      `'& lock_map_mutex()'`), templated function, and a multi-line signature
      (today `walk_node` at 345 chars / 11 lines). Add each to
      `tests/smoke/cpp_extractor_test.cpp`.
- [x] 3.5 Update `tests/smoke/cpp_extractor_test.cpp:126` — it hardcodes the buggy
      label: `has_edge(graph, "handle(const Payload& p, Service& s)", "Payload",
      "references")` becomes `"handle"`.

### Discovered while doing §3 (each was measured, not anticipated)

- [x] 3.6 Bare labels collide within a file: an overload set, a constructor
      sharing its class's name, `operator=` for copy and move. `add_symbol_node`
      (`src/engine/extractor.cpp`) now disambiguates a colliding id with the
      declaration's start line, which is deterministic for a given file.
- [x] 3.7 `semantic_dedup`'s exact pass keyed on `label + source_file` only, so an
      overload set collapsed to one node. The key now carries the declaration site
      (`src/engine/dedup.cpp`). A genuine double-extraction shares a site and still
      merges.
- [x] 3.8 The fuzzy pass was silently deleting real functions once labels got
      short: `validate_semantic_fragment_file` merged into `..._json`,
      `drainer_uninstall` into `drainer_installed`, `query_zero_hit_rate` into
      `query_zero_hits`, `supervisor_sync` into `supervisor_spec`. A long
      signature-bearing label had been providing accidental protection. Two nodes
      that each name a concrete declaration site are now never fuzzy-merged; the
      pass keeps working for site-less nodes, which is what it was for.
- [x] 3.9 Net effect measured: 0 symbols lost, 84 recovered (nodes 1337 -> 1421).
      Regression tests added for every case above, each verified to fail when its
      guard is removed.

## 4. Cause B — normalize the callee side

- [x] 4.1 Add `.call_member_node_types = {"field_expression"}` and
      `.call_member_field = "field"` to `c_config()`, modelled on `go_config()`
      (`:205-206`) and `csharp_config()` (`:90-91`).
- [x] 4.2 Verified in the vendored grammar, not assumed: `field_expression` is
      `field('operator', choice('.', '.*', '->'))` with `field('field', ...)`
      (`vendor/tree-sitter/grammars/cpp/grammar.js:1119`, and the C grammar at
      `:1202`), so one entry covers `.`, `->`, and `.*`.
- [x] 4.3 Added `LanguageConfig::call_scope_separator` (`"::"` for C/C++) and
      reduce a non-member callee label to its tail in `add_raw_call`. Done on the
      label rather than by a field lookup because `qualified_identifier` NESTS in
      tree-sitter-cpp (`field('name', choice(..., $.qualified_identifier, ...))`),
      so a single lookup would still leave a qualified name behind. A qualified
      call stays a NON-member call, because the qualification determines the name
      and project-wide resolution is sound -- unlike `obj.method()`.
- [x] 4.4 Assert member calls set `is_member_call` (`extractor.cpp:150-153`) and
      so stay excluded from project-wide matching (`graph_builder.cpp:396`). A
      member call escaping to project-wide matching would invent edges.
- [x] 4.5 Golden case: a struct with a method, called via `.` and via `->` from
      the same file, plus a `ns::free_function()` call, all produce `CALLS` edges.

## 5. Cause C — a call target must be callable

- [x] 5.1 Apply the per-file kind filter (`graph_builder.cpp:309`) to
      `label_index` (`:61-67`) or to the project-wide lookup. Exclude `field`;
      keep `function` and `class`.
- [x] 5.2 By the time this landed, Causes A and B had already re-resolved most of
      the original 12 to real functions, leaving 5 field-targeting edges:
      `unix_endpoint_is_live -> connect`, `request_over_unix_socket -> connect`,
      `raw_connect -> connect` (all three invoke the ::connect syscall),
      `status -> enrichment_state`, and `remember_checkpoint -> count`. All 5 are
      gone; 4 of those callee names now resolve to the real function instead, so
      total CALLS moved 1155 -> 1154 while field targets went 5 -> 0.

## 6. Make call resolution measurable

- [x] 6.1 Added a `CallResolution` struct in
      `src/engine/include/cgraph/operation_stats.hpp` (embedded in `BuildStats` as
      `calls`) with `total` plus the five outcomes, a `balances()` invariant, and
      `resolved_rate()`. `resolve_raw_calls` takes an optional out-param.
- [x] 6.2 Populate at each resolve and `continue` site in `resolve_raw_calls`
      (`src/engine/graph_builder.cpp:383-416`); serialize in
      `src/engine/operation_stats.cpp` so they reach `stats.json`. Assert the
      partition sums to `raw_calls_total`.
- [x] 6.3 `bench/run.sh:30-31` records `phase_ms` and the new counters, not just
      `node_count`/`edge_count`.

## 7. Prove the graph got better, not just bigger

- [x] 7.1 Re-run every §1.3 measurement. Report before/after for each.
- [x] 7.2 `ctest --preset default` green (64 tests) and `ctest --preset sanitizers`
      green. Golden diffs must reduce to: label changes from §3, the §5.2
      enumerated removals, the §2 reclassification, and added edges. Anything else
      is a defect.
- [x] 7.3 **The acceptance test.** All five must return real callers, not
      markdown: `impact` on `merge_fragments` names `pipeline.cpp` and
      `incremental_update.cpp`; `impact` on `run_one_shot` names `cli/main.cpp`
      and `semantic_orchestration.cpp`; find-callers on `handle_daemon_request`,
      `make_id`, and `publish_graph_snapshot` return non-empty. Quote the output.
- [x] 7.4 No longer routes through `namespace cgraph`. It routes through the
      containing file node (2 hops), which is shorter than the real call chain via
      `mutate_graph_snapshot` (3 hops). Honest structure rather than a tautology,
      but shortest-path still prefers containment over call chains where both
      exist -- a ranking question, recorded and not addressed here.
- [x] 7.5 Re-measure daemon read latency — baseline taken while planning was
      `status` 17 ms, `query` 19 ms, `context` 32 ms at 4k, 115 ms at 8k, including
      client spawn. A denser graph makes every full-scan read op more expensive; if
      this regresses materially, the snapshot index moves up the backlog.

## 8. Decide and document the id migration

- [x] 8.1 State in the proposal and PR that C/C++ node ids change, and that
      persisted incremental indexes rebuild once.
- [x] 8.2 Decide the fate of existing `graph_remember` checkpoints, whose
      `concerns` edges point at old C/C++ ids. Recommended: accept the orphaning —
      there is no release tag or packaged distribution yet — and say so explicitly.
      If not acceptable, this needs its own migration change first.
- [ ] 8.3 Verify a daemon running against a stale persisted index recovers rather
      than serving a half-migrated graph: start on the old index, confirm the
      rebuild path, confirm `freshness.verified` and `content_root` are correct
      afterwards.

## 9-10. Context budget and gate re-pin — MOVED OUT

Implemented and measured, then deliberately moved to its own change: enforcing the
stated budget erases the knapsack's advantage over greedy, because the advantage was
the overshoot. On the frozen fixture at 2k/4k/8k, knapsack goes 0.590/0.641/0.660
(over-packing) -> 0.445/0.532/0.624 (honest), against greedy's 0.462/0.544/0.620.
The two packers were never compared at the same real cost.

Re-pinning two committed gates to make an extraction PR pass would have buried that.
See `openspec/changes/honest-context-budget/` (proposal, tasks, delta spec, and
`reference-implementation.patch`) and `research/honest-context-budget/results.md`.

- [x] 9.1 Budget fix implemented and verified against a live daemon at budgets
      500-8000: overshoot 6.1x -> within budget at every size, every returned entry
      carrying a snippet. Preserved as a reviewable patch.
- [x] 9.2 Measured the consequence and wrote it up rather than absorbing it.
- [x] 9.3 Filed `honest-context-budget` with the decisions the follow-up owes:
      whether a snippet-less row counts as retrieved context, and whether knapsack
      should stay the default.
- [x] 10.1 The stale `retrieval_quality_test` pins move with it -- they need
      re-pinning regardless of the packer outcome, and belong in the same change.

## 11. Report

- [ ] 11.1 PR body carries before/after for: `CALLS` count, call-target kind
      histogram, function nodes with no incoming call, namespace-as-class counts,
      path-through-namespace share, the resolution counters, Debug-vs-Release
      timing, and the `graph_context` overshoot ratio at a 3000-token budget. Per
      CLAUDE.md, benchmarks belong in the PR description.
- [ ] 11.2 Quote the §7.3 acceptance output verbatim.
- [ ] 11.3 State that the retrieval effect of the call-graph fix is UNMEASURED,
      and why: both gates read a frozen pre-change graph, and regenerating the eval
      pair would rewrite the very node ids the labels are keyed on. Testing the
      "3-7-hop misses are missing-call-edge misses" hypothesis needs its own change
      that regenerates the eval pair and compares packer variants within the new
      graph.
