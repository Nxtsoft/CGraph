## Why

`graph_query` search (issue #16) ranks host-authored semantic-enrichment nodes
(`doc:`/`concept:`/`media:`/`topic:`) as if they were code symbols. `matching_nodes` /
`lexical_matches` exclude only the `memory:` namespace, so a `concept:` node whose prose mentions
a symbol appears in code search and, at equal centrality, can sort ABOVE the actual functions.
Observed: `query resolveRepo` returned the concept before `resolveRepoId`/`resolveRepoIdCached`.

(The severe form the issue first reported -- a concept *eclipsing* structural results entirely,
`total:1` -- was a separate dedup bug: a source-less concept merging into a sourced symbol and
deleting it. That is already fixed by the species guard in #22; the function now survives and an
exact query returns it via the entity route. This change addresses the remaining ranking issue.)

## What Changes

- Add `is_enrichment_node_id` (types.hpp): the `doc:`/`concept:`/`media:`/`topic:` namespaces --
  prose about the code, not code symbols.
- In `query_graph`'s search route, `stable_partition` the result list so structural nodes come
  before enrichment nodes, preserving the centrality/overlap order within each group. Enrichment
  nodes still appear (no eclipse) and `total` is unchanged; an exact structural match still routes
  to `entity` and is always returned.

## Impact

- A code search surfaces code first: `resolveRepo` -> the two functions, then the concept.
- No change to totals, exact-match routing, or which nodes are returned -- only ordering.
- **Touches:** `src/engine/include/cgraph/types.hpp`, `src/engine/daemon_ops.cpp`,
  `tests/smoke/daemon_ops_test.cpp`.

## Capabilities

### Modified Capabilities

- `graph-daemon-client` -- enrichment nodes rank below structural in query search.
