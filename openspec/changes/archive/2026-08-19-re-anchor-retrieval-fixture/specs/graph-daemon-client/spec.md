## MODIFIED Requirements

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
taken only along query-relevant paths. Gather and fill are independent: adaptive gather SHALL use
the greedy rank-order fill unless `packing = "knapsack"` is requested explicitly, and SHALL use the
existing deterministic relevance signal; it SHALL introduce no model or LLM.

#### Scenario: Default gather is adaptive with the greedy fill
- **WHEN** a `context` request omits `gather` and `packing`
- **THEN** the gather is `"adaptive"` (depth 3, θ=0.05 gate), the fill is greedy, and the response
  reports `gather: "adaptive"` and `packing: "greedy"`

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
- **THEN** its candidate set reaches symbols a 2-hop gather cannot, at a candidate count strictly
  below the full 3-hop fan-out

### Requirement: In-engine revalidation gates adaptive gather
The `context` default gather SHALL be `"adaptive"`, and a parity test SHALL guard that default
through the engine's own token accounting (capped source-slice cost and the response's real
packing), not only the offline Python harness. The parity test SHALL drive the `context` op with
`gather = "adaptive"` over the evaluation rows and assert that the default path's mean grade-2
recall is not inferior to the fixed 2-hop greedy baseline (within the recorded tolerance) while its
candidate pool stays strictly smaller than the full 3-hop fan-out. It SHALL measure against a
committed, version-controlled fixture pair (a deterministic code-only graph and a verbatim eval
snapshot), NOT the mutable working-tree artifacts (`cgraph-out/graph.json`,
`research/eval/queries.jsonl`), so the gate is reproducible and immune to daemon-state or
working-tree drift. Because the fixture is always present, the gate SHALL run on every checkout
including CI; the artifact-absent skip SHALL remain only as a defensive fallback for the case where
the fixture is missing. Whenever the fixture is regenerated, the recorded targets SHALL be
re-measured and re-pinned on the new pair in the same change.

#### Scenario: Parity gate holds the default to the fixed baseline in-engine
- **WHEN** the parity test runs `gather = "adaptive"` against the committed fixture rows
- **THEN** mean grade-2 recall@budget is at least the fixed-2-hop greedy baseline minus the
  recorded tolerance, the adaptive candidate pool is strictly smaller than fixed 3-hop's, and the
  test fails if either does not hold

#### Scenario: Parity gate runs against the committed fixture, not the working tree
- **WHEN** the parity test runs on any checkout, regardless of the contents of
  `cgraph-out/graph.json` or whether a daemon has accumulated unrelated nodes
- **THEN** it reads the committed fixture pair, reaches the measurement (does not skip), and its
  result depends only on the fixture and the engine, not on working-tree state

#### Scenario: Skip is a fallback only when the fixture is missing
- **WHEN** the committed fixture pair is absent (e.g. a deliberately stripped tree)
- **THEN** the test skips with a success exit rather than failing, exactly as the prior
  artifact-absent behavior

### Requirement: Adaptive gather admits bounded same-file focal context
When `gather = "adaptive"` and a free-text query is present, the `context` op SHALL consider code nodes that share a non-empty source file with the primary resolved focal, even when no persisted graph edge connects those nodes. Same-file candidates SHALL be admitted directly as candidates, SHALL NOT expand the graph frontier, SHALL be deduplicated against normally reached nodes, and SHALL be bounded to five admitted nodes from the focal source file. Ordering SHALL be deterministic by lexical overlap descending, centrality descending, then node id ascending. Admitted entries SHALL report depth 2 and `via = "same_file"`. Same-file admission is an inferred relationship: a same-file candidate SHALL be packed only on lexical evidence, under either fill — during knapsack packing it receives lexical-overlap value only (never the structural hop-value term), and during greedy packing a zero-overlap same-file candidate is not packed at all. The implementation SHALL NOT add nodes or edges to `graph.json`.

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

#### Scenario: Query overlap controls inferred-candidate packing
- **WHEN** a focal-file sibling has no lexical overlap with the free-text query but remains within the five-candidate cap
- **THEN** the sibling remains in the gathered candidate set (and in the adaptive reach summary) but is not packed: it receives zero knapsack value under the knapsack fill and is skipped by the greedy fill, so it cannot displace a structural neighbor under any budget

## ADDED Requirements

### Requirement: Knapsack item value scales with slice cost
The knapsack fill's per-item value SHALL scale sublinearly with the item's estimated slice token
cost (relevance × √cost), so that equally-relevant entries compete on relevance rather than on
being small. A per-item value that ignores cost lets the 0/1 fill maximize entry count — packing
many tiny weakly-relevant entries while shedding the larger slices a query is about.

#### Scenario: A large equally-relevant slice is not shed for confetti
- **GIVEN** a focal with one large high-relevance neighbor and several tiny low-relevance
  candidates, under a budget that fits either the large slice or the tiny ones
- **WHEN** `packing = "knapsack"` runs
- **THEN** the large neighbor is included

#### Scenario: Both fills stay within the parity band on the committed fixture
- **WHEN** the parity gate compares greedy and knapsack at the same enforced budgets on the
  committed fixture
- **THEN** each packer meets its pinned floor and the two stay within the recorded parity band

### Requirement: Retrieval gate baselines are anchored to a recorded fixture generation
The committed retrieval fixture pair SHALL be regenerated only by the documented procedure —
the real one-shot build from a clean checkout with a two-run byte-identity check, and
`scripts/bootstrap_eval.py` under the committed config with no grading edits — and every
regeneration SHALL re-pin both gates' baseline constants to freshly measured values in the same
change, recording the generation commit and the measurement environment in the fixture README.
Baseline constants SHALL be transcriptions of measured output, never targets.

#### Scenario: Regeneration re-pins both gates atomically
- **WHEN** the fixture pair is replaced with a regeneration at a newer commit
- **THEN** the same change updates the baseline constants of the parity gate and the end-to-end gate to values measured on the new pair, and the fixture README records the new node/link counts, row count, and generation commit

#### Scenario: The fixture graph is reproducible
- **WHEN** the one-shot pipeline runs twice over the same clean checkout
- **THEN** the two `graph.json` outputs are byte-identical

#### Scenario: Eval rows derive from queries, never labels
- **WHEN** `queries.jsonl` is regenerated
- **THEN** rows are mined from git history by the committed config, grading and thresholds are unchanged, and no row is hand-edited
