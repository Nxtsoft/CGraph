# deterministic-graph-pipeline Specification

## Purpose
TBD - created by archiving change improve-graph-html-view. Update Purpose after archive.
## Requirements
### Requirement: Interactive HTML view reveals community structure
The interactive `graph.html` export SHALL position nodes so that computed community assignments are visible as spatially distinct regions, rather than using community only for color while leaving the layout a uniform frame-filling cloud. The layout SHALL remain deterministic for a given graph (no use of `Math.random`).

#### Scenario: Communities render as separated regions
- **WHEN** the pipeline exports `graph.html` for a graph with multiple detected communities and the view settles
- **THEN** nodes of the same community are drawn closer to one another than to nodes of other communities, so distinct communities read as separate regions

#### Scenario: Layout is not clamped to a viewport box
- **WHEN** the layout settles for a graph larger than the viewport
- **THEN** nodes relax in open space without piling against fixed canvas edges, and the view auto-fits the whole graph so it is visible on load

#### Scenario: Layout is deterministic
- **WHEN** `graph.html` is generated twice for the same graph
- **THEN** the generated layout logic uses only seeded placement (no `Math.random`), so the same graph produces the same layout each load

### Requirement: Interactive HTML view bounds on-screen labels
The interactive `graph.html` export SHALL limit always-on node labels to a bounded set (the highest-degree nodes) and SHALL reveal additional labels progressively on hover, selection, active highlight, search match, and zoom-in, so an overview of a large graph is not an unreadable wall of overlapping text.

#### Scenario: Overview labels are bounded
- **WHEN** `graph.html` is exported for a graph with hundreds of nodes and viewed at the default zoom with no selection or search
- **THEN** only a bounded top-by-degree subset of nodes is labelled, not every node above a fixed radius

#### Scenario: Hidden labels are reachable
- **WHEN** the user hovers, selects, searches, or zooms into a node whose label is hidden in the overview
- **THEN** that node's label becomes visible, so no label is permanently inaccessible

### Requirement: Interactive HTML view selection is reversible
The interactive `graph.html` export SHALL let the user return to the unfocused full-graph view without reloading the page. Clicking empty canvas and pressing `Escape` SHALL clear the current selection and highlight, and the view SHALL provide controls to reset the view and to fit the whole graph to the viewport.

#### Scenario: Escape clears selection
- **WHEN** a node is selected (everything else dimmed) and the user presses `Escape`
- **THEN** the selection and highlight clear and the full graph is shown undimmed

#### Scenario: Clicking empty canvas clears selection
- **WHEN** a node is selected and the user clicks an empty area of the canvas
- **THEN** the selection clears before any pan begins, restoring the unfocused view

#### Scenario: Fit to screen recenters the graph
- **WHEN** the user has panned or zoomed away and activates the fit-to-screen control
- **THEN** the view recenters and scales so the whole graph's bounding box is visible within the viewport

### Requirement: Interactive HTML view supports light and dark themes
The interactive `graph.html` export SHALL render in both a light and a dark theme, defaulting to the operating system's color-scheme preference and providing a control to switch themes. The canvas (background, node strokes, edges, and labels) SHALL follow the active theme, not only the surrounding DOM chrome.

#### Scenario: Theme follows OS preference by default
- **WHEN** `graph.html` is opened with no explicit theme chosen and the OS prefers a dark color scheme
- **THEN** the view renders in dark theme, including the graph canvas

#### Scenario: User can switch themes
- **WHEN** the user activates the theme toggle
- **THEN** the page and the graph canvas switch between light and dark, with node, edge, and label colors updating to remain legible

### Requirement: Detection excludes dependency and virtual-environment trees
Project file detection SHALL skip dependency, build, tooling, virtual-environment,
agent-tooling-config, and linked git-worktree directory trees rather than index their contents as
project source. A directory SHALL be excluded when its name is in the skip list — which includes the
Python ecosystem (`.venv`, `venv`, `site-packages`, `__pycache__`, `.tox`, `.nox`, `.pytest_cache`,
`.mypy_cache`, `.ruff_cache`, `.hypothesis`, `.eggs`) and the agent-CLI / spec-tool config
directories (`.claude`, `.codex`, `.gemini`, `.cursor`, `.factory`, `.opencode`, `.windsurf`,
`.aider`, `.specify`) — OR when it contains a `pyvenv.cfg` virtual-environment marker — OR when it is
a git-worktree checkout tree. A worktree checkout SHALL be detected by either of two structural
markers: (a) a `.git` entry that is a regular file (the live worktree `gitdir:` marker) rather than a
directory, or (b) a directory named `worktrees` whose parent directory name begins with a dot (the
`<.tool>/worktrees/` convention used by agent tools, which also covers stale checkouts whose `.git`
has been pruned). The project root's own `.git` is a directory and SHALL NOT trigger this exclusion,
and a `worktrees` directory under a non-dotted parent (a legitimate source module) SHALL NOT be
excluded. The daemon file watcher SHALL apply the identical exclusion, so a file created under a
skipped tree never produces an incremental update. The same exclusion governs the enrichment chunk
planner, so neither agent-tooling docs nor worktree-duplicated docs are planned for semantic
enrichment. Files that remain in scope are extracted unchanged (parity is held).

#### Scenario: Virtualenv contents are not indexed
- **WHEN** a project root contains a virtualenv (e.g. `research/.venv/lib/pythonX/site-packages/…`)
  and the graph is built
- **THEN** no node has a `source_file` under that `.venv` or any `site-packages` directory, and the
  graph contains only the project's own source

#### Scenario: Oddly-named virtualenv is detected by marker
- **WHEN** a directory not in the name skip list (e.g. `qa-env/`) contains a `pyvenv.cfg` file
- **THEN** the directory and its contents are skipped during detection

