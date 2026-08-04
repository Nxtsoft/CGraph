# graph-daemon-client Specification

## Purpose
TBD - created by archiving change persist-op-stats-ledger. Update Purpose after archive.
## Requirements
### Requirement: Durable op-stats ledger flush on shutdown
The daemon SHALL, on leaving its serve loop for any reason (idle timeout, `shutdown` op, or clean
termination), append exactly one JSON line describing the lifetime's accumulated op-stats to
`cgraph-out/op-stats-ledger.jsonl` in the project's output directory, beside the existing on-exit
graph persist. The line SHALL record wall-clock `boot` and `shutdown` timestamps, `uptime_seconds`,
the query zero-hit count, and per-operation `count`, `total_ms`, and a fixed-layout latency
histogram. The flush SHALL be best-effort: a failure to write SHALL be logged and discarded and
SHALL NOT block, delay, or abort shutdown, nor affect the graph persist that shares the teardown.
The flush SHALL be gated on at least one substantive operation (query, path, explain, impact, or
context) in the lifetime; a lifetime with only `status`/`shutdown` ops SHALL write no line. The
ledger SHALL be append-only — each lifetime adds one line and the file is never rewritten. The live
since-boot `status` path SHALL be unchanged.

#### Scenario: A lifetime with query activity appends one ledger line
- **WHEN** a daemon serves one or more substantive ops and then shuts down
- **THEN** exactly one new JSON line is appended to `cgraph-out/op-stats-ledger.jsonl`, it parses as
  JSON, and its per-op `count`/`total_ms` equal the lifetime's accumulated op-stats

#### Scenario: Idle, work-free lifetimes write nothing
- **WHEN** a daemon spawns, answers only `status`/`shutdown` ops, and idle-shuts-down
- **THEN** no line is appended to the ledger

#### Scenario: Flush never breaks shutdown
- **WHEN** the ledger path is unwritable at shutdown
- **THEN** the failure is logged, the daemon still completes its graph persist and exits cleanly,
  and no exception escapes the teardown

#### Scenario: Append-only, crash-tolerant
- **WHEN** the ledger already contains lines from prior lifetimes
- **THEN** the new line is appended without rewriting existing lines, and a reader that encounters a
  malformed trailing line skips it and still parses every well-formed line

### Requirement: Wall-clock timestamps derived from the monotonic substrate
The ledger line's `boot` and `shutdown` timestamps SHALL be wall-clock (ISO-8601 UTC), but the
running daemon's timing substrate SHALL remain purely monotonic. The implementation SHALL read the
system wall clock at most once per lifetime, at flush time, to obtain `shutdown`, and SHALL derive
`boot` as `shutdown − (monotonic_now − start_time)`. No wall-clock read SHALL occur in per-operation
recording or in the scoped phase timers.

#### Scenario: Boot is derived, not measured live
- **WHEN** a ledger line is produced at flush
- **THEN** `boot` equals `shutdown` minus the monotonic uptime, `shutdown >= boot`, and
  `uptime_seconds` equals the monotonic elapsed time since daemon start

#### Scenario: Substrate stays monotonic
- **WHEN** operations are recorded during the daemon's lifetime
- **THEN** no system wall clock is read on the per-op recording path; only the flush reads it, once

### Requirement: Versioned latency histogram
Each ledger line SHALL carry a `schema_version` and, per operation, a latency histogram over a
pinned, log-spaced bucket layout fixed by that version (v1: upper bounds in milliseconds of
`[1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024]` plus one overflow bucket — 12 counts). A latency
SHALL fall in the bucket whose range contains it, with latencies at or above the top bound counted
in the overflow bucket. Changing the bucket layout SHALL bump `schema_version`; consumers SHALL
treat histograms of different versions as non-mergeable.

#### Scenario: Latencies bucket deterministically
- **WHEN** operations with latencies of 0.4 ms, 1 ms, 1.5 ms, and 4000 ms are recorded
- **THEN** they land respectively in bucket 0 ([0,1)), bucket 1 ([1,2)), bucket 1 ([1,2)), and the
  overflow bucket, and the per-op histogram counts sum to that op's `count`

#### Scenario: Version pins the layout
- **WHEN** a ledger line is produced
- **THEN** it includes `schema_version` equal to the current ledger schema version and a histogram
  whose bucket count matches that version's pinned layout

### Requirement: Cross-session op-stats rollup
The CLI SHALL provide a `stats` subcommand — `cgraph stats [--root PATH] [--since today|<ISO>|
<duration>]` — that reads a service's `op-stats-ledger.jsonl`, selects lifetimes whose `shutdown`
falls within the window (default: today), and reports an aggregate across them: summed per-operation
counts, overall query zero-hit rate, weighted-mean latency, and approximate p50/p90 latency merged
from same-version histograms. The headline SHALL lead with per-operation query counts and the
zero-hit rate (the usefulness signal), with latency percentiles secondary. Percentiles derived from
merged histograms SHALL be labeled approximate; summed counts and weighted means are exact. When the
selected window mixes histogram `schema_version`s, the rollup SHALL report that rather than merge
incomparable buckets. When the subcommand can reach a live daemon for the same root, it SHALL also
show that daemon's since-boot stats in a clearly separate section, so live and durable figures are
never conflated.

#### Scenario: Rollup sums across lifetimes within the window
- **WHEN** the ledger holds three lifetimes within today's window with query counts 20, 22, and 12
- **THEN** `cgraph stats --since today` reports a total query count of 54 and a zero-hit rate equal
  to the summed zero-hit queries over 54

#### Scenario: Out-of-window lifetimes are excluded
- **WHEN** a lifetime shut down before the `--since` boundary
- **THEN** its counts are excluded from the rollup totals

#### Scenario: Live and durable are labeled distinctly
- **WHEN** a daemon for the root is running and the ledger has prior lifetimes
- **THEN** the output shows a live since-boot section and a durable ledger section under distinct
  labels, and the durable section's window is stated

#### Scenario: Percentiles are honest about precision
- **WHEN** the rollup reports latency percentiles merged from histograms
- **THEN** they are labeled approximate, while summed counts and weighted-mean latency are exact;
  and a window mixing `schema_version`s is reported rather than silently merged

