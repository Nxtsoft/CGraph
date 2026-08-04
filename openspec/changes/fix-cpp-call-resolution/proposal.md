## Why

CGraph's stated purpose is to answer "what calls this?" and "what breaks if I change it?". On its own C++ source it answers neither, and `path` answers a third question wrongly.

Measured against the live daemon on this repo (171 files, 1425 nodes, 1865 edges):

| Question | Ground truth | Returned |
| --- | --- | --- |
| who calls `handle_daemon_request`? | 106 call sites | `found: true`, 0 callers |
| who calls `make_id`? | 61 call sites | `found: true`, 0 callers |
| who calls `publish_graph_snapshot`? | 34 call sites | `found: true`, 0 callers |
| what breaks if I change `merge_fragments`? | `pipeline.cpp:57`, `incremental_update.cpp:48` | 37 dependents: 30 markdown, 4 media, 0 functions |
| what breaks if I change `run_one_shot`? | `cli/main.cpp:95`, `semantic_orchestration.cpp:187` | 8 dependents: 6 openspec markdown, 0 functions |
| how do `handle_daemon_request` and `publish_graph_snapshot` relate? | via `mutate_graph_snapshot` | "both are in `namespace cgraph`" |

`found: true` with an empty caller list is the dangerous shape: an agent reads it as "nothing calls this" and deletes the function. An error would at least send it back to grep.

Aggregate: 120 `CALLS` edges across 657 function nodes. 583 of 657 (89%) have no incoming call edge; 441 of 657 (67%) have no call, reference, or import edge at all; mean semantic degree 0.53.

Four independent causes, all confirmed by measurement:

**A. C/C++ labels carry the entire declarator.** `c_config()` sets `.name_fields = {"name", "declarator"}` and a tree-sitter `function_definition` has no `name` field, so `label_for_node` falls through to the declarator's raw text. No `resolve_function_name` is installed for C/C++. Labels become `run_one_shot(const std::filesystem::path& root)`, `& lock_map_mutex()` (return-type ampersand leaked in), and for `walk_node` a 345-character, 11-line string. Call sites record the bare callee identifier, and resolution compares the two through `make_id`, so they never agree. 509 of 617 C/C++ function nodes (82%) carry a parameter list and are unreachable as a call target. The proof: of the 77 C++ call edges that do resolve, 60 (78%) target a zero-argument function, though only 18% of C++ functions are zero-argument — `main()` normalizes to `main` and matches by accident. Python is unaffected because `python_extractor.cpp` sets `.name_fields = {"name"}`.

**B. The callee side is never normalized either.** `c_config()` sets `.call_accessor_fields = {"function"}` and nothing more. Go sets `.call_member_node_types = {"selector_expression"}` / `.call_member_field = "field"`; C# sets `{"member_access_expression"}` / `"name"`. With neither set, `add_raw_call` records the verbatim callee text: `cgraph::run_one_shot(...)` becomes `"cgraph::run_one_shot"` and `state.stats.record(...)` becomes `"state.stats.record"`. Measured over `src/` and `tests/`: 786 qualified plus 823 member call sites whose callee names a project C++ function. A and B each block the other.

**C. The project-wide index has no kind filter.** The per-file table filters candidates to callable kinds, but `label_index` indexes every node and the project-wide lookup filters nothing. Of 120 `CALLS` edges, 12 target `field` nodes — `unix_endpoint_is_live` "calls" a struct field named `connect` because it invokes the `::connect` syscall. Fixing A widens the key space, which would multiply these, so C is a precondition rather than a cleanup.

**D. A namespace is classified as a class.** `cpp_config()` pushes `namespace_definition` into `class_node_types`. Ids are per-file, so every file containing `namespace cgraph { }` mints its own `cgraph` "class", and `add_containment_edge` labels its children `method`. Measured: 96 of 214 class nodes (44%) are labelled `cgraph`; 416 of 449 `method` edges (92%) originate at one; the highest-degree node in the entire graph is `class 'cgraph'` at degree 45 with `centrality: 1.0, god_node: true`; and over 300 random function pairs, 243 of 266 connected pairs (91%) route their shortest path through a namespace-as-class node. This also corrupts the centrality ranking that orders `query` results.