#### Scenario: Agent-tooling config directories are not indexed or enriched
- **WHEN** a project root contains agent-CLI or spec-tool config trees (e.g. `.claude/commands/`,
  `.factory/skills/`, `.specify/templates/`)
- **THEN** detection skips them and the enrichment planner does not plan their documents, so they
  contribute neither code nodes nor doc nodes

#### Scenario: Live and stale git-worktree trees are not indexed
- **WHEN** a project root contains agentic git worktrees under the `<.tool>/worktrees/<id>/`
  convention (e.g. `.anvil/worktrees/<uuid>/…`, `.agents/worktrees/<slug>/…`) — some live (`.git` is
  a regular file) and some stale (the `.git` pointer already pruned, only the duplicated source tree
  remaining) — and the graph is built
- **THEN** no node has a `source_file` under any worktree checkout tree (live or stale), and the
  graph contains only the project's own (single-copy) source

#### Scenario: Project root is never skipped as a worktree
- **WHEN** detection enters the project root, whose `.git` is a directory (a normal repository)
- **THEN** the root is walked normally and its source files are indexed; the `.git` marker only
  matches a regular file, and the convention marker only matches a `worktrees` dir under a dotted
  parent

#### Scenario: A legitimate source module named `worktrees` is not skipped
- **WHEN** a project has a real source directory literally named `worktrees` under a non-dotted
  parent (e.g. `packages/core/src/worktrees/`) with no worktree `.git` marker
- **THEN** it is walked normally and its source files are indexed

#### Scenario: Lookalike directory is not over-skipped
- **WHEN** a real source directory has a name that merely contains a skipped token, or matches a
  skipped name without its leading dot (e.g. `my_env_utils/`, `environment/`, `factory/`), and has
  neither a `pyvenv.cfg` nor a `.git` worktree-marker file
- **THEN** it is walked normally and its source files are indexed

#### Scenario: Watcher and detector agree
- **WHEN** the daemon is running and a recognized source file is created under a skipped tree
- **THEN** the watcher emits no event and the resident graph is unchanged, matching what a cold
  detection pass would have produced

#### Scenario: Pathologically deep generated dependency cannot crash the build
- **WHEN** a repo contains a machine-generated dependency file deep in a skipped tree (e.g. a
  NumPy header under `site-packages`) that previously triggered an extraction stack overflow
- **THEN** detection never walks it, so it cannot contribute nodes or destabilize the build

### Requirement: SQL files are indexed as file-level nodes
Project file detection SHALL recognize the `.sql` extension as a known language
(`DetectedLanguage::Sql`), so `.sql` files are classified as code (detected, extracted, and watched
for incremental updates) rather than as enrichment-only documents. For each detected `.sql` file the
deterministic extractor SHALL emit exactly one file-level node with `kind = "sql_file"`, a label
derived from the file name, and the file's `source_file` set — with no symbol nodes and no edges
(file-level only; SQL contents are not parsed). The node SHALL be queryable, enrichable, and
seam-anchorable like any other graph node.

#### Scenario: A SQL file produces one file-level node
- **WHEN** the graph is built over a project containing `.sql` files (e.g. Prisma migrations)
- **THEN** each `.sql` file contributes exactly one node of kind `sql_file` whose `source_file` is
  that file, and no symbol nodes or edges are emitted for it

#### Scenario: SQL files are discoverable
- **WHEN** an agent queries the graph for SQL files (e.g. by kind `sql_file` or a `file:` path match)
- **THEN** the `sql_file` nodes are returned, so the project's data layer is visible in the graph

#### Scenario: Extraction parity is preserved
- **WHEN** the extractor goldens (which contain no `.sql` fixtures) are regenerated
- **THEN** they are unchanged — indexing `.sql` is additive and does not alter extraction of any
  existing language

### Requirement: SQL DDL is extracted into a schema graph
Beyond the file-level `sql_file` node, the deterministic extractor SHALL parse the DDL in `.sql`
files into schema nodes and relationships, matching identifiers in both their **quoted** and
**unquoted** forms (each optionally `schema.`-qualified):

- `CREATE TABLE <name>` SHALL emit a `sql_table` node, and `CREATE TYPE <name> AS ENUM` a
  `sql_enum` node, whether `<name>` is quoted (`"Users"`) or unquoted (`users`). An
  `ALTER TABLE <old> RENAME TO <new>` SHALL emit a `sql_table` node for `<new>`. Their ids SHALL
  be keyed on the entity **name** (independent of the source file), so the same table or enum
  appearing across multiple migration files merges — via the graph builder's id dedup — into a
  single node representing the current schema. The node SHALL record the `source_file` and
  location of its defining statement.
- A foreign key `ALTER TABLE <X> … FOREIGN KEY (…) REFERENCES <Y>` SHALL emit a `references`
  edge from the `sql_table` node for `X` to the `sql_table` node for `Y` (reusing the existing
  `references` relation, so `impact` / typed `explain` / query routing operate over it), for
  quoted and unquoted `<X>` / `<Y>`. Duplicate foreign keys across migrations SHALL collapse via
  edge dedup.
- Identifier handling SHALL reconcile quoted and unquoted references to the same table to one
  node. An **unquoted** identifier's canonical name SHALL be folded to lowercase (PostgreSQL
  semantics) and a **quoted** identifier SHALL keep its written case as its label; node ids are
  then produced through the existing case-folding id normalization (the Graphify id contract), so
  `CREATE TABLE users` and `REFERENCES "users"` resolve to the same `sql_table` node. Because that
  id normalization case-folds, case-variant identifiers (e.g. `"Users"` and `users`) reconcile to
  a single node rather than diverging — a deliberate consequence of the shared id scheme, not a
  SQL-specific rule.