### Requirement: Adaptive relevance-gated context gathering
The `pack_context` path SHALL support a `gather` parameter (`"adaptive"` **default**, `"fixed"`) that
changes which candidates the BFS collects and adds an additive gather-reach summary to the response
(see "Adaptive responses report gather reach"); the per-entry focus/included/omitted shape SHALL be
otherwise identical between `fixed` and `adaptive`. Under `gather = "fixed"` the gathered candidate
set and the existing response fields SHALL be byte-for-byte unchanged from the historical fixed
k-hop BFS (the pre-flip default), so callers can opt back into it exactly. Under `gather =
"adaptive"` (the default) the gather SHALL expand all nodes at depth 0 and 1 unconditionally
(preserving the full 2-hop core), and SHALL expand a node at depth ≥ 2 only when its query
lexical-overlap is ≥ `gather_theta` (default 0.05), to a maximum depth of 3, so that the third hop is
taken only along query-relevant paths. Adaptive gather SHALL use the existing knapsack fill and the
existing deterministic relevance signal; it SHALL introduce no model or LLM.

#### Scenario: Default gather is adaptive
- **WHEN** a `context` request omits `gather`
- **THEN** the gather is `"adaptive"` (knapsack packing, depth 3, θ=0.05 gate) and the response
  reports `gather: "adaptive"`

#### Scenario: Explicit fixed gather is unchanged
- **WHEN** a `context` request sets `gather = "fixed"`
- **THEN** the gathered candidate set and the pre-existing response fields are byte-for-byte
  identical to the historical fixed k-hop behavior for the same focal, budget, and packing

#### Scenario: Adaptive keeps the 2-hop core and gates the third hop
- **WHEN** a `context` request runs adaptive gather with a `gather_theta`
- **THEN** every node within 2 hops of the focal is still gathered, and a node at depth 2 expands
  its depth-3 neighbors only if its query lexical-overlap is ≥ `gather_theta`; a depth-2 node below
  the threshold contributes no depth-3 neighbors

#### Scenario: Adaptive reaches beyond two hops for less than full three-hop cost
- **WHEN** adaptive gather runs on the evaluation set versus a fixed 2-hop and fixed 3-hop gather
- **THEN** its candidate set reaches relevant symbols a 2-hop gather misses, at a candidate-token
  cost far below the full 3-hop gather

### Requirement: In-engine revalidation gates adaptive gather
The `context` default gather SHALL be `"adaptive"`, and a parity test SHALL guard that default by
reproducing its grade-2 recall improvement through the engine's own token accounting (capped
source-slice cost and the response's real packing), not only the offline Python harness. The parity
test SHALL drive the `context` op with `gather = "adaptive"` over the evaluation rows and assert the
in-engine recall and candidate-cost deltas against recorded targets. It SHALL measure against a
committed, version-controlled fixture pair (a deterministic code-only graph and a verbatim eval
snapshot), NOT the mutable working-tree artifacts (`cgraph-out/graph.json`,
`research/eval/queries.jsonl`), so the gate is reproducible and immune to daemon-state or
working-tree drift. Because the fixture is always present, the gate SHALL run on every checkout
including CI; the artifact-absent skip SHALL remain only as a defensive fallback for the case where
the fixture is missing. The recorded targets and tolerance SHALL be unchanged by the default flip.

#### Scenario: Parity gate reproduces the recall gain in-engine
- **WHEN** the parity test runs `gather = "adaptive"` against the committed fixture rows
- **THEN** mean grade-2 recall@budget is at least the fixed-2-hop baseline plus the change's target
  margin, measured under the engine's real cost model, and the test fails if it is not

#### Scenario: Parity gate runs against the committed fixture, not the working tree
- **WHEN** the parity test runs on any checkout, regardless of the contents of
  `cgraph-out/graph.json` or whether a daemon has accumulated unrelated nodes
- **THEN** it reads the committed fixture pair, reaches the measurement (does not skip), and its
  result depends only on the fixture and the engine, not on working-tree state

#### Scenario: Skip is a fallback only when the fixture is missing
- **WHEN** the committed fixture pair is absent (e.g. a deliberately stripped tree)
- **THEN** the test skips with a success exit rather than failing, exactly as the prior
  artifact-absent behavior

### Requirement: Context responses self-describe their gather and packing mode
Every `context` op response SHALL include a `gather` field (`"fixed"` or `"adaptive"`) and a
`packing` field (`"greedy"` or `"knapsack"`) in every code path, so a caller can determine which
retrieval and packing strategy produced the bundle without inferring it from other fields. The fields
SHALL reflect the strategy actually used after defaults and the `adaptive`-implies-`knapsack` coupling
are applied.

#### Scenario: Default response names its mode
- **WHEN** a `context` request runs with the defaults (adaptive gather)
- **THEN** the response includes `gather: "adaptive"` and `packing: "knapsack"`

#### Scenario: Explicit fixed/greedy response names its mode
- **WHEN** a `context` request sets `gather = "fixed"` with default packing
- **THEN** the response includes `gather: "fixed"` and `packing: "greedy"`

### Requirement: Adaptive responses report gather reach
When `gather = "adaptive"`, the `context` response SHALL include a `reach` summary reporting the total
candidate count gathered, the number of candidates admitted beyond the 2-hop core by the relevance
gate, and the number of depth-2 frontier nodes the gate rejected, so a caller and the telemetry can
observe whether the gate actually expanded the third hop. When `gather = "fixed"` the response SHALL
NOT include the `reach` summary.

#### Scenario: Gate admits a relevant third hop
- **WHEN** adaptive gather runs with a query whose terms match a depth-2 node, and that node has
  depth-3 neighbors
- **THEN** the `reach` summary reports at least one candidate admitted beyond the 2-hop core

#### Scenario: Gate collapses to the core when nothing is relevant
- **WHEN** adaptive gather runs with a query that matches no depth-2 frontier node above the threshold
- **THEN** the `reach` summary reports zero candidates admitted beyond the 2-hop core

### Requirement: Context op contributes a zero-result signal to op-stats
The op-stats recording for the `context` op SHALL set the zero-hit flag when the focal node does not
resolve — the id or query matched nothing — mirroring the query op's zero-hit semantics, so the
durable ledger distinguishes context calls that found a symbol from those that found nothing. A
`context` call that returns a resolved focus SHALL NOT be recorded as a zero hit, even when a tight
budget left no room for neighbors (the focal snippet is still usable context). The context zero-hit
count SHALL be persisted on the ledger line and summed in the cross-session rollup so the rate is
durable, not only live.

#### Scenario: Unresolved focus is a zero hit
- **WHEN** a `context` request resolves no focal node
- **THEN** the op-stats record for that call has the zero-hit flag set, and the persisted context
  zero-hit count for that lifetime increments

#### Scenario: A resolved focus is not a zero hit
- **WHEN** a `context` request returns a resolved focus (with or without included neighbors)
- **THEN** the op-stats record for that call does not have the zero-hit flag set

