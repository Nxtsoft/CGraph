## Context

Four defects compound to empty the symbol layer of the graph for C and C++. They sit in three files that already contain every helper the fix needs:

- `src/engine/configured_extractors.cpp` — `c_config()` / `cpp_config()`, the declarative language configuration.
- `src/engine/cpp_extractor.cpp` — `declarator_name` and `name_tail`, both currently confined to the anonymous namespace.
- `src/engine/graph_builder.cpp` — `label_index` and `resolve_raw_calls`.

The extraction machinery is not at fault. `LanguageConfig` already has a `resolve_function_name` hook (`language_config.hpp:80`), `label_for_node` already consults it first (`extractor.cpp:59-60`), and `javascript_extractor.cpp:496` already uses it. C and C++ were simply never given one.

## Goals / Non-Goals

**Goals**

- A C/C++ function, method, or field label is the symbol's name.
- A call site's callee key is normalized the same way as a declaration label.
- A `CALLS` edge only ever targets something callable.
- A namespace is structure, not a type, and never a god node.
- The call-resolution rate is readable from `stats.json`.
- `tokens_used` on `context` is the true serialized cost, and the budget is a ceiling.

**Non-Goals**

- Type-aware receiver resolution for member calls. The receiver type is unknown, so member calls stay scoped to the caller's own file, exactly as Go's already do.
- Resolving overloads. Two declarations sharing a name resolve to nothing, and the ambiguity is counted rather than guessed.
- Any performance work beyond adding the `release` preset.

## Decisions

### Fix the label, not the comparison

Two options were weighed.

*Option 1* — leave labels alone, add a second index keyed on the leading symbol token, consulted when the full-label lookup misses. Node ids never change, so the parity contract holds trivially.

*Option 2, chosen* — install `resolve_function_name` so the label itself is the name.

Option 2 is less code: with bare labels, the existing lookup works unchanged and no resolution ladder is needed. It also fixes an agent-facing symptom Option 1 leaves in place — a 345-character, 11-line label ships today in every `explain`, `impact`, and `context` response. And the project's own rule ("no fallbacks, no band-aids; standardize at the source") rejects a compensating index over a wrong label.

The cost is an id migration, accepted and documented in the proposal.

### Reuse `declarator_name` rather than write a name extractor

`cpp_extractor.cpp:106-119` already does exactly the required job, including the cases that would otherwise need careful new code: `destructor_name`, `operator_name`, `qualified_identifier`, and descent through pointer, reference, array, and parenthesized declarators. It is promoted out of the anonymous namespace and declared in `include/cgraph/cpp_extractor.hpp` beside the existing handlers.

The wiring must preserve the fallthrough at `extractor.cpp:59-66`: when `resolve_function_name` returns empty, `label_for_node` must still reach the `name_fields` path. A silently dropped symbol is worse than an ugly label, so this is asserted rather than assumed.

### Order the work so each number is attributable

Cause D lands first. It is the smallest change and it removes the structural noise — 96 god-node classes and 416 spurious `method` edges — that would otherwise contaminate the before/after call-edge measurement. Causes A and B land together, because each blocks the other: bare labels with unnormalized callees still miss every qualified and member call, and normalized callees with declaration-text labels still miss every free function. Cause C lands with them, because it is a precondition for A rather than an independent cleanup.

### Namespaces keep a node, lose the type

Dropping namespace nodes entirely would lose real structure. Instead a namespace gets its own kind, and `add_containment_edge` maps it to `contains` rather than `method`. Exclusion from centrality and god-node ranking follows the pattern already established for session-memory nodes at `analysis.cpp:219`, `:254`, and `:266-267`.

### The retrieval gates cannot measure this change, and that is left honest

Both retrieval gates read a frozen fixture, not a live build — `retrieval_quality_test.cpp:36-37` and `pack_context_parity_test.cpp:56-57` load `tests/fixtures/pack_context_parity/{graph.json,queries.jsonl}`, and `pack_context_parity_test.cpp:54` states the intent outright: "NOT the mutable cgraph-out/graph.json -- so the gate is drift-immune".

Two consequences, both accepted rather than worked around:

1. **This change cannot break them.** The frozen graph is unaffected by any extraction change, so both gates keep measuring the packer on a stable graph. That is exactly their job.
2. **This change cannot be credited by them either.** A call-graph improvement is invisible to a gate that reads a pre-change graph.

Regenerating the fixture is not the answer. The eval labels are node ids — `queries.jsonl` grades rows against ids like `..._daemon_ops_cpp_status_const_daemonstate_state_const_graphsnapshot_graph`, which are signature-bearing and which this change rewrites. Regenerating both halves through `scripts/bootstrap_eval.py` would produce a *different graph and different labels*, so the resulting recall number would not be comparable to the pre-change one. The repo's own research discipline names this trap: absolute metrics are only comparable within the same graph.

So: the recall effect of the call-graph fix is reported as **unmeasured**, with the reason stated. The gate re-pin (task 10) is still done, but it is scoped to what it actually covers — the packer and the budget change — and it is independent of the extraction work.

If the retrieval effect needs measuring later, it wants its own change: regenerate the eval pair on the post-fix graph, then compare packer variants *within* that graph.

### The knapsack keeps its ranking weight

`daemon_ops.cpp:309-313` records why the knapsack weighs the capped source slice and not the serialized JSON entry: including the overhead flattens the weight spread and degenerates the knapsack toward greedy. That reasoning is sound and the weight is left alone. The defect is only that the same number is then reported as `tokens_used`. So the selected set is re-costed at true serialized size after backtracking — the way the greedy path already does at `:1129` — and lowest-value entries are dropped until it fits.

This also resolves the composition problem without a separate rule. Today 65% of entries carry no snippet and the knapsack treats them as nearly free, while each still ships a ~200-byte mangled id and an absolute path. Once they pay their real cost, they are squeezed out on their own.

## Risks / Trade-offs

- **Id migration** (see proposal). Mitigation: verify a daemon started against a stale persisted index rebuilds rather than serving a half-migrated graph.
- **Overload collapse.** Bare labels merge overloads into one key, and the existing ambiguity rule (`graph_builder.cpp:314-316`) clears the slot, so overloads resolve to nothing rather than wrongly. Correct but lossy. Of 476 distinct C/C++ bare names, 439 (92%) are already unique, and `dropped_ambiguous` makes the residue visible.
- **Standard-library collisions.** Member-call capture will produce many callee names like `push_back`, `find`, and `size` that no project node declares. These resolve to nothing and land in `dropped_unknown`. This is why 1,609 new member and qualified call sites will not become 1,609 edges, and why the counters matter more than the raw edge count.
- **Denser graph, slower reads.** Every read op is a full scan. If latency regresses materially against the recorded baseline, the snapshot index moves up the backlog — it is deliberately not in this change.
- **Deliberate golden edits.** Cause C deletes 12 existing edges and Cause D reclassifies ~416. Every removal is enumerated and justified; an unexplained diff is treated as a defect, not an update.

## Migration Plan

1. Land the `release` preset and record the baseline census, so every later number has a before.
2. Cause D, then re-census.
3. Causes A, B, and C together, then re-census.
4. Counters, then confirm the partition sums.
5. Acceptance test against the live daemon: the README's own questions must return real callers.
6. Context budget, then the re-pinned recall gate.

## Open Questions

- Should existing `graph_remember` checkpoints be migrated rather than orphaned? Recommendation is to accept the orphaning, since there is no release tag or packaged distribution. If not acceptable, a checkpoint-migration change must land first.
- ~~Should the namespace node keep a distinct kind (`module`) or be dropped entirely?~~ **Resolved: dropped entirely.** Node ids are per-file, so a `namespace cgraph { }` block in 96 files mints 96 separate nodes all labelled `cgraph` — it never grouped anything across files, which is the only thing a namespace node could have been for. The file node already anchors containment, and `label_for_node` already documents the skip path: when no node is emitted "the enclosing scope is unchanged, and the construct's members are still walked and attached to the real scope that contains it" (`extractor.cpp:69-77`). So removing `namespace_definition` from `class_node_types` needs no new config field, no new kind, and no centrality guard in `analysis.cpp` — there is no node left to guard. Members attach directly to their file with `contains`.