Extraction is regex-based over the (Prisma-style) DDL; SQL is not fully parsed. Forms not matched
(e.g. inline column-level references inside a `CREATE TABLE` body, non-Prisma/non-Postgres
dialects) SHALL simply yield no edge rather than an error.

#### Scenario: Tables, enums, and foreign keys become a schema graph
- **WHEN** the graph is built over `.sql` migrations declaring `CREATE TABLE`, `CREATE TYPE … ENUM`,
  and `ALTER TABLE … FOREIGN KEY … REFERENCES` statements
- **THEN** the graph contains a `sql_table` node per table, a `sql_enum` node per enum, and a
  `references` edge between the owning and referenced tables for each foreign key

#### Scenario: Unquoted DDL is extracted
- **WHEN** a migration declares tables and foreign keys with unquoted identifiers, e.g.
  `CREATE TABLE IF NOT EXISTS skills (…)` and `… FOREIGN KEY (org_id) REFERENCES organizations(id)`
- **THEN** the graph contains the `sql_table` nodes (`skills`, `organizations`) and the `references`
  edge between them, exactly as it would for the equivalent quoted DDL

#### Scenario: Quoted and unquoted references to the same table reconcile
- **WHEN** a table is defined unquoted (`CREATE TABLE organizations`) and later referenced quoted
  (`REFERENCES "organizations"`), or defined quoted (`CREATE TABLE "organizations"`) and referenced
  unquoted (`REFERENCES organizations`)
- **THEN** the reference resolves to the single existing `sql_table` node (no dangling edge to a
  phantom node), because unquoted identifiers fold to lowercase and quoted identifiers preserve case

#### Scenario: Case-variant identifiers reconcile to one node
- **WHEN** a table is defined unquoted (`CREATE TABLE users`) and a foreign key references it with
  different case (`REFERENCES "Users"`)
- **THEN** the reference resolves to the single existing `sql_table` node (no dangling edge),
  because node ids are case-folded by the shared id normalization — case-variant identifiers are
  one node, consistent with the Graphify id contract rather than PostgreSQL's case sensitivity

#### Scenario: A table merges across migrations into one node
- **WHEN** the same table is created in one migration file and altered in others
- **THEN** the graph contains exactly one `sql_table` node for it (its id keyed on name, not file),
  with the foreign keys added by later migrations attached as `references` edges

#### Scenario: Schema is queryable via existing relation-aware ops
- **WHEN** an agent runs `impact` or `explain --relation references` on a `sql_table` node
- **THEN** the response returns the tables related by foreign keys, because the foreign keys are
  `references` edges the existing ops already traverse

#### Scenario: Goldens are unaffected by the unquoted extension
- **WHEN** the extractor goldens (which contain no `.sql` fixtures) are regenerated
- **THEN** they are unchanged — extending SQL identifier matching is additive and does not alter
  extraction of any other language

### Requirement: Native deterministic pipeline
The system SHALL provide a native one-shot pipeline that detects project files, extracts language fragments, builds and deduplicates the graph, clusters communities, analyzes graph metrics, and exports deterministic outputs.

#### Scenario: One-shot graph build completes
- **WHEN** the user runs the native one-shot command against a supported project root
- **THEN** the system produces a deterministic graph without requiring a daemon or semantic enrichment

#### Scenario: Unsupported file is skipped safely
- **WHEN** the file detector encounters an unsupported or ignored file
- **THEN** the system excludes that file without aborting the pipeline

### Requirement: Graphify fragment contract
The system SHALL emit and consume extraction fragments compatible with Graphify's fragment shape, including `nodes`, `edges`, optional `hyperedges`, source metadata, relation names, confidence labels, and confidence scores where applicable.

#### Scenario: Extractor emits compatible fragment
- **WHEN** a supported source file is extracted
- **THEN** the extracted fragment contains Graphify-compatible node and edge records for downstream build and merge stages

#### Scenario: Extractor failure is contained
- **WHEN** one file extractor throws or fails to parse
- **THEN** the system records a warning and continues the batch with an empty fragment for that file

### Requirement: ID normalization parity
The system SHALL normalize node identifiers byte-for-byte compatibly with Graphify's `_make_id` and build normalization behavior, including Unicode normalization, word-character handling, underscore collapse, and case folding.

#### Scenario: Unicode fixture matches reference
- **WHEN** the native normalizer runs against ASCII, accented, composed, decomposed, CJK, and Cyrillic identifier fixtures
- **THEN** every output matches the Python Graphify reference output exactly

### Requirement: Tree-sitter extraction parity
The system SHALL use tree-sitter grammars and per-language extraction logic to match Graphify's node and edge sets for supported language fixtures.

#### Scenario: Language golden matches reference
- **WHEN** a native extractor runs against a ported Graphify language fixture
- **THEN** the produced node and edge sets match the reference fixture except for documented ordering differences

### Requirement: Graph build and dedup parity
The system SHALL merge fragments into a graph with Graphify-compatible per-file deduplication, cross-file idempotency, semantic merge behavior, and raw-call resolution.

#### Scenario: Duplicate symbols merge correctly
- **WHEN** multiple fragments contain semantically duplicate nodes
- **THEN** the build stage merges them according to the reference dedup pipeline and avoids ghost duplicate nodes

#### Scenario: Ambiguous raw call remains unresolved
- **WHEN** a raw call matches only common or ambiguous names
- **THEN** the system avoids creating a misleading extracted call edge

### Requirement: Semantic dedup never merges file nodes
Semantic dedup SHALL exclude nodes of kind `file` from both dedup passes; a file's identity is
its path, never a label match.
*Evidence: `src/engine/dedup.cpp:229-231` (at f3b9837).*

#### Scenario: Sibling files with near-identical names both survive
- **GIVEN** file nodes `viewers/compiq-viewer.tsx` and `viewers/compiq-viewer-states.tsx` whose labels score above the fuzzy threshold
- **WHEN** semantic dedup runs
- **THEN** both file nodes survive and every edge referencing them still resolves