This is also the retrieval ceiling. `research/ceiling-diagnostic/results.md` found 0% of missed context is disconnected and 100% is 3-7 hops away: relevant code is far because the edges that would make it near do not exist, and the paths that do exist detour through a namespace god node.

Two adjacent defects on the same agent-facing surface:

**The `context` budget is not honest.** With the default `gather=adaptive`, a `budget: 3000` request returned 73,590 bytes (~18,397 estimated tokens) while self-reporting `tokens_used: 2999`. 104 of 159 entries (65%) carried no snippet, no line, and no location. Through MCP a response that size is refused as over the client token cap, so the agent receives nothing. The knapsack weighs snippet text only by deliberate design, which is a defensible ranking choice, but `tokens_used` is computed from those same weights and never corrected. The non-default greedy path is honest by construction.

**Nothing in this repository is compiled with optimization.** `"CMAKE_BUILD_TYPE": "Debug"` in `CMakePresets.json` is the only build type present; engine translation units carry no `-O` flag. A Release build of the same tree measured 449 ms against 123 ms, a 3.65x speedup, producing a byte-identical `graph.json`. Every published benchmark and every binary a user installs today is unoptimized.

## What Changes

- Install `resolve_function_name` for C and C++ so a function label is the symbol's name, reusing the existing `declarator_name` helper, which already descends pointer/reference/array/parenthesized wrappers and already reduces qualified identifiers, destructor names, and operator names to their tail.
- Give C and C++ the member-call configuration Go and C# already have, and reduce a qualified-identifier callee to its tail, so `obj.method()`, `ptr->method()`, and `ns::free_function()` resolve.
- Restrict project-wide call resolution to callable kinds, excluding `field` while keeping `function` and `class`, because `Foo()` is a genuine constructor call in Python and JavaScript.
- Stop classifying a C++ `namespace_definition` as a class; emit namespace containment as `contains` rather than `method`, and exclude namespace nodes from degree centrality and god-node ranking.
- Report a partition of raw call outcomes in `BuildStats` and `stats.json`, so the resolution rate is measurable from a committed artifact rather than requiring an instrumented build.
- Report the true serialized cost of a `context` bundle as `tokens_used`, never exceed the requested budget, and mark entries whose snippet could not be read in every packing mode.
- Add a `release` preset, build and test it in CI, and point the documented build at it.
- Re-pin the retrieval-quality gate, whose baselines were orphaned by a fixture rewrite and now tolerate a 57% recall regression.
- Non-goals: gating `write_layout` off the daemon path, building a snapshot index for the read ops, reviving Kotlin extraction, fixing Python import orphans or the five missing import handlers, JSX component edges, wiring `peer_is_authorized` into the accept path, packaging, and the MCP specification catch-up.

## Impact

Changing a C/C++ label changes its node id, because a node id is derived from `source_file + ":" + label`. This is a one-time migration and the primary risk:

- Persisted incremental indexes invalidate and rebuild once. Self-healing.
- Goldens referencing a C++ id change. `tests/smoke/cpp_extractor_test.cpp` currently hardcodes a buggy label as its expected value.
- Existing `graph_remember` checkpoints orphan, because their `concerns` edges point at old ids. With no release tag and no packaged distribution yet, accepting the break is reasonable, but it is recorded here rather than discovered later.

On the Graphify parity contract: bare labels move C and C++ **toward** parity, because Python, JavaScript, TypeScript, and every other configuration already emit bare names. The alternative considered and rejected was leaving labels alone and adding a second bare-name index at resolution time; that preserves ids but is a compensating layer over a wrong label, leaves 345-character labels in every agent-facing response, and contradicts the project's rule about standardizing at the source.

Also expected: `CALLS` edge growth makes every daemon read op more expensive, because each is a full scan with no index. The baseline to compare against is `status` 17 ms, `query` 19 ms, `context` 32 ms at a 4k budget and 115 ms at 8k, including client spawn.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `deterministic-graph-pipeline` — symbol labels, call resolution, callable call targets, namespace classification, call-resolution telemetry.
- `graph-daemon-client` — the `context` token budget contract.
- `reproducible-ci-dependencies` — an optimized build exists and is covered by CI.
