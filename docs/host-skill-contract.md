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
- `report`: Structural report sized to a token budget. `view: "modules"` groups files into
  modules by directory depth, aggregates imports/calls between them, ranks layers by longest
  path, lists cycles, and renders `json` | `mermaid` | `markdown` | `svg`. `view: "types"` audits
  type definitions (`class`/`type` nodes, members = the labels of the `field` nodes they
  `defines`): `identical` (groups of differently named types with exactly the same member set),
  `duplicates` (one name declared in several files, with the lowest/highest member-set Jaccard
  between the declarations), `overlaps` (pairs that nest with the smaller at least half of the
  larger, or are at least `threshold` Jaccard-similar, default 0.80; only types with at least
  `min_members` members, default 3, take part) and `unreferenced` (no non-structural edge into
  the type from anything in the graph; same-file use is not an edge, so this is a lead, not a
  verdict); it renders `json` | `markdown`, and a diagram format answers
  `code: "report_format_unsupported"`. `view: "clones"` groups functions whose bodies are at
  least `threshold` similar (default 0.80) after identifiers and literals are normalized
  (rename-insensitive fingerprints computed at extraction, compared by Jaccard over winnowed
  5-token shingles) into `classes` with members' file:line-line, lowest pairwise `similarity` and
  shortest body `tokens`; bodies under `min_tokens` (default 30) are skipped; classes whose members
  all lie under test roots are `test_classes` unless `include_tests`; it renders `json` |
  `markdown`; a `hint` says when functions lack fingerprints (a graph fast-loaded from a persist
  written before fingerprints existed) and that `update .` computes them. `view: "design"` lists
  `entry_points` -- `main`, HTTP `route` handlers (inline `<x>.<verb>('/path', handler)` and
  Next.js `app/**/route.ts` exports), framework `page` files (`app/**/page.tsx`, `layout.tsx`,
  `pages/**`), and `root` functions with callees that nothing in the graph calls -- ranked by
  `reach` (functions transitively called over `CALLS`/`dispatches_to`), each with its top call
  `flow` to `hops` (default 3; four children per node by reach, the rest counted in `more`),
  `layers` (functions per shortest call distance from an entry point, with the modules that hold
  them) and the `unreached` count; it renders `json` | `mermaid` (`flowchart TD`) | `markdown`,
  and `svg` answers `code: "report_format_unsupported"`. Whole rows are shed to fit `budget`;
  `omitted` always reports how many. A daemon that predates the op answers `unknown op: report`;
  hosts should surface that as "upgrade the daemon". Hosts also call `graph_report` when asked for
  the architecture or a module map, for type bloat, duplicate interfaces/structs, or dead types,
  for copy-pasted or duplicated logic, and for how the program is entered and flows. The `modules`
  view names its modules after the repository's own workspace packages when the root declares a
  workspace (npm/pnpm/Cargo/go.work), else by directory depth; the response's `group_by`,
  `manifest` and `packages` say which, and the request's `group_by` (`auto`, `packages`, `depth`)
  selects.

Hosts should prefer the thin client command surface unless they are implementing an MCP or always-on bridge that already speaks local JSON frames.

## Cross-Service Seam Graphs

The CLI additionally ships `cgraph seam gen|discover|fuse|query` for cross-service seam graphs:
`gen` emits a standard node-link enrichment fragment from a host-authored seam spec, `discover`
emits the same fragment from the `endpoint` nodes each graph already serves (`handled_by`),
consumes (`CONSUMES`) and documents (OpenAPI, proto, GraphQL or openapi-typescript files,
`documented: true`) with no spec (endpoints join by their repo-free canonical id) and reports
contract drift between documents and code, `fuse`
builds a fused multi-repo render, and a fused seam directory can be served resident by the
daemon. Seam fragments follow the same fragment schema as semantic enrichment drops.

## Workspaces

A seam is an artifact; a workspace is live. When the root a host passes holds
`cgraph.workspace.json` (written by `cgraph workspace init`, naming member repositories), the
thin client, the CLI and the MCP server answer across those repositories with no new tool: each
is asked exactly as a lone project would be, keeping its own daemon, watcher and incremental
updates. `impact` and `path` cross between repositories at the `endpoint:` contract nodes they
share, one hop per contract, and every returned node carries the `repo` it came from. A
repository whose daemon cannot be reached is listed in `unreachable` rather than omitted.
`report`, `context` and the session-memory ops are answered per project: at a workspace root they
return `ok:false` with `code: "workspace_op_unsupported"` and the repository roots to use.

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
  code graph is available to match against; ignoring it leaves a fragment valid. Code-node ids
  derive from the source path relative to the project root, so an id a host records is valid in
  every checkout of the same tree. Ids recorded before this change (openspec change
  `relative-node-ids`; the first release carrying it is the one after `bin-v0.4.0`) embedded the
  absolute path and no longer resolve.

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