### Requirement: Exact-pass identity is normalized label, source file, and declaration site
For nodes with a non-empty `source_file`, semantic dedup SHALL merge two nodes in its exact pass
only when their normalized labels (`make_id`: NFKC, non-word runs to `_`, casefold), their
`source_file`, and their declaration site (start line AND start column; empty when
`source_location` is absent) are all equal.
*Evidence: `src/engine/dedup.cpp:250-263`, `src/engine/normalize.cpp:111-121` (at f3b9837).*

#### Scenario: A double-extraction of one symbol merges
- **GIVEN** two nodes labeled `same_site_twice` at `twice.cpp` line 12
- **WHEN** semantic dedup runs
- **THEN** one node survives

#### Scenario: Overloads sharing a line survive
- **GIVEN** three `three_up` nodes at `cols.cpp` line 7 with columns 0, 30, and 62
- **WHEN** semantic dedup runs
- **THEN** all three survive — the site is line AND column

#### Scenario: Same name in two files is two symbols
- **GIVEN** `HelperWidget` declared in `one.tsx` and in `two.tsx`
- **WHEN** semantic dedup runs
- **THEN** both survive

#### Scenario: Exact identity is separator- and case-insensitive
- **GIVEN** `Payment Service` and `payment_service` in one file at one site
- **WHEN** semantic dedup runs
- **THEN** their normalized labels are equal and the exact pass unites them

### Requirement: Nodes without a source file are excluded from the exact pass
Semantic dedup SHALL NOT enter a node with an empty `source_file` into the exact pass; the fuzzy
pass is such a node's only merge path.
*Evidence: `src/engine/dedup.cpp:250,337-341` (at f3b9837).*

#### Scenario: Identical source-less concepts merge via the fuzzy pass
- **GIVEN** two `concept` nodes labeled `OpenSpec Change Lifecycle` with no `source_file`, the label clearing the fuzzy entropy gate
- **WHEN** semantic dedup runs
- **THEN** they merge into one node

#### Scenario: Identical low-entropy source-less labels never merge
- **GIVEN** two source-less nodes with the identical low-entropy label `aaa`
- **WHEN** semantic dedup runs
- **THEN** both survive — they enter neither the exact pass nor the fuzzy pass

### Requirement: Only high-entropy labels enter the fuzzy pass
Semantic dedup SHALL admit a node to the fuzzy pass only when its normalized label has length at
least 3 and Shannon entropy at least `entropy_floor` (default 2.5); the exact pass has no
entropy gate.
*Evidence: `src/engine/dedup.cpp:60-62,265-268`, `src/engine/include/cgraph/dedup.hpp:11` (at f3b9837).*

#### Scenario: A low-entropy label survives a high similarity score
- **GIVEN** a node labeled `aaaaaa` alongside similar high-entropy labels
- **WHEN** semantic dedup runs
- **THEN** the `aaaaaa` node survives untouched

### Requirement: Fuzzy candidates come from LSH bands and community buckets
Semantic dedup SHALL compare a fuzzy pair only when the two nodes share a MinHash/LSH band
(16 hashes, bands of 4, over 3-gram shingles of the normalized label) or the same non-empty
`community` property; pairs colliding in neither MAY be missed — candidate generation is
approximate by design.
*Evidence: `src/engine/dedup.cpp:21-22,104-141,270-281` (at f3b9837).*

#### Scenario: Same-community pairs are always compared
- **GIVEN** two high-entropy nodes with `community = "payments"` whose MinHash bands do not collide
- **WHEN** semantic dedup runs
- **THEN** the pair is still scored, via the shared community bucket

### Requirement: Fuzzy merges require the similarity threshold
Semantic dedup SHALL merge a fuzzy candidate pair only when the Jaro-Winkler similarity of the
normalized labels is at least `jaro_winkler_threshold` (default 0.92), lowered to
`same_community_threshold` (default 0.88) when both nodes carry the same non-empty community.
*Evidence: `src/engine/dedup.cpp:53-58,302-309`, `src/engine/include/cgraph/dedup.hpp:12-13` (at f3b9837).*

#### Scenario: A near-identical pair in one community merges at the lower threshold
- **GIVEN** `PaymentServic` and `PaymentServce`, both in community `payments`
- **WHEN** semantic dedup runs
- **THEN** they merge

#### Scenario: Cross-file similar code symbols merge (Graphify parity)
- **GIVEN** `PaymentService` in `a.cpp` and `Payment Service` in `b.cpp`, no locations, no shared community
- **WHEN** semantic dedup runs
- **THEN** they merge into one node — the pair passes the threshold and no guard fires

### Requirement: Prefix extensions and short labels are blocked from fuzzy merge
Semantic dedup SHALL NOT fuzzy-merge a pair where one label is a strict prefix of the other, and
SHALL NOT fuzzy-merge a pair whose longer label is under 12 characters unless the labels are
same-length, differ at exactly one position, and score at least 0.97.
*Evidence: `src/engine/dedup.cpp:89-102,310-312` (at f3b9837).*

#### Scenario: A component and its props type stay distinct
- **GIVEN** `MessageBubble` and `MessageBubbleProps` in one file
- **WHEN** semantic dedup runs
- **THEN** both survive

### Requirement: From-source and source-less nodes never fuzzy-merge
Semantic dedup SHALL NOT fuzzy-merge two nodes whose `source_file` presence differs — a code
symbol and an enrichment concept are different species.
*Evidence: `src/engine/dedup.cpp:320-329` (at f3b9837).*

#### Scenario: A concept does not absorb a code symbol
- **GIVEN** a sited `PaymentProcessor` function node and a source-less `PaymentProcessor` concept
- **WHEN** semantic dedup runs
- **THEN** both survive

