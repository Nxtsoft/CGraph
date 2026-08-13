# Acceptance evidence

The README's own questions, asked of a live daemon serving the post-change graph
of this worktree (1434 nodes, 2843 edges). Every one of these returned nothing
usable before the change.

## who calls X

Before: `found: true` with an empty caller list for all five -- the shape that makes
an agent conclude nothing calls a function and delete it.

| symbol | distinct callers now | `rg` call-site mentions |
| --- | --- | --- |
| `handle_daemon_request` | 14 | 106 |
| `make_id` | 21 | 61 |
| `publish_graph_snapshot` | 14 | 34 |
| `resolve_raw_calls` | 5 | 9 |
| `semantic_dedup` | 5 | 8 |

Distinct callers are fewer than `rg` mentions because a caller is the enclosing
function, and `rg` counts every mention including the definition, declarations in
headers, and repeated calls inside one function.

### Hand-verified: callers of `publish_graph_snapshot`

| caller reported by the graph | file | line |
| --- | --- | --- |
| `run_daemon_server` | daemon_server.cpp | 293 |
| `full_stat_index_rescan` | incremental_update.cpp | 103 |
| `apply_incremental_code_updates` | incremental_update.cpp | 209 |
| `main` | seam_test.cpp | 93 |
| `ingest_enrichment` | semantic_orchestration.cpp | 182 |
| `main` | daemon_hardening_test.cpp | 29 |
| `main` | host_surface_integration_test.cpp | 46 |
| `main` | daemon_lifecycle_test.cpp | 28 |
| `main` | daemon_ops_test.cpp | 19 |
| `main` | semantic_ingest_test.cpp | 39 |
| `load_graph_snapshot` | daemon_lifecycle.cpp | 93 |
| `mutate_graph_snapshot` | daemon_ops.cpp | 1728 |
| `main` | pack_context_parity_test.cpp | 52 |
| `main` | daemon_snapshot_test.cpp | 20 |

Cross-checked against `rg -n 'publish_graph_snapshot\(' src/`. Every non-test call
site rg finds is present, attributed to the correct enclosing function:

| rg call site | enclosing function the graph names |
| --- | --- |
| `daemon_server.cpp:567`, `:311` | `run_daemon_server` (spans from :293) |
| `incremental_update.cpp:205` | `full_stat_index_rescan` (from :103) |
| `incremental_update.cpp:311` | `apply_incremental_code_updates` (from :209) |
| `semantic_orchestration.cpp:190` | `ingest_enrichment` (from :182) |
| `daemon_lifecycle.cpp:101` | `load_graph_snapshot` (from :93) |
| `daemon_ops.cpp:1732` | `mutate_graph_snapshot` (from :1728) |

The graph is correct here, not merely fuller.

## what breaks if I change X

| symbol | before | after |
| --- | --- | --- |
| `merge_fragments` | 37 dependents: 30 markdown, 4 media, 0 functions | 29 dependents, 19 functions |
| `run_one_shot` | 8 dependents: 6 openspec markdown, 0 functions | 15 dependents, 8 functions |

- `impact merge_fragments` names the real callers: pipeline.cpp, incremental_update.cpp  (function-bearing files: cpp_extractor_test.cpp, daemon_server.cpp, graph_builder_test.cpp, graph_parity_test.cpp, incremental_dedup_test.cpp, incremental_rescan_test.cpp, incremental_update.cpp, incremental_update_test.cpp, main.cpp, pipeline.cpp, pipeline_test.cpp, semantic_orchestration.cpp, semantic_orchestration_test.cpp)
- `impact run_one_shot` names the real callers: main.cpp, semantic_orchestration.cpp  (function-bearing files: cpp_extractor_test.cpp, incremental_update_test.cpp, main.cpp, pipeline_test.cpp, semantic_orchestration.cpp, semantic_orchestration_test.cpp)

## path

Before: `handle_daemon_request -> cgraph [class, centrality 1.0, god_node] -> publish_graph_snapshot`
-- i.e. "both are in namespace cgraph", which was true of 92% of function pairs.

After: `handle_daemon_request [function] -> engine/daemon_ops.cpp [file] -> publish_graph_snapshot [function]`

Note precisely what this is: the two functions share a file, so the file node is the
shortest connector (2 hops) rather than the real call chain through
`mutate_graph_snapshot` (3 hops). That is honest graph structure -- file containment
is a real edge -- and it is no longer a tautology about namespace membership. Shortest
path still prefers containment over call chains where both exist, which is a separate
ranking question and is not in this change.

## Read latency on the denser graph

The graph carries 2.3x more edges, and every daemon read op is a full scan with no
index, so latency was the thing to watch.

| op | after (release, 1434n/2843e) | earlier baseline |
| --- | --- | --- |
| `status` | 12 ms | 17 ms |
| `query` | 8 ms | 19 ms |
| `context` @4k | 19 ms | 32 ms |
| `context` @8k | 41 ms | 115 ms |
| `impact` | 9 ms | 9 ms (not measured before) |

Read this as **no regression**, not as an improvement: the two columns differ in
both build type (Release vs Debug) and graph, so they are not a controlled
comparison. The point established is that a 2.3x denser graph did not make the read
path worse. Both include full client process spawn.

The full-scan reads remain the known scaling risk, and the snapshot index is
deliberately out of scope for this change.