### Requirement: Op-stats distinguish adaptive gather usage
The durable op-stats ledger SHALL track `context` calls served with `gather = "adaptive"` separately
from fixed-gather calls, as an additive count that is persisted and summed across sessions, so a
rollup can report adaptive adoption. The adaptive count SHALL be readable from ledger files written
before this field existed by defaulting it to zero, so older ledgers continue to roll up without
migration.

#### Scenario: Adaptive context calls are counted distinctly
- **WHEN** N `context` calls run with `gather = "adaptive"` and M run with `gather = "fixed"`, then
  the stats are persisted and reloaded
- **THEN** the cross-session rollup reports the adaptive `context` count as N, independent of the
  total `context` count

#### Scenario: Older ledgers without the field still roll up
- **WHEN** a persisted ledger line predates the adaptive-usage field
- **THEN** the rollup reads its adaptive count as zero and sums it without error

### Requirement: graph_context advertises adaptive gather and forwards its parameters
The `graph_context` MCP tool description SHALL document the adaptive gather mode, when to use it (a
query is present and relevant reach beyond two hops is wanted at bounded token cost), and the
requirement that a `query`/`q` be supplied for the gate to take effect. The `gather` and
`gather_theta` arguments SHALL forward verbatim to the daemon `context` op.

#### Scenario: Tool description names adaptive
- **WHEN** an MCP client lists tools
- **THEN** the `graph_context` description mentions the adaptive gather mode

#### Scenario: Adaptive parameters forward to the context op
- **WHEN** an MCP client calls `graph_context` with `gather = "adaptive"` and a `gather_theta`
- **THEN** the daemon request targets the `context` op with `gather` and `gather_theta` carried
  through unchanged

### Requirement: Explain supports optional relation filtering
The `explain` op SHALL accept an optional `relation` parameter. When `relation` is a
non-empty string, the op SHALL return only adjacent edges whose stored relation token
equals it, using the same exact, case-sensitive comparison as `impact`'s relation filter;
no case-folding or alias mapping SHALL be applied. The relation filter SHALL be applied
before centrality ordering and before the `limit` truncation, so that edges matching the
requested relation are never displaced by higher-centrality edges of other relations. The
relation filter SHALL compose with the existing `direction` filter: when both are given,
only edges satisfying both SHALL be returned. When `relation` is absent or empty, the op's
returned neighbor set, ordering, and counts SHALL be byte-for-byte identical to current
behavior. A `relation` value that matches no adjacent edge SHALL yield an empty neighbor
list and SHALL NOT be treated as an error or a missing-node case.

#### Scenario: Relation filter returns only matching edges
- **WHEN** `explain` is called on a node with mixed adjacent relations and `relation: CALLS`
- **THEN** every returned neighbor carries `relation == "CALLS"`, and an adjacent non-`CALLS`
  neighbor that appears in the unfiltered result is absent

#### Scenario: Absent relation preserves current behavior
- **WHEN** `explain` is called without a `relation` parameter
- **THEN** the returned neighbor set, centrality ordering, and counts are identical to the
  behavior before this change

#### Scenario: Relation and direction compose
- **WHEN** `explain` is called with `direction: in` and `relation: CALLS`
- **THEN** only incoming `CALLS` edges are returned (the intersection of both filters)

#### Scenario: No-match relation yields an empty neighbor list
- **WHEN** `explain` is called with a `relation` that no adjacent edge carries
- **THEN** the response is a found node with an empty neighbor list, not an error or a
  not-found result

### Requirement: graph_explain advertises typed traversal and forwards the relation parameter
The `graph_explain` MCP tool description SHALL document the `relation` parameter and the
named single-hop traversal patterns an agent uses, mapped to the project's stored relation
tokens: find callers (`direction: in`, `relation: CALLS`), find callees (`direction: out`,
`relation: CALLS`), find references (`relation: references`), trace imports
(`relation: imports`), and inspect inheritance/implementation edges (`relation: inherits`).
The tool SHALL forward the `relation` argument verbatim to the `explain` op so the param is
never silently dropped.

#### Scenario: Tool description names the typed patterns
- **WHEN** an MCP client lists tools
- **THEN** the `graph_explain` description names the `relation` parameter and the
  callers/callees/references usage patterns

#### Scenario: relation forwards to the explain op
- **WHEN** an MCP client calls `graph_explain` with `relation: CALLS`
- **THEN** the daemon receives `op == "explain"` with `params.relation == "CALLS"`

### Requirement: Parity gate uses a committed deterministic fixture graph
The pack_context parity gate's graph fixture SHALL be a deterministic, code-only build of the
project (excluding disposable research and generated build artifacts), committed under version
control alongside a verbatim snapshot of the evaluation set. The fixture graph SHALL be reproducible:
rebuilding it from the same sources SHALL produce a byte-identical `graph.json`. The fixture graph
and the fixture eval set SHALL be internally consistent — every grade-2 evaluation `node_id` SHALL
resolve to a node in the fixture graph — so the gate is independent of checkout path and machine. The
fixture eval set SHALL be a verbatim copy: labels, grades, queries, and recorded targets SHALL NOT be
altered when producing or regenerating the fixture.

#### Scenario: Fixture graph is deterministic
- **WHEN** the fixture graph is rebuilt from the same sources
- **THEN** the resulting `graph.json` is byte-identical to the committed fixture

#### Scenario: Fixture graph and eval set are internally consistent
- **WHEN** the parity test resolves each grade-2 evaluation `node_id` against the fixture graph
- **THEN** every id resolves to a node present in the fixture graph

#### Scenario: Fixture excludes disposable and generated content
- **WHEN** the fixture graph is generated
- **THEN** it contains only the project's code (e.g. src/tests/scripts) and no `research/` or
  `build/` nodes, matching the graph the recorded targets were calibrated on

### Requirement: Lexical multi-seed focal resolution for free-text queries
When resolving a focal node for a `context` or `query` request, the engine SHALL first attempt
exact (id, label, bare symbol) and substring matching, and SHALL fall back to lexical term-overlap
matching only when those produce no match. The lexical fallback SHALL rank nodes by the overlap of
the query's lexical terms with the node label and resolve the focal from the top match,
deterministically (ties broken by centrality then id). For a `context` request, the gather SHALL be
seeded from the top-N lexical matches and union their neighborhoods. When the best lexical overlap
falls below a minimum confidence threshold, the focal SHALL remain unresolved — the response returns
suggestions and the call is recorded as a zero hit, unchanged from current behavior.

#### Scenario: Natural-language query resolves via lexical overlap
- **WHEN** a `context` request supplies a free-text query that is not an exact match or a substring
  of any node id or label, but shares lexical terms with one or more symbols