### Requirement: Identical from-source labels belong to the exact pass alone
Semantic dedup SHALL NOT fuzzy-merge two from-source nodes whose normalized labels are
identical; only the exact pass (file + site) may unite them.
*Evidence: `src/engine/dedup.cpp:357-359` (at f3b9837).*

#### Scenario: Cross-file identical labels stay apart in the fuzzy pass
- **GIVEN** `HelperWidget` in `one.tsx` and `two.tsx`, both high-entropy
- **WHEN** the fuzzy pass scores them at 1.0
- **THEN** they are not united

### Requirement: Distinct declaration sites never fuzzy-merge
Semantic dedup SHALL NOT fuzzy-merge two nodes that both carry a `source_location` unless their
file, start line, and start column are all equal.
*Evidence: `src/engine/dedup.cpp:375-380` (at f3b9837).*

#### Scenario: Similar function names at different sites survive
- **GIVEN** `validate_semantic_fragment_json` at `validation.cpp:9` and `validate_semantic_fragment_file` at `validation.cpp:15`, scoring above the threshold
- **WHEN** semantic dedup runs
- **THEN** both survive

### Requirement: Merges keep the earliest node and rewrite edges
When nodes merge, semantic dedup SHALL keep the group's earliest node (lowest index) as the
survivor, rewrite every edge endpoint to the survivor's id, and collapse edges that become
duplicates under the (source, relation, target) key.
*Evidence: `src/engine/dedup.cpp:37-47,152-186` (at f3b9837).*

#### Scenario: No edge references a merged-away id
- **GIVEN** edges pointing at nodes that merge
- **WHEN** semantic dedup runs
- **THEN** every edge endpoint resolves to a surviving node id

### Requirement: Neighborhood dedup scopes to changed files
`semantic_dedup_neighborhood` SHALL consider a merge only when at least one node of the pair has
its `source_file` in the changed-files set (for the exact pass, the node itself must be in
scope), and SHALL be a no-op when the changed set is empty. A scoped pair MAY merge with an
unchanged file's node — that is how a re-extracted duplicate finds its original.
*Evidence: `src/engine/dedup.cpp:213-215,250,292-294,478-487` (at f3b9837).*

#### Scenario: An unrelated pair is not re-examined
- **GIVEN** a neighborhood dedup scoped to `{a.cpp}`
- **WHEN** two similar nodes both sourced from `b.cpp` are candidates
- **THEN** they are not compared

#### Scenario: Empty change set is a no-op
- **GIVEN** an empty changed-files set
- **WHEN** `semantic_dedup_neighborhood` is called
- **THEN** the graph is unchanged

### Requirement: Graph analysis and exports
The system SHALL compute community assignments, centrality-derived god-node rankings, cross-community surprise signals, and Graphify-compatible exports.

#### Scenario: Graph JSON is compatible
- **WHEN** the native pipeline exports `graph.json`
- **THEN** existing Graphify-compatible loaders can parse the output as NetworkX node-link data

#### Scenario: Analysis output is available
- **WHEN** clustering and analysis complete
- **THEN** clients can access community, centrality, and surprise metadata needed by query and reporting features

### Requirement: Verification gates
The system SHALL include automated parity tests, sanitizer builds, fuzz targets, and benchmarks before long-tail language, exporter, or host integration fan-out.

#### Scenario: Parity gate blocks fan-out
- **WHEN** native one-shot output has unexplained missing or spurious graph nodes or edges against the reference corpus
- **THEN** implementation does not proceed to long-tail integrations until the difference is fixed or explicitly documented

### Requirement: Fragment merge
The graph build SHALL merge per-file extraction fragments into a single graph, deduplicating
nodes by normalized id, edges by (source, relation, target), and hyperedges by id, with the
first occurrence of any duplicate retained. The merge SHALL complete in time linear in the total
number of fragment nodes and edges, and SHALL NOT rebuild its deduplication index from the
accumulated graph on a per-fragment basis.

#### Scenario: Duplicates are removed, first occurrence wins
- **WHEN** fragments contain nodes, edges, or hyperedges whose dedup key already appeared in an
  earlier fragment or earlier in the same fragment
- **THEN** the merged graph keeps only the first occurrence of each key and discards the rest

#### Scenario: Bulk merge stays linear
- **WHEN** a large number of fragments are merged in one build
- **THEN** total merge time grows linearly with total fragment size, not with file count squared

### Requirement: File extraction
The system SHALL extract a fragment, raw calls, and raw relations from each detected project file
using the language-appropriate extractor. Extraction across files MAY execute concurrently, and
the resulting sequence of per-file extraction results SHALL be identical to extracting the same
files serially in detection order.

#### Scenario: Parallel extraction matches serial output
- **WHEN** a set of detected files is extracted concurrently
- **THEN** the per-file results are produced in detection order and each result is identical to
  extracting that file on its own, so the merged graph is byte-identical to a serial build

#### Scenario: Unextractable file is isolated
- **WHEN** one file fails to extract (missing, too large, or no registered extractor)
- **THEN** its result carries the warning and an empty fragment, and the other files in the batch
  are unaffected

### Requirement: One-shot operation stats
A one-shot build SHALL record per-phase wall-clock timings (extract, merge, resolve, dedup,
community detection, analysis) and counters (files extracted, files reused from cache, node count,
edge count) measured at the pipeline orchestration boundary. It SHALL write these to a sidecar
`stats.json` in the output directory and emit a single human-readable summary line to stderr. The
build SHALL NOT embed stats in `graph.json`; the node-link output SHALL remain byte-identical to a
build with stats disabled, preserving the Graphify parity contract.

#### Scenario: Build records phase timings and counts
- **WHEN** a one-shot build completes over a non-empty source tree
- **THEN** every recorded phase timing is greater than zero, the node and edge counters equal the
  resulting snapshot's `nodes.size()` and `edges.size()`, and `cgraph-out/stats.json` exists and
  parses as JSON

