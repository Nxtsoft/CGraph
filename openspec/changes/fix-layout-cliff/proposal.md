# Fix: one layout algorithm at every graph size (the 2,000-node DrL cliff)

## Why

`write_layout` (`analysis.cpp`) precomputes the graph.html layout on the igraph object built for
community detection. Since 04d49d5 it chose the algorithm by node count: Fruchterman-Reingold
below 2,000 nodes, DrL at or above, on the stated assumption that "a Fruchterman-Reingold pass
would be prohibitively slow" for large graphs.

Measured while benchmarking CGR-11 (#87), the assumption is inverted on the vendored igraph:

| graph | nodes | algorithm chosen | `communities_ms` (covers layout) |
|---|---:|---|---:|
| CGraph at d917ec1 | 1,948 | FR | 254 ms |
| CGraph + `report.cpp`/`report.hpp` from #87 | 2,007 | DrL | 12,755 ms |
| CGraph at 495ef1c (#87 merged) | 2,022 | DrL | 13,400-16,900 ms |
| same tree, threshold disabled | 2,022 | FR | 297 ms |
| `frontend` (Next.js) | 17,439 | DrL | 226,850 ms of a 253,087 ms build |
| `frontend`, threshold disabled | 17,439 | FR | 2,864 ms of a 26,761 ms build |
| `turing-api` (Elysia) | 7,267 | DrL | 74,476 ms of a 77,496 ms build |
| `turing-api`, threshold disabled | 7,267 | FR | 1,195 ms of a 4,895 ms build |

Three consequences. A repository's one-shot build time jumps fifty-fold the day its graph
crosses 2,000 nodes -- CGraph's own did in #87 (0.86 s to 13-17 s) with no change to
extraction, resolution or dedup, all in layout. The `frontend` daemon's initial build and every
`update .` rescan (measured at 160-170 s during CGR-9/10/11) were ~90% layout. And the
`communities_ms` phase timer hides it, because layout runs inside `detect_communities`.

## What Changes

- `write_layout` runs Fruchterman-Reingold with `IGRAPH_LAYOUT_AUTOGRID` at every size (igraph's
  grid approximation makes the pass O(n) per iteration above 1,000 vertices). The DrL branch and
  `kDrlThreshold` are removed; `kLayoutIterations = 500` moves to `analysis.hpp` beside the
  canvas constants so the layout policy has one home.
- `analysis_test.cpp` lays out a 2,500-node ring-plus-chords graph and asserts every node has
  finite canvas-scale coordinates spanning the sqrt(n) side. It crosses the old threshold; the
  whole test now runs in well under a second, where DrL took 12-20 s at this size.
- Spec: the interactive-view requirement states the layout SHALL be computed by one algorithm
  regardless of node count, so a size threshold cannot be reintroduced without changing it.

### Non-goals
- A separate `layout_ms` phase timer. Worth doing, but it changes `stats.json` and the
  op-stats ledger; tracked as its own follow-up.
- Tuning FR (iterations, temperature) or the canvas scale: the 500-iteration pass is what every
  graph under 2,000 nodes has shipped with; this change only stops switching away from it.

## Impact

- **Touches:** `src/engine/analysis.cpp`, `src/engine/include/cgraph/analysis.hpp`,
  `tests/smoke/analysis_test.cpp`.
- `graph.json` coordinates change for graphs of 2,000 nodes or more (FR positions instead of
  DrL positions); the node-link topology, ids and every other property are unchanged. Graphs
  under 2,000 nodes are byte-identical.
- Build time, one-shot, same machine and sources: CGraph's own tree (2,022 nodes) 13.4-16.9 s ->
  1.0 s; `frontend` (17,439 nodes) 253 s -> 26.8 s, of which layout 227 s -> 2.9 s; `turing-api`
  (7,267 nodes) 77.5 s -> 4.9 s. The daemon's
  initial build and every full rescan of `frontend` drop by the same ~224 s. For graphs under
  2,000 nodes nothing changes (FR was already chosen); DrL on those would have cost 11 s at 1,556
  nodes and 15 s at 1,948, measured by forcing it.

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — the precomputed layout uses one algorithm at every size.