- **THEN** a focal node is resolved from the highest-overlap match and a non-empty context bundle is
  returned, instead of the empty `focus:null` response

#### Scenario: Exact lookups are unchanged
- **WHEN** a request supplies an exact node id, an exact label, or a bare symbol name that resolves
  by the existing exact/substring path
- **THEN** that node is resolved exactly as before and the lexical fallback does not run

#### Scenario: Off-topic query stays an honest zero hit
- **WHEN** a query's best lexical overlap with any node is below the confidence threshold
- **THEN** the focal stays unresolved, the response returns `suggestions`, and the call is recorded
  as a context/query zero hit

#### Scenario: Multi-seed gather unions several ego graphs
- **WHEN** a free-text `context` query overlaps several symbols and resolves via the lexical fallback
- **THEN** the gathered candidate set is the union of the neighborhoods of the top-N lexical seeds,
  deduplicated by shallowest reach, and a relevant node reachable only from a lower-ranked seed is
  included

#### Scenario: Resolution is deterministic
- **WHEN** the same free-text query is resolved twice against the same snapshot
- **THEN** the same focal node (and seed set) is selected, with equal-overlap ties broken by
  centrality then id

### Requirement: Query op routes by deterministic intent
The `query` op SHALL classify each query into one of three routes using only deterministic signals —
an exact match against the snapshot's symbol table, a fixed case-insensitive grammar of
structural-intent phrases, and the lexical fallback — with no model or inference. Classification
SHALL be evaluated in priority order: (1) a structural-intent phrase whose operand resolves to a
symbol, (2) an exact-entity match, (3) lexical search. An unrecognized or unresolved query SHALL
fall through to lexical search, so the op's behavior is never worse than the prior name search.

The structural-phrase grammar SHALL recognize, at minimum: `callers of X` / `who calls X` /
`what calls X` (relation `CALLS`, incoming); `callees of X` / `what does X call` (relation `CALLS`,
outgoing); `references to X` / `uses of X` (relation `references`, incoming); `implementations of X`
/ `who implements X` / `subclasses of X` (relation `inherits`, incoming); `importers of X` /
`who imports X` (relation `imports`, incoming). The operand `X` SHALL be resolved with the same exact
matcher the entity route uses; if it does not resolve, the query falls through to lexical search.
Relation tokens SHALL reuse the edge-type strings already stored on edges and accepted by the
`explain` relation filter — no new vocabulary is introduced.

#### Scenario: Structural-intent phrase returns typed neighbors
- **WHEN** the query is `who calls buildApp` and `buildApp` resolves to a symbol
- **THEN** the response is routed to incoming `CALLS` traversal and returns the caller symbols
  (the same set `explain` would yield filtered to that relation and direction), not a flat name
  search over the words `who`, `calls`, `buildApp`

#### Scenario: Unique exact symbol returns the entity with a typed-neighbor summary
- **WHEN** the query is whitespace-free and pins down exactly one symbol — `matching_nodes` returns a
  single node that the query equals by id or case-insensitive label / bare symbol name
- **THEN** the response returns that node together with a compact typed-neighbor summary (callers,
  callees, references — counts and a capped set of top ids), answering "the symbol and who uses it"
  in a single call

#### Scenario: A name that also matches other symbols stays a search
- **WHEN** the query exactly matches one symbol's name but is also a substring of other symbols
  (e.g. `alpha` matches both `Alpha` and `AlphaLeaf`)
- **THEN** the query is NOT routed to the single-symbol entity result; it stays in lexical search and
  returns all matches, so routing never narrows a result the prior search would have returned

#### Scenario: Natural-language query keeps lexical search
- **WHEN** the query is a multi-word natural-language phrase that is neither an exact symbol nor a
  recognized structural phrase (e.g. `review and land a finished run`)
- **THEN** the response is the existing importance / lexical-overlap ranked symbol list, unchanged

#### Scenario: Structural phrase with an unresolvable operand falls through
- **WHEN** the query matches a structural phrase shape but its operand does not resolve to any symbol
  (e.g. `references to the old config format`)
- **THEN** the query is not forced into a typed traversal; it falls through to lexical search and
  returns ranked matches (or did-you-mean suggestions when none), exactly as today

### Requirement: Query responses self-describe their route
Every `query` response SHALL carry a `route` field naming the path taken (`entity`, the structural
intent name such as `callers`/`callees`/`references`/`implementations`/`importers`, or `search`), so
an agent can tell a precise structural answer from a fuzzy search and the op-stats ledger can record
the route distribution. The structural route SHALL also identify the resolved operand it traversed
from.

#### Scenario: Route is reported on every path
- **WHEN** any `query` call returns
- **THEN** the response includes a `route` field identifying which of entity / structural-intent /
  search produced the result, and a structural route additionally reports the operand symbol it
  traversed from

### Requirement: graph_query advertises intent routing
The `graph_query` MCP tool description SHALL state that it answers structural questions directly —
callers, callees, references, implementations, and importers of a symbol — in addition to name
search, and that the response reports the `route` taken, so a host agent issues the structural
question to `graph_query` instead of manually orchestrating `graph_query` then `graph_explain`.

#### Scenario: Tool description guides direct structural use
- **WHEN** a host inspects the `graph_query` tool schema
- **THEN** the description advertises direct structural answers (callers/callees/references/
  implementations/importers) and the `route` field, not only case-insensitive name search

### Requirement: Not-ready reads are excluded from op-stats quality accounting
Reads served while the daemon is still building (the empty build snapshot, `build_state` Empty) SHALL
NOT be counted as operations or zero-hits in op-stats. Such a read SHALL instead increment a separate
`not_ready` counter and SHALL NOT affect per-op counts, latency, the zero-hit counters, or the recent
window. As a result, `query_zero_hit_rate` (and the other zero-hit/latency figures) reflect only
reads against a ready graph. The `not_ready` count SHALL be surfaced in `status` and the durable
ledger so build-window traffic remains visible without polluting the quality metrics.

#### Scenario: A query during build is not a miss
- **WHEN** a `query` is served while the graph is still building (empty snapshot, returning no
  results because nothing is loaded)
- **THEN** `query_zero_hits` and `query` count are unchanged, the `not_ready` counter increments by
  one, and `query_zero_hit_rate` is unaffected

#### Scenario: Ready-state accounting is unchanged
- **WHEN** a `query` is served against a ready graph
- **THEN** it is counted normally (op count, latency, and zero-hit if it returned nothing), exactly
  as before