#### Scenario: graph.json parity is preserved
- **WHEN** a build is run with operation stats enabled
- **THEN** the produced `graph.json` is byte-identical to the parity golden for the same source tree

#### Scenario: Stderr summary is human-readable
- **WHEN** a one-shot build completes
- **THEN** stderr contains one summary line reporting file count, node count, edge count, and total
  build time in human units

### Requirement: Modeled cache-saving estimate
When a build or rescan reuses cached extractions, the stats output SHALL include a modeled
cache-saving estimate derived as `files_reused × mean(per-file extract time)` from measured
timings, presented under a key that identifies it as an estimate. The estimate SHALL be omitted —
never fabricated, hardcoded, or computed from a zero mean — when no extraction ran in the session
to establish a per-file mean.

#### Scenario: Estimate present when reuse and timings exist
- **WHEN** a rescan reuses at least one cached file and at least one file was actually extracted
- **THEN** the stats output includes a cache-saving estimate equal to
  `files_reused × mean(extract_ms)`, labeled as an estimate

#### Scenario: Estimate omitted on full cache hit
- **WHEN** a rescan reuses files but extracts none (no per-file mean available this session)
- **THEN** no cache-saving estimate is emitted, rather than a fabricated or zero value

### Requirement: Go source files are extracted through the configured tree-sitter path
The deterministic extractor SHALL handle `.go` files (which project file detection already maps
to `DetectedLanguage::Go`) via a declarative `LanguageConfig` over the shared walker (no bespoke
extractor translation unit). For each Go file it SHALL emit: a file node; `type` nodes for named types
(`type_spec` and `type_alias` — structs, interfaces, aliases); `function` nodes for
`function_declaration` and `method_declaration` (methods keep their bare name; the receiver is
not resolved); module stub nodes + file-level `imports` edges for each `import_spec` quoted path
(resolved against project files by suffix, unresolved stubs dropped); and raw calls from
`call_expression` — a `selector_expression` target is recorded as a same-file member call
carrying the bare field name. IDs flow through the existing normalization contract unchanged.

#### Scenario: A Go file produces real symbols
- **WHEN** the graph is built over a project containing `type Service struct{}`, a
  pointer-receiver method `func (s *Service) Run()`, and a plain function
- **THEN** the graph contains a `type` node "Service" and `function` nodes "Run" and the plain
  function, each contained by the file node

#### Scenario: Go calls resolve like other languages
- **WHEN** a Go function body calls a same-file function `helper()` and a package function
  `fmt.Println(...)`
- **THEN** `helper` yields a plain raw call (project-resolvable) and `Println` a member call that
  resolves only within the caller's file, never by project-wide name guess

#### Scenario: Persisted graphs from the pre-Go extractor are not fast-loaded
- **WHEN** a daemon restarts over any project with an index manifest written by a binary older
  than this change
- **THEN** the version key mismatch forces a full rebuild instead of serving the stale
  symbol-less graph

### Requirement: Detected-but-unextracted files are counted per language
The pipeline SHALL maintain a per-language map of detected files that no registered extractor
handles (`unextracted`: language name -> file count), exposed by `unextracted_counts` over any
detected-file set and included in the one-shot `stats.json`. Registry membership is answered by
`has_registered_extractor` (tree-sitter config or non-grammar extractor). `Unknown` files are
excluded (they are not detected as project files).

#### Scenario: A coverage hole is visible in one-shot stats
- **WHEN** `cgraph --root` runs over a project containing a `.cs` file
- **THEN** `stats.json` contains `"unextracted": {"csharp": 1}` alongside the build counters

#### Scenario: Total coverage yields an empty map
- **WHEN** every detected file's language has a registered extractor
- **THEN** `unextracted` is an empty object, not absent

### Requirement: Canonical code content root
The deterministic graph pipeline SHALL compute a `sha256-merkle-v1` root from one leaf per detected code file, where each leaf binds the normalized project-relative path to that file's SHA-256 content hash. Root construction SHALL be deterministic and independent of input enumeration order.

#### Scenario: Identical source trees have identical roots
- **WHEN** the same path-and-content entries are supplied in different orders
- **THEN** the pipeline returns the same root and leaf count

#### Scenario: Source identity changes with relevant inputs
- **WHEN** file content, normalized path, addition, or deletion changes
- **THEN** the pipeline returns a different root

#### Scenario: Empty source tree has a stable identity
- **WHEN** no detected code files exist
- **THEN** the pipeline returns the domain-separated empty-tree root with a zero leaf count

### Requirement: Content-verified code rescan
A content-verified code rescan SHALL hash every currently detected code file, SHALL NOT accept a metadata-only cache hit, and SHALL reuse an existing extraction only when the freshly computed content hash matches that extraction's stored hash.

#### Scenario: Equal-length preserved-mtime edit is invalidated
- **WHEN** a code file is overwritten with different equal-length bytes and its original modification time is restored
- **THEN** a content-verified rescan computes a different file hash, re-extracts that file, and publishes a different content root

#### Scenario: Timestamp-only rewrite reuses extraction
- **WHEN** file metadata changes but freshly computed content is byte-identical
- **THEN** the rescan records a content-hash hit and reuses the existing extraction

#### Scenario: Verified rescan removes vanished input
- **WHEN** a previously indexed code file is absent from the verified source tree
- **THEN** its extraction and leaf are removed before the new graph and root are published

### Requirement: Symbol labels name the symbol
A function, method, or field node's `label` SHALL be the symbol's name, not its declaration text. For C and C++ the name SHALL be the leaf identifier reached by descending the declarator through pointer, reference, array, and parenthesized wrappers, reduced to its tail for a qualified name. A label SHALL NOT contain a parameter list, a return type, or a newline. This aligns C and C++ with Python, JavaScript, and TypeScript, which already emit bare names.

