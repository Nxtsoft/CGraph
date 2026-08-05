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