### Requirement: Op-stats record the query route distribution
Op-stats SHALL record how query-op responses were routed, in three buckets — `entity` (a unique exact
symbol), `structural` (a typed-traversal intent such as callers/callees/references), and `search`
(lexical/name search or unrouted) — so routing adoption is observable. These counts SHALL be surfaced
in `status` and persisted in the durable ledger and its cross-session rollup, alongside the existing
adaptive-context counter.

#### Scenario: Route counts reflect how queries resolved
- **WHEN** queries are served and routed (some to entity, some to structural traversal, some to
  lexical search)
- **THEN** `status` and the ledger report the per-bucket counts, so the fraction of queries answered
  by typed routing vs search is visible

### Requirement: The stats command reports all-time by default
`cgraph stats` SHALL default to the full durable history (no lower time bound); `--since` SHALL still
narrow the window. Checking stats without arguments SHALL surface all recorded lifetimes rather than
only the current day's.

#### Scenario: Default stats shows full history
- **WHEN** `cgraph stats --root PATH` is run with no `--since`
- **THEN** it rolls up every recorded lifetime in the ledger, not only those since the start of the
  current day

### Requirement: Per-project daemon
The system SHALL run at most one resident daemon per canonical project root, keyed by the absolute project path and isolated from other project roots.

#### Scenario: Client auto-spawns daemon
- **WHEN** a thin client command runs and no daemon is listening for the project root
- **THEN** the client starts the daemon, waits with bounded backoff, connects, and sends the request

#### Scenario: Concurrent spawn race resolves
- **WHEN** two clients attempt to start a daemon for the same project root concurrently
- **THEN** exactly one daemon binds the project socket or pipe and both clients communicate with that daemon

### Requirement: Cross-platform local IPC
The daemon SHALL support length-prefixed JSON request and response frames over Unix sockets on Linux and macOS. Windows named-pipe transport is deferred: the daemon server, endpoint security descriptor, and client auto-spawn are stubbed on Windows and fail with an explicit not-implemented error rather than a silent degradation.

#### Scenario: Command frame succeeds
- **WHEN** a client sends a valid `query`, `path`, `explain`, `update`, `status`, or `shutdown` frame
- **THEN** the daemon processes the operation and returns a valid response frame

#### Scenario: Protocol version mismatch recovers
- **WHEN** a client connects with an incompatible protocol version
- **THEN** the daemon is shut down or rejected according to the version-skew policy and the client can respawn a compatible daemon

### Requirement: Secure daemon endpoint
The daemon SHALL restrict socket or pipe access to the current user and reject cross-user access where platform support exists.

#### Scenario: Unauthorized peer is rejected
- **WHEN** a peer from another user attempts to connect on a platform with peer credential support
- **THEN** the daemon rejects the connection before processing any graph command

### Requirement: Thin client command surface
The system SHALL provide thin client commands for `query`, `path`, `explain`, `update`, `status`, and `shutdown` that do not rebuild the graph for each request in daemon mode.

#### Scenario: Query uses resident graph
- **WHEN** a user runs a thin client `query` command while the daemon has a loaded graph
- **THEN** the client sends one request and prints the daemon response without running the full pipeline locally

### Requirement: Immutable snapshot concurrency
The daemon SHALL serve reads from immutable graph snapshots and apply graph mutations through a
single writer before publishing a new complete snapshot. A graph the daemon rebuilds from source
(full rescan or incremental update) SHALL be identical to the graph the canonical one-shot pipeline
produces for the same files: the same deduplication result and the same node and edge counts, with
community and centrality computed on the deduplicated node set.

#### Scenario: Read during update is consistent
- **WHEN** a read request overlaps a watcher-driven update or semantic fragment merge
- **THEN** the read observes either the previous complete snapshot or the next complete snapshot,
  never a partially mutated graph

#### Scenario: Rebuilt graph matches the canonical pipeline
- **WHEN** the daemon rebuilds the graph for a project
- **THEN** its node and edge counts and its deduplication result match a one-shot build of the same
  project, and every node carries the centrality computed after deduplication

### Requirement: Daemon lifecycle and fallback
The daemon SHALL support idle shutdown, clean socket or pipe cleanup, a tiered version-stamped
startup that reuses a persisted extraction index, and one-shot CLI fallback for environments that
cannot run resident processes. Startup SHALL escalate by cost: serve directly from the persisted
graph when no source file has changed; re-extract only changed, added, or removed files when some
have; and perform a full rebuild only when no usable cache exists. A tiered startup SHALL produce
a graph byte-identical to a full cold rebuild for the same source tree and version key. A full
rescan SHALL re-extract only the files that changed since the in-memory index was built, reusing
the held extraction for unchanged files, and SHALL produce the same graph as re-extracting every
file.

#### Scenario: Idle daemon exits
- **WHEN** the daemon has no activity for the configured idle timeout
- **THEN** it flushes authoritative outputs as needed, releases the endpoint, and exits

#### Scenario: Restricted environment uses one-shot
- **WHEN** the target environment disallows background resident processes
- **THEN** the same engine can run in one-shot mode without daemon IPC

#### Scenario: Unchanged tree serves from disk without re-extracting
- **WHEN** the daemon starts, a valid version-matched cache exists, and a stat/hash diff finds no
  changed, added, or removed source files
- **THEN** the daemon loads the persisted graph and serves queries without calling the extractor

#### Scenario: Changed subset re-extracts only what changed
- **WHEN** the daemon starts with a valid cache and a stat/hash diff finds a subset of files
  changed, added, or removed
- **THEN** the daemon re-extracts only those files, reuses cached results for the rest, rebuilds
  the merged graph, and the result equals a full cold rebuild on the same tree

#### Scenario: Full rescan re-extracts only changed files
- **WHEN** a full rescan runs while the in-memory index already holds extractions and only a subset
  of files changed
- **THEN** only the changed and new files are re-extracted, unchanged files reuse their held
  extraction, and the resulting graph equals a rescan that re-extracted every file

#### Scenario: Branch switch does not force a full re-extract
- **WHEN** a checkout rewrites file modification times without changing file contents
- **THEN** the stat miss falls back to a content-hash comparison and unchanged files are reused
  rather than re-extracted

### Requirement: Daemon status
The daemon SHALL expose status including process id, uptime, node count, edge count, build state,
cache hit rate, and resident memory where available, with no field carrying a permanent
placeholder value. Specifically `uptime_seconds` SHALL reflect real elapsed time since daemon
start, `cache_hit_rate` SHALL reflect the measured fraction of files reused from cache on the most
recent (re)build, and `enrichment_running` SHALL reflect the count of in-flight enrichment ingests,
with `enrichment_state` reporting `running` while an ingest is active. The status payload SHALL
continue to report `pid`, `node_count`, `edge_count`, `build_state`, the enrichment pending/stale/
failed counts, `watching`, and `incremental_updates`. Enrichment status (pending, stale, running,
failed) SHALL be refreshed asynchronously: a build, update, or fragment ingestion SHALL NOT block
its response on the whole-project enrichment scan, and the enrichment counts SHALL converge after
the asynchronous re-plan completes.