#### Scenario: A C++ function label is its name
- **WHEN** the pipeline extracts `GraphSnapshot merge_fragments(std::span<const Fragment> fragments)`
- **THEN** the node's label is exactly `merge_fragments`

#### Scenario: Declarator decoration does not leak into the label
- **WHEN** the pipeline extracts a function returning a reference, a destructor, and an operator overload
- **THEN** the labels contain no leading `&` or `*`, and are the plain names `~Foo` and `operator==`

#### Scenario: No symbol is lost to an unnameable declarator
- **WHEN** a declaration's declarator yields no leaf identifier
- **THEN** the existing name-field path still supplies a label and no node is dropped

### Requirement: Call sites are keyed on the callee name
A call site's callee key SHALL be the callee's name, resolved through the grammar: the leaf name reached by descending a qualified identifier's `name` field (which nests to arbitrary depth), a template function's or template method's `name`, and the property name of a member access.

Resolution SHALL NOT be performed by reducing the callee's text at a scope separator. `::` legitimately appears in nine distinct callee node types, so no text rule distinguishes them: `ns::make<zoo::Beast>` reduced at its last `::` yields `Beast>`, which normalizes to `Beast` and fabricates a call to an unrelated struct while losing the real one.

A callee that names explicitly global scope (a qualified identifier with no scope, `::stat(...)`) SHALL resolve to nothing: it names a platform symbol, not a project one. Resolution order SHALL remain a symbol declared in the caller's own file, then a project-wide symbol whose name is unique. The exactly-one-candidate rule SHALL continue to govern the project-wide tier. A member call SHALL remain scoped to the caller's own file, because the receiver type is unknown. Confidence grading is unchanged: `EXTRACTED` when the caller's file imports the resolved symbol or its module, `INFERRED` when it is only a name match.

#### Scenario: A cross-file call to a parameterized function resolves
- **GIVEN** `graph_builder.cpp` declares `merge_fragments`
- **AND** `pipeline.cpp` contains the call site `merge_fragments(fragments)`
- **THEN** a `CALLS` edge exists from the enclosing symbol in `pipeline.cpp` to the `merge_fragments` node

#### Scenario: A qualified call resolves
- **WHEN** `cli/main.cpp` contains `cgraph::run_one_shot(args.root)`
- **THEN** a `CALLS` edge exists to the `run_one_shot` node

#### Scenario: A C++ member call resolves within its own file
- **GIVEN** a struct declares a method `is_live`
- **AND** another function in the same file calls `handle->is_live()`
- **THEN** a `CALLS` edge exists to that method node
- **AND** the call is never matched project-wide

#### Scenario: A template argument is never mistaken for the callee
- **GIVEN** calls `wrapper<zoo::Beast>(1)` and `ns::made<zoo::Beast>(2)`
- **THEN** no `CALLS` edge to `Beast` is emitted
- **AND** edges to `wrapper` and `made` are emitted

#### Scenario: An explicitly global callee resolves to nothing
- **WHEN** a function calls `::stat_local_probe(p)` and a local symbol of that name exists
- **THEN** no `CALLS` edge to the local symbol is emitted

#### Scenario: A same-file overload set resolves to its first declaration
- **GIVEN** two declarations in one file share the name `add`
- **WHEN** a call to `add` in that file is resolved
- **THEN** a `CALLS` edge to the first declaration is emitted with `INFERRED` confidence
- **AND** `resolved_overload_first` is incremented

#### Scenario: A project-wide ambiguous name resolves to nothing and is counted
- **GIVEN** two files each declare `write_text` and neither is the caller's file
- **WHEN** a call to `write_text` is resolved
- **THEN** no `CALLS` edge is emitted and `dropped_ambiguous` is incremented

#### Scenario: Overloads sharing one line remain distinct nodes
- **GIVEN** three declarations of `triple` on a single line
- **THEN** the graph holds three distinct `triple` nodes

### Requirement: A call target must be callable
Project-wide call resolution SHALL only consider candidates whose kind can be invoked, and that set SHALL be the same one the per-file table admits: `function`, `class`, `type`, and `variable`. `class` is eligible because `Foo()` is a constructor call in Python and JavaScript, and `type`/`variable` because a module-level binding can hold a callable. A `field` node SHALL NOT be the target of a `CALLS` edge.

The two resolution tiers SHALL agree on what counts as a symbol. The defect being corrected is that the project-wide tier applied no kind filter at all, not that it applied a different one.

#### Scenario: A syscall name colliding with a struct field emits no edge
- **GIVEN** a struct declares a field named `connect`
- **AND** a function invokes the platform `::connect` symbol
- **THEN** no `CALLS` edge is emitted to the field node
- **AND** the call is counted in `dropped_unknown`

### Requirement: A namespace is not a class
A C++ `namespace_definition` SHALL NOT produce a node of kind `class`, and SHALL NOT parent its members. Its members attach to the enclosing scope -- in practice the file -- so a namespace never contributes a `method` edge.

Ids are per-file, so a namespace-as-class minted one node per file all bearing the same label, grouping nothing across files while making every member read as a method of a type.

#### Scenario: A namespace produces no class node
- **WHEN** the pipeline extracts a file containing `namespace demo { int helper(int x); }`
- **THEN** no node of kind `class` labelled `demo` exists

#### Scenario: A namespace member attaches to its file
- **WHEN** a namespace contains a free function
- **THEN** the function has an incoming `contains` edge from its file node
- **AND** it has no incoming `method` edge

#### Scenario: A real class still owns its methods
- **WHEN** a class declares a method inline
- **THEN** the containment edge relation is still `method`

#### Scenario: No symbol is lost when the namespace node disappears
- **WHEN** a project's namespaces stop producing nodes
- **THEN** the count of function, class, field, import and variable nodes is unchanged
- **AND** no member is left without an incoming containment edge

