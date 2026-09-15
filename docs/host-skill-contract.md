# Host Skill Contract

This contract defines how host integrations use the native graph daemon without embedding model-provider logic in the binary. Hosts own model selection, token spend, and agent dispatch. The native tool owns deterministic graph commands, chunk planning, fragment validation, and local graph mutation.

## Deterministic Graph Commands

Hosts call the thin client or daemon protocol for graph operations. Requests use the current length-prefixed JSON frame format when talking to the daemon directly.

Supported operations:

- `query`: Search graph nodes by query text (routed by deterministic intent: structural / entity / search).
- `path`: Return a graph path between source and target ids.
- `explain`: Return a node and its neighboring edges, optionally filtered by `relation`.
- `impact`: Return the reverse-dependency radius of a node.
- `context`: Gather and pack a token-budgeted context window around a focal query.
- `update`: Apply deterministic updates. `update .` means full stat-index rescan.
- `status`: Return daemon, graph, cache, and enrichment state.
- `shutdown`: Ask the per-root daemon to exit cleanly.
- `remember`: Persist a session-memory checkpoint (title, tags, markdown body).
- `recall`: List or filter persisted checkpoints.
- `report`: Structural report sized to a token budget. `view: "modules"` (the only implemented
  view; `design`, `clones`, `types` answer `code: "report_view_not_implemented"`) groups files
  into modules by directory depth, aggregates imports/calls between them, ranks layers by
  longest path, lists cycles, and renders `json` | `mermaid` | `markdown` | `svg`. Whole rows are
  shed to fit `budget`; `omitted` always reports how many. A daemon that predates the op answers
  `unknown op: report`; hosts should surface that as "upgrade the daemon". Hosts also call
  `graph_report` when asked for the architecture or a module map.

Hosts should prefer the thin client command surface unless they are implementing an MCP or always-on bridge that already speaks local JSON frames.

## Cross-Service Seam Graphs

The CLI additionally ships `cgraph seam gen|fuse|query` for cross-service seam graphs: `gen`
emits a standard node-link enrichment fragment describing cross-service call seams, `fuse`
builds a fused multi-repo render, and a fused seam directory can be served resident by the
daemon. Seam fragments follow the same fragment schema as semantic enrichment drops.

## Chunk Plan Dispatch

The native tool emits semantic chunk plans for uncached or stale documentation, media, and semantic inputs. Code extraction stays deterministic and does not require host model work.

Each chunk contains bounded file inputs with:

- source path
- input kind, such as document or media
- content hash
- byte size
- `candidate_links`: real code-node ids (with labels) that the document mentions, matched
  deterministically against the code graph. These are suggestions only — ids and labels as
  evidence, never a relation — so a host can emit `doc -> <code-node-id>` edges connecting prose to
  code without first discovering the ids itself. The array is empty for media inputs and when no
  code graph is available to match against; ignoring it leaves a fragment valid.

Hosts dispatch each chunk to their own agent or model workflow. A completed chunk writes exactly one fragment file named `chunk_NN.json` into the configured semantic drop directory, where `NN` is the chunk index. Cached content is skipped when a valid cache record exists for the same content hash and fragment path.

## Semantic Fragment Schema

Dropped fragments must use the node-link fragment shape:

- `nodes`: array of node objects with required `id` and `label`
- `edges`: array of edge objects with required `source`, `target`, and `relation`
- `hyperedges`: optional array of hyperedge objects with `id`, `nodes`, and `relation`
- optional `source_file`, `source_location`, `type` or `kind`, `confidence`, `confidence_score`, `properties`, and `warnings`

The native daemon validates every dropped fragment before graph mutation. Malformed JSON or schema violations are rejected and must not alter the graph snapshot. Valid fragments are merged through the daemon single-writer path and update the semantic cache by content hash.

## Disk Success Signals

Hosts signal completion by writing a complete `chunk_NN.json` file to the semantic drop directory. The native watcher discovers created or modified `chunk_NN.json` files and ignores unrelated names.

Recommended host write sequence:

- write to a temporary file in the same directory
- flush and close the file
- atomically rename it to `chunk_NN.json`

Native success is observable through:

- `status.enrichment_state`
- `status.enrichment_pending`
- `status.enrichment_running`
- `status.enrichment_stale`
- `status.enrichment_failed`
- the semantic cache record for the source content hash
- the graph snapshot containing the merged fragment nodes and edges

Failure is observable through rejected validation errors, failed enrichment counts, and unchanged graph snapshots. Hosts should retry by writing a corrected `chunk_NN.json` file with the same chunk index.

## Reference Driver

`integrations/skills/cgraph-enrich` is the reference host driver for this contract: it runs the plan → author → atomic drop → verify loop described above. Install it (and the query-routing `cgraph` skill) into host skill directories with `cgraph skills install`; keep pending enrichment drained on a schedule with `cgraph drain install` (status-gated, chunk-capped, host CLI dispatch — no model logic in the binary).