#### Scenario: Uptime advances with daemon lifetime
- **WHEN** `status` is queried after the daemon has been running for a measurable interval
- **THEN** `uptime_seconds` is greater than zero and increases on a later query

#### Scenario: Cache hit rate reflects real reuse
- **WHEN** a warm rescan reuses a subset of files and re-extracts the rest
- **THEN** `cache_hit_rate` reported by `status` equals reused files / total files and lies in
  the interval (0, 1]

#### Scenario: Enrichment running is observable
- **WHEN** an enrichment ingest is in flight
- **THEN** `status` reports `enrichment_running` >= 1 and `enrichment_state` equal to `running`,
  and both clear once the ingest completes

#### Scenario: Status reports enrichment state
- **WHEN** a client requests status
- **THEN** the response includes the current enrichment state and pending/stale/failed counts

#### Scenario: Update does not block on enrichment planning
- **WHEN** an `update` op rebuilds the graph on a project with many enrichable documents
- **THEN** the op responds once the graph is rebuilt and persisted, without waiting for the
  enrichment scan, and the enrichment counts are refreshed shortly afterward

### Requirement: Persisted extraction index
The daemon SHALL persist the per-file extraction index (each file's extraction fragment, raw
calls, raw relations, file cache entry, and resolved path aliases) to disk under the project
output directory, written atomically, so that a subsequent start can rebuild the graph without
re-extracting unchanged files.

#### Scenario: Index round-trips without loss
- **WHEN** the daemon persists its extraction index and a later start loads it
- **THEN** rebuilding the graph from the loaded index produces a graph equal to rebuilding from
  the in-memory index that was persisted

#### Scenario: Corrupt cache is ignored, never half-loaded
- **WHEN** the persisted index file is truncated, partially written, or otherwise unparseable
- **THEN** the daemon treats it as no usable cache, falls back to a full rebuild, and serves a
  valid graph without crashing

### Requirement: Version-stamped cache invalidation
The persisted extraction index SHALL carry a content-addressed version key derived from extractor
identity, language configuration, and ID-normalization rules. The daemon SHALL discard the entire
cache and perform a full rebuild when the loaded key does not match the running binary's key,
even when every source file is byte-identical on disk.

#### Scenario: Extractor change invalidates a byte-identical tree
- **WHEN** the source tree is unchanged but the running binary's version key differs from the
  persisted cache's key
- **THEN** the daemon discards the cache and rebuilds the graph from a full extraction rather than
  serving a graph produced by the previous extractor

#### Scenario: Matching key permits reuse
- **WHEN** the persisted version key matches the running binary's key
- **THEN** the daemon is permitted to reuse cached per-file extraction results for unchanged files

### Requirement: Daemon operation stats in status
The daemon SHALL accumulate per-op counts and total latency for each request type, measured at the
request-dispatch boundary, and expose them through the `status` op as since-boot lifetime totals
together with a rolling recent window (last N operations). For queries it SHALL additionally report
a zero-hit rate (queries returning no results / total queries). The `status` payload SHALL also
include a modeled cache-saving estimate derived from the most recent (re)build's measured per-file
timings, labeled as an estimate and omitted when no per-file mean is available.

#### Scenario: Status reports per-op counts and latency
- **WHEN** a client has issued several `query` ops against the daemon
- **THEN** `status` reports a `query` count equal to the number issued and a non-zero total/mean
  latency for that op

#### Scenario: Zero-hit queries are tracked
- **WHEN** some issued queries return zero results and others return matches
- **THEN** `status` reports a query zero-hit rate equal to (zero-result queries / total queries)

#### Scenario: Lifetime totals and recent window coexist
- **WHEN** more operations have been issued than the rolling window capacity
- **THEN** `status` reports lifetime totals covering all operations and a recent window reflecting
  only the most recent N, and the recent window never exceeds its capacity

### Requirement: Status reports semantic connectivity
The `status` op SHALL report a `semantic` block describing how well the host-authored semantic
layer connects to the code graph, computed from the current snapshot so it reflects live
enrichment. The block SHALL include the number of document nodes, the number of concept nodes, how
many document nodes reach a code node (the connected set), the orphan document count, the orphan
concept count, the number of edges bridging semantic nodes directly into code, and a connectivity
rate (connected documents / document nodes). A node SHALL be treated as semantic when its id is
namespaced `doc:`, `concept:`, or `topic:`, and as a code node otherwise.

#### Scenario: Status carries the semantic block
- **WHEN** the snapshot contains a document node with an edge into a code node
- **THEN** `status.semantic` reports `doc_nodes` >= 1 and a `connectivity_rate` greater than 0

#### Scenario: Empty semantic layer reports zeros without dividing by zero
- **WHEN** the snapshot has no semantic nodes (a pure code graph)
- **THEN** `status.semantic` reports zero doc and concept nodes and a connectivity rate of 0

### Requirement: Document-to-code connectivity is transitive
A document node SHALL count as connected when it reaches a code node within a bounded number of
hops over the graph edges, so a document linked to code through a concept
(`doc -> concept -> code`) counts as connected, not orphan. A document that reaches no code node
within the bound SHALL count as an orphan document.

#### Scenario: Document connected through a concept counts as connected
- **WHEN** a document links to a concept that links to a code node, and the hop bound is at least 2
- **THEN** the document is counted in the connected set, not as an orphan

#### Scenario: Direct and transitive bounds differ
- **WHEN** the only path from a document to code is `doc -> concept -> code`
- **THEN** the document is connected at a hop bound of 2 and not connected at a hop bound of 1

#### Scenario: Document reaching no code is an orphan
- **WHEN** a document links only to a concept that itself has no edge to any code node
- **THEN** the document is counted as an orphan document and the concept as an orphan concept

### Requirement: End-to-end retrieval quality gate
The system SHALL gate end-to-end retrieval quality with a committed smoke test that, for each
symbol-granularity row of the committed eval fixture, sends the row's free-text query to the
`context` op via `q` only (no focal id injection, engine defaults for gather, packing, and
depth), computes mean grade-2 recall at fixed token budgets, and fails when recall drops below
a committed measured baseline minus a fixed tolerance. Rows whose query fails to resolve a
focal SHALL count as zero recall rather than being skipped. The gate SHALL run as part of the
default CTest suite and SHALL NOT read mutable build output (it reads only the committed
fixture).