### Requirement: Call resolution is measurable from a committed artifact
`BuildStats` SHALL report, per build, `raw_calls_total` and a partition of it: `resolved_same_file`, `resolved_project_unique`, `dropped_unknown`, `dropped_ambiguous`, and `dropped_self`. The partition SHALL sum to `raw_calls_total`, and every field SHALL be serialized to `stats.json`. `resolved_overload_first` SHALL also be reported as a subset of `resolved_same_file`.

#### Scenario: The resolution rate is readable without instrumenting a build
- **WHEN** `cgraph --root PATH --out DIR` completes
- **THEN** `DIR/stats.json` contains every field
- **AND** the five partition fields sum to `raw_calls_total`

### Requirement: An enrichment node's identity includes the file it came from
A node of an enrichment kind -- `document` and `media`, matched case-insensitively because `kind` is unvalidated host input -- SHALL NOT be merged by label similarity with ANY node unless both nodes share the SAME non-empty `source_file`. A document's identity is scoped by the file it was written from: a differing file keeps them apart, a shared non-empty file may still merge (a genuine re-extraction), and a MISSING file is no proof of shared identity and also keeps them apart. This holds against any counterpart, enrichment or code symbol alike; unlike a `file` node (excluded from dedup entirely), a document still participates in dedup within its own file.

A `concept` -- which is not a file-scoped kind -- is unaffected: with no file to scope to, label similarity remains its only merge signal. A `document` or `media` node that omits `source_file` is NOT treated like a concept: it is kept apart rather than label-merged, so a non-compliant host that drops `source_file` does not have its similar-named documents silently deleted.

#### Scenario: A change's proposal, tasks and design stay three documents
- **GIVEN** document nodes for `x/proposal.md`, `x/tasks.md` and `x/design.md`
- **AND** their labels are similar enough to cross the fuzzy threshold
- **WHEN** semantic dedup runs
- **THEN** all three nodes survive
- **AND** every edge that referenced them still resolves

#### Scenario: Two records of one document still merge
- **GIVEN** two document nodes with the same non-empty `source_file`
- **WHEN** semantic dedup runs
- **THEN** they merge into one node

#### Scenario: Source-file-less documents are not merged by label
- **GIVEN** two `document` nodes with similar labels, a shared community, and no `source_file`
- **WHEN** semantic dedup runs
- **THEN** both survive

#### Scenario: Concept dedup is unchanged
- **GIVEN** two `concept` nodes with the same label and no `source_file`
- **THEN** they merge into one node

### Requirement: Rust source files are extracted through the configured tree-sitter path
The system SHALL detect `.rs` files as Rust and extract, via the grammar-driven configured
extractor, function nodes (free functions, impl methods, and trait method signatures), type nodes
(struct, enum, union, trait, and type alias), and call edges — with the same Graphify-compatible
fragment shape as other configured languages. A `Type::method()` or turbofish `foo::<T>()` callee
SHALL be reduced to its leaf name; an `x.method()` call SHALL be recorded as a same-file member
call.

Additionally, Rust `use` declarations SHALL resolve to project files through the module layout:
`::` paths map to `<path>.rs` or `<path>/mod.rs`, leading `crate::`/`self::`/`super::` segments
are stripped before unique-suffix matching, grouped/aliased/glob forms are expanded per leaf
(an alias resolves via the original name and never becomes a node), and an imported item SHALL
remap to its declared node in the resolved file when one exists (the file node otherwise). A
path resolving to no project file, or to more than one candidate file, SHALL leave no stub node
and no dangling edge in the graph.

#### Scenario: Rust structs and functions become nodes
- **GIVEN** a `.rs` file declaring a `struct`, an `impl` method, and a free function
- **WHEN** the one-shot pipeline runs
- **THEN** the struct is a `type` node, the impl method and free function are `function` nodes, and the file `contains` them

#### Scenario: Rust calls become edges, including same-file member calls
- **GIVEN** an impl method that calls a free function and `self.other_method()`
- **WHEN** extraction runs
- **THEN** a `CALLS` edge to the free function is produced, and the `self.method()` call resolves to the same-file method

#### Scenario: A qualified call is reduced to its name
- **GIVEN** a call `Type::make()`
- **WHEN** extraction runs
- **THEN** the callee is reduced to `make` (not dropped) and kept eligible for resolution

#### Scenario: A plain item import resolves to the declared symbol
- **GIVEN** `src/a.rs` containing `use crate::foo::bar::Baz;` and `src/foo/bar.rs` declaring `Baz`
- **WHEN** the one-shot pipeline runs
- **THEN** the graph contains an `imports` edge from `src/a.rs`'s file node to `Baz`'s node

#### Scenario: The mod.rs layout resolves
- **GIVEN** the module lives at `src/foo/bar/mod.rs` instead of `src/foo/bar.rs`
- **WHEN** the same import resolves
- **THEN** it resolves to that file's declared symbol (or its file node when the item is not declared)

#### Scenario: An external crate import leaves the graph unchanged
- **GIVEN** a file containing `use serde::Serialize;` and no project file matching the path
- **WHEN** the one-shot pipeline runs
- **THEN** no import stub node and no edge referencing it remains

#### Scenario: Grouped and aliased imports expand per leaf
- **GIVEN** a file containing `use crate::foo::{bar::Baz, qux as Q};`
- **WHEN** extraction and resolution run
- **THEN** one resolved edge exists per leaf, and `qux as Q` resolves via the original name `qux`

#### Scenario: A glob names the module itself
- **GIVEN** a file containing `use crate::foo::bar::*;`
- **WHEN** the one-shot pipeline runs
- **THEN** the graph contains an `imports` edge from the importing file to `foo/bar`'s module file