#### Scenario: Regression on the resolution path fails the suite
- **WHEN** a change causes free-text queries to stop resolving to their focal symbols (for
  example, the entity/lexical resolution path is broken) and the smoke suite runs
- **THEN** the end-to-end gate's measured recall drops below the committed baseline minus
  tolerance and the test fails

#### Scenario: Default-path behavior is what the gate measures
- **WHEN** the gate issues its `context` requests
- **THEN** the requests carry only the query text and budget, so the engine's actual default
  gather and packing configuration is what is measured

#### Scenario: Unresolved rows are counted, not skipped
- **WHEN** a fixture row's query resolves no focal node
- **THEN** the row contributes zero recall to the mean instead of being excluded

### Requirement: Never-idle daemon mode
A daemon started with an idle timeout of zero or less SHALL NOT idle-shut-down: it SHALL remain
resident and continue watching the project tree until it receives an explicit `shutdown` op or a
termination signal. A daemon started with a positive idle timeout SHALL retain the existing
behavior — it shuts down after that much time with no request and no code edit. The configured
idle timeout SHALL be settable via the daemon's `--idle-timeout SECONDS` argument.

#### Scenario: Zero idle timeout keeps the daemon resident
- **WHEN** a daemon is started with `--idle-timeout 0` and then receives no requests and observes no
  code edits for longer than the default idle window
- **THEN** the daemon is still listening and answers a `status` op

#### Scenario: Positive idle timeout still shuts down
- **WHEN** a daemon is started with a positive `--idle-timeout` and then receives no requests and
  observes no code edits for longer than that timeout
- **THEN** the daemon leaves its serve loop and shuts down (and the existing op-stats ledger flush
  on shutdown still occurs)

#### Scenario: Explicit shutdown always works
- **WHEN** a never-idle daemon receives a `shutdown` op
- **THEN** it leaves its serve loop and terminates regardless of the idle-timeout setting

### Requirement: Single-owner endpoint bind
Daemon startup SHALL guarantee at most one live daemon per canonical project root. Before claiming
the endpoint, startup SHALL probe the existing socket: if a live daemon answers, the starting
instance SHALL NOT unlink or rebind the endpoint — it SHALL report that the root is already served
and exit without error. Only when the probe finds no live listener (a stale socket left by a crashed
daemon, or no socket at all) SHALL startup unlink any stale endpoint and bind its own.

#### Scenario: Second start defers to the live daemon
- **WHEN** a daemon is already resident and serving a root, and a second daemon is started for the
  same root
- **THEN** the second instance reports the root is already served and exits without error, the
  original daemon still owns the socket, and a `query` issued to that socket is answered by the
  original daemon

#### Scenario: Stale socket is reclaimed
- **WHEN** a daemon is started for a root whose socket file exists but no daemon is listening on it
  (a crashed predecessor)
- **THEN** startup unlinks the stale socket, binds its own, and serves normally

### Requirement: Adaptive gather admits bounded same-file focal context
When `gather = "adaptive"` and a free-text query is present, the `context` op SHALL consider code nodes that share a non-empty source file with the primary resolved focal, even when no persisted graph edge connects those nodes. Same-file candidates SHALL be admitted directly as candidates, SHALL NOT expand the graph frontier, SHALL be deduplicated against normally reached nodes, and SHALL be bounded to five admitted nodes from the focal source file. Ordering SHALL be deterministic by lexical overlap descending, centrality descending, then node id ascending. Admitted entries SHALL report depth 2 and `via = "same_file"`. During knapsack packing, a same-file candidate SHALL receive lexical-overlap value only and SHALL NOT receive the structural hop-value term used by persisted graph neighbors. The implementation SHALL NOT add nodes or edges to `graph.json`.

#### Scenario: Adaptive gather includes a relevant same-file sibling
- **WHEN** a free-text `context` request resolves a primary focal whose source file contains an otherwise-unreachable sibling
- **THEN** adaptive gathering admits that sibling as a depth-2 candidate with `via = "same_file"`

#### Scenario: Fixed gather remains unchanged
- **WHEN** the same request sets `gather = "fixed"`
- **THEN** no same-file candidate expansion occurs and the response remains byte-for-byte identical to historical fixed gathering

#### Scenario: Existing graph reach wins deduplication
- **WHEN** an eligible same-file sibling was already reached through persisted graph edges at an equal or shallower depth
- **THEN** the existing reach record and relation remain unchanged and the sibling appears at most once

#### Scenario: Empty and non-code sources are excluded
- **WHEN** a seed or sibling has an empty source file or the sibling is a session-memory node
- **THEN** that node does not participate in same-file expansion

#### Scenario: Dense files are deterministically capped
- **WHEN** more than five eligible siblings share a focal source file
- **THEN** exactly the first five siblings under overlap-descending, centrality-descending, id-ascending order are admitted

#### Scenario: Query overlap controls inferred-candidate value
- **WHEN** a focal-file sibling has no lexical overlap with the free-text query but remains within the five-candidate cap
- **THEN** the sibling remains in the gathered candidate set but receives zero knapsack value and cannot displace a positive-value structural neighbor

### Requirement: Current-path recall gates same-file expansion
Same-file expansion SHALL be retained only when the committed query-only retrieval fixture demonstrates a strict mean grade-2 recall increase at one or more fixed budgets and no decrease at the remaining budgets, without fixture regeneration or tolerance weakening. The serialized graph parity tests SHALL remain unchanged, and median warmed `context` latency over at least 50 calls SHALL NOT regress by more than 10% on the same graph and query.

#### Scenario: Measured recall gain permits the change
- **WHEN** the committed end-to-end retrieval gate runs before and after same-file expansion
- **THEN** at least one of the 2k, 4k, or 8k recall measurements increases strictly and none decreases before baselines are updated

#### Scenario: No gain rejects the mechanism
- **WHEN** no committed retrieval budget improves or any budget decreases
- **THEN** the same-file expansion implementation is removed rather than shipped or hidden behind a fallback

#### Scenario: Graph parity remains byte-identical
- **WHEN** extractor golden and graph parity tests run after the change
- **THEN** their serialized `graph.json` outputs remain byte-identical to the pre-change outputs

#### Scenario: Context latency blocks a regression
- **WHEN** at least 50 warmed resident-daemon `context` calls are measured before and after on the same graph and query
- **THEN** the change is blocked if after-change median latency exceeds the before-change median by more than 10%

#### Scenario: Transfer topology blocks a regression
- **WHEN** the unchanged and changed engines score the same frozen TypeScript graph and query-only evaluation rows
- **THEN** no measured budget decreases before the same-file mechanism is retained

### Requirement: Daemon status reports unextracted coverage
The `status` op payload SHALL include `unextracted`: a map of language name -> count of detected
files no registered extractor handles. Full rescans recompute the map from the detection walk;
the Tier-1 fast-load start computes it from the detection walk that path already performs;
incremental updates adjust it as unsupported-language files are added or removed (full rescans
self-heal any drift).

#### Scenario: Status surfaces the hole a warning used to hide
- **WHEN** a daemon serves a project containing `.cs` or `.blade.php` files
- **THEN** `status` reports them under `unextracted` (e.g. `{"csharp": 12, "php-blade": 3}`)
  instead of only a per-fragment warning inside the index

#### Scenario: Fast-load start still reports coverage
- **WHEN** a daemon starts via the Tier-1 persisted-graph path (no rescan)
- **THEN** `unextracted` reflects the current tree, computed from the manifest-check detection
  walk

### Requirement: Declared frame lengths are validated before allocation
The daemon SHALL validate the declared body length of every received frame against the shared
`kMaxFrameBodyBytes` cap before allocating a buffer, and SHALL reject an oversized frame as a
protocol error without reading its body. The send path SHALL enforce the same cap so both sides
share one ceiling.

#### Scenario: Hostile length header
- **WHEN** a client sends a 4-byte length header declaring a body larger than the cap (e.g.
  `0xFFFFFFFF`)
- **THEN** the daemon rejects the frame without allocating the declared size, drops the
  connection, and continues serving subsequent connections normally

#### Scenario: Oversized payload is never framed
- **WHEN** `encode_frame` is asked to frame a body larger than the shared cap
- **THEN** it returns an empty frame instead of emitting bytes the peer would reject

### Requirement: Connections carry I/O timeouts
The daemon SHALL apply receive and send timeouts to every accepted connection so a peer that
stalls mid-frame cannot block the single serve-loop thread indefinitely; a timed-out connection
is dropped and the serve loop continues.

#### Scenario: Client stalls mid-frame
- **WHEN** a client connects, sends a partial frame, and then sends nothing further
- **THEN** the daemon drops that connection after the timeout and still answers a subsequent
  status request

### Requirement: Status reads enrichment state race-free
The daemon SHALL synchronize every read and write of the enrichment counters
(`enrichment_state`/`pending`/`running`/`stale`/`failed`/`plans_run`) and the `unextracted` map
through one dedicated lock, so a `status` request concurrent with enrichment refresh, drop
ingest, or a rescan never observes a torn counter or iterates the map mid-mutation.

#### Scenario: Status polled during an update
- **WHEN** a `status` request arrives while an incremental update is rewriting the unextracted
  coverage map
- **THEN** the response contains a consistent snapshot of the counters and map

### Requirement: Persistence never destroys the last-known-good graph
The daemon SHALL persist the graph snapshot by writing a temp file and atomically renaming it
over `graph.json`; on rename failure it SHALL leave the existing `graph.json` untouched, remove
the temp file, and surface the failure. It SHALL NOT delete the existing file to retry.

#### Scenario: Rename fails
- **WHEN** the atomic rename of the temp snapshot fails
- **THEN** the prior `graph.json` still exists with its previous content and the temp file is
  cleaned up

### Requirement: Content-verified graph synchronization
The daemon `update` operation SHALL act as a blocking content-verification barrier for detected code inputs. A successful response SHALL identify the immutable graph snapshot produced or confirmed by that barrier with its content root, algorithm, leaf count, and verification work counts.

#### Scenario: Synchronization catches preserved metadata
- **WHEN** a source file changes to different equal-length bytes while retaining its prior modification time and the client invokes `update`
- **THEN** the daemon re-extracts the changed file and returns a new content root

#### Scenario: Clean synchronization preserves identity
- **WHEN** the client invokes `update` twice without source changes
- **THEN** both successful responses return the same content root and the second update reuses every extraction

#### Scenario: Update completes before returning
- **WHEN** an update response returns successfully
- **THEN** subsequent reads pinned to its content root are served from the synchronized immutable snapshot

### Requirement: Content-root-pinned graph reads
Query, path, explain, impact, and context operations SHALL accept an optional `expected_content_root`. Each successful graph read SHALL report the content root of the immutable snapshot it used.

#### Scenario: Expected root matches
- **WHEN** a graph read supplies the root returned by the latest completed update and that snapshot remains current in the daemon
- **THEN** the daemon serves the read and echoes that root in freshness metadata

#### Scenario: Expected root does not match
- **WHEN** a graph read supplies a root different from the immutable snapshot selected for the request
- **THEN** the daemon returns an error and no graph result

#### Scenario: Unpinned read remains compatible
- **WHEN** an existing client omits `expected_content_root`
- **THEN** the daemon serves the read and includes the selected snapshot root without claiming that the mutable worktree was verified at request time

### Requirement: Content-root-gated persisted fast-load
The daemon SHALL persist the source content root in the versioned index manifest and SHALL fast-load a persisted graph only after hashing the complete detected code tree and matching its file set, individual hashes, root, leaf count, and index logic version.

#### Scenario: Unchanged tree fast-loads
- **WHEN** the persisted manifest version and root match a newly content-verified source tree
- **THEN** the daemon loads the persisted graph without re-extracting source files and assigns the verified root to the live snapshot

#### Scenario: Preserved-mtime edit rejects fast-load
- **WHEN** source bytes differ from the persisted leaf while size and modification time remain equal
- **THEN** startup rejects the persisted graph and rebuilds from source

#### Scenario: Old or incomplete manifest rebuilds
- **WHEN** the manifest lacks the current logic version or a valid content root
- **THEN** startup treats it as unusable and performs a verified rebuild

### Requirement: Watcher updates remain explicitly eventual
The live watcher SHALL use filesystem metadata only to identify candidate changes, SHALL content-hash every delivered code candidate before cache reuse, and SHALL never represent watcher-only freshness as a full-tree verification.

#### Scenario: Restored-mtime local rewrite is observed
- **WHEN** a normal POSIX file rewrite changes device, inode, or change time while restoring size and modification time
- **THEN** the watcher delivers a modified event and the updater validates content before reuse

#### Scenario: Status distinguishes identity from current-worktree proof
- **WHEN** status or an unpinned read reports a snapshot content root
- **THEN** the response describes the graph's source identity without asserting that no unseen filesystem edit occurred after the last barrier

