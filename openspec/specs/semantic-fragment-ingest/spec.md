# semantic-fragment-ingest Specification

## Purpose
TBD - created by archiving change build-native-graphify-variant. Update Purpose after archive.
## Requirements
### Requirement: Host-orchestrated semantic enrichment
The system SHALL support semantic enrichment through host-generated fragments and SHALL NOT require the native binary to call LLM APIs or store model provider credentials.

#### Scenario: No model call from binary
- **WHEN** semantic enrichment is requested
- **THEN** the native tool emits work for the host to perform and does not make an LLM API request itself

### Requirement: Chunk plan generation
The system SHALL generate chunk plans for uncached or stale docs, media, and other semantic inputs, grouped into bounded chunks suitable for host subagents.

#### Scenario: Changed doc enters chunk plan
- **WHEN** a documentation file changes and no valid semantic cache entry exists for its content hash
- **THEN** the next enrichment plan includes that file in a chunk for host processing

#### Scenario: Cached content is skipped
- **WHEN** a semantic input has an unchanged content hash with a valid cached fragment
- **THEN** the chunk plan excludes that input from new host work

### Requirement: Fragment validation
The system SHALL validate every dropped semantic fragment before merging it into the graph.

#### Scenario: Valid fragment merges
- **WHEN** the host writes a valid `chunk_NN.json` fragment to the drop directory
- **THEN** the system merges it through the single-writer path and updates the semantic cache

#### Scenario: Malformed fragment is rejected
- **WHEN** the host writes malformed JSON or a fragment that violates the schema
- **THEN** the system rejects the fragment, records an error, and leaves the graph intact

### Requirement: Enrichment state visibility
The system SHALL expose whether semantic enrichment is idle, pending, running, stale, or failed for relevant files and for the graph as a whole.

#### Scenario: Stale enrichment is visible
- **WHEN** a semantic input changes after deterministic graph output is available
- **THEN** status reports enrichment as stale or pending without blocking deterministic graph queries

### Requirement: Incremental semantic cache
The system SHALL store semantic enrichment results by content hash and invalidate them when source content changes.

#### Scenario: File edit invalidates semantic cache
- **WHEN** a semantic source file's content hash changes
- **THEN** the prior semantic fragment is marked stale and the file is eligible for the next chunk plan

### Requirement: Enrichment planning reuses unchanged-file hashes
Enrichment planning SHALL maintain a per-file stat index (path, size, modification time, content
hash) and, before hashing a doc/media file, classify it against that index. When a file's size
and modification time match the indexed entry (a stat hit), planning SHALL reuse the stored hash
without reading the file. Planning SHALL read and re-hash only files that are new or whose size
or modification time changed. The chunk plan produced — its chunks, cache-hit count, and stale
count — SHALL be identical whether each file's hash was reused or recomputed.

#### Scenario: Unchanged files are not re-hashed across plans
- **WHEN** a plan runs over a tree of N doc/media files with an empty stat index, then runs again
  reusing the resulting index with no file changes
- **THEN** the first plan reads and hashes all N files and the second plan reads and hashes none,
  reusing all N stored hashes, and both plans produce the same chunks, cache-hit count, and stale
  count

#### Scenario: A changed file is re-hashed, others reused
- **WHEN** one file's modification time or size changes between two plans
- **THEN** only that file is read and re-hashed and the remaining files reuse their stored hashes

#### Scenario: Plan output is independent of hash reuse
- **WHEN** the same tree and cache are planned once cold (all hashed) and once warm (all reused)
- **THEN** the two plans are equal in chunk membership, content hashes, cache-hit count, and
  stale count

### Requirement: Enrichment stat index persists across restarts
The stat index SHALL be persisted to the semantic drop directory and reloaded on daemon start, so
that a restart over an unchanged tree reuses stored hashes rather than re-hashing every doc/media
file. A missing or unreadable index SHALL be treated as empty (cold), causing a one-time re-hash
without error.

#### Scenario: Restart reuses persisted hashes
- **WHEN** the daemon has planned a tree, persisted the stat index, and restarted with the tree
  unchanged
- **THEN** the first plan after restart reuses the persisted hashes and reads no unchanged file

#### Scenario: Absent index is treated as cold
- **WHEN** no stat index file exists
- **THEN** planning proceeds as a cold plan that hashes every file, with no error

### Requirement: Enrichment re-plans only on relevant change
After the initial plan that populates the pending counts at startup, enrichment SHALL re-plan
only when doc/media files change. A code-only operation — a build or an `update .` rescan that
touches no doc/media file — SHALL NOT trigger an enrichment re-plan or a doc-tree walk.

#### Scenario: Code-only rescan does not re-plan
- **WHEN** an `update .` rescan runs and no doc/media file has changed
- **THEN** no enrichment re-plan is triggered and the doc tree is not walked

#### Scenario: Doc change triggers a re-plan
- **WHEN** a doc/media file is added, changed, or removed
- **THEN** an enrichment re-plan is triggered

#### Scenario: Initial plan runs once
- **WHEN** the daemon starts and completes its initial build or fast-load
- **THEN** enrichment is planned exactly once to populate the pending/stale counts

### Requirement: Plan offers candidate code links per document
The enrichment chunk plan SHALL compute, for each document input, candidate links to real code
nodes that the document mentions, derived deterministically from the code graph's node labels. Each
candidate SHALL carry the real code-node id and its label. The plan SHALL surface these as a
`candidate_links` array on each document input in `plan.json`. Candidates SHALL be node ids and
labels only; the plan SHALL NOT assign an edge relation (the authoring host chooses the relation).

#### Scenario: A document naming a code symbol gets that node as a candidate
- **WHEN** a planned document's text mentions a compound code symbol that exists as a graph node
  (e.g. `classify_cached_file`)
- **THEN** that document's `candidate_links` includes the symbol's real code-node id and label

#### Scenario: Candidates are ids and labels, not relations
- **WHEN** the plan emits candidate links for a document
- **THEN** each candidate contains a code-node id and label and no edge relation

### Requirement: Candidate matching is high-precision
Candidate matching SHALL accept compound identifiers (snake_case or internal CamelCase) and
capitalized type names, and SHALL reject bare lowercase words that merely coincide with a symbol
name. When a symbol name is shared by multiple nodes, rarer names SHALL rank ahead of common ones,
and the number of candidates per document SHALL be bounded.

#### Scenario: Bare-word collisions are excluded
- **WHEN** a document contains a bare lowercase word (e.g. `cache`) that happens to match a code
  symbol name, and no compound form
- **THEN** that word does not by itself produce a candidate link

#### Scenario: Candidate count is bounded
- **WHEN** a document mentions more matching symbols than the per-document cap
- **THEN** only the top-ranked candidates up to the cap are emitted

#### Scenario: Non-text inputs yield no candidates
- **WHEN** an input is media (no readable text)
- **THEN** it has no candidate links

### Requirement: Candidate links degrade gracefully without a code graph
Candidate computation SHALL require a code graph to match against. When no persisted code graph is
available to the planner, the plan SHALL emit empty `candidate_links` and otherwise produce the
same chunk plan it does today. The feature SHALL be additive: a fragment that ignores
`candidate_links` SHALL remain valid and merge unchanged.

#### Scenario: No code graph present
- **WHEN** the planner has no persisted code graph to load
- **THEN** every input's `candidate_links` is empty and the rest of the plan is unchanged

#### Scenario: Candidate links are advisory
- **WHEN** a host authors a fragment that does not use the offered candidate links
- **THEN** the fragment still validates and merges exactly as before

### Requirement: New fragment names never collide with existing drops
When the chunk plan assigns filenames for new fragments, each name SHALL be strictly greater than
the highest existing fragment index in the drop directory, so a new enrichment pass never reuses an
occupied fragment number and never overwrites a fragment authored by an earlier pass. This SHALL
hold even when existing fragment numbers are non-contiguous (gaps from a partial earlier pass). With
no existing fragments, numbering SHALL start at zero.

#### Scenario: Non-contiguous existing fragments do not cause a collision
- **WHEN** the drop directory contains `chunk_00.json` and `chunk_05.json` (a gap), and a plan emits
  at least one new chunk
- **THEN** the new fragment's filename has an index greater than 5 and matches no existing file

#### Scenario: Cold directory starts at zero
- **WHEN** the drop directory contains no fragments and a plan emits chunks
- **THEN** the first new fragment is `chunk_00.json`

#### Scenario: Accumulation across passes
- **WHEN** successive plans each emit new fragments into the same drop directory
- **THEN** each pass's fragment names are strictly past all prior fragments, so the directory
  accumulates and no earlier fragment is overwritten

### Requirement: Host skills are installable from the binary
The CLI SHALL provide `cgraph skills <install|status|uninstall>`: `install` symlinks the
canonical `integrations/skills/cgraph` and `integrations/skills/cgraph-enrich` directories
into the host skill paths (`~/.claude/skills/` and the shared agents skills path) without
copying content; `status` reports each expected link's presence and target; `uninstall`
removes only links that point into this repository. Install SHALL be idempotent.

#### Scenario: Install then discover in another repo
- **WHEN** `cgraph skills install` runs and a new host session starts in a supervisor-tracked repository
- **THEN** `cgraph skills status` shows both links resolving into this repository and the session lists the `cgraph` and `cgraph-enrich` skills

#### Scenario: Re-install is a no-op
- **WHEN** `cgraph skills install` runs while correct links already exist
- **THEN** the links are unchanged and the command succeeds

### Requirement: Pending enrichment is drained autonomously and bounded
A scheduled host-side drainer SHALL exist that, per tracked repository, reads
`status.enrichment_pending` and exits without any model dispatch when it is zero; when
positive it SHALL run the enrich loop headlessly with a per-run chunk cap, leaving
remaining chunks for subsequent runs. Model selection and spend SHALL remain host-owned.

#### Scenario: Pending repo drains to zero
- **WHEN** the drainer (or a manual `/cgraph-enrich` invocation) runs on a repository whose daemon reports `enrichment_pending > 0`
- **THEN** across one or more runs `status` reaches `enrichment_pending: 0` and `enrichment_failed: 0`, and the semantic block reports non-zero `doc_nodes` and `doc_code_edges`

#### Scenario: Steady state costs nothing
- **WHEN** the drainer fires on a repository with `enrichment_pending: 0`
- **THEN** it exits after the status check with no host model invocation

#### Scenario: Stale doc re-enters the next run
- **WHEN** a previously enriched document changes on disk
- **THEN** the next plan lists it as stale and only that input is re-enriched

#### Scenario: Re-run is idempotent
- **WHEN** the drainer runs again with no input changes
- **THEN** the plan reports all inputs as cache hits and no host model work is performed

### Requirement: Fragment edges must resolve to known nodes
Semantic ingest SHALL reject a fragment atomically — with the graph unchanged and the rejection
counted as a failed fragment — when any edge endpoint resolves against neither the fragment's
own nodes (keyed exactly as merge keys them) nor the current graph snapshot. This check wraps
the semantic-ingest path only; the deterministic pipeline's merge semantics are unchanged.

#### Scenario: Dangling edge endpoint
- **WHEN** a schema-valid fragment carries an edge whose target id exists in neither the
  fragment nor the graph
- **THEN** the fragment is rejected with an error naming the unknown endpoint, no node or edge
  from the fragment enters the graph, and `enrichment_failed` is incremented

#### Scenario: Edges into the existing graph still merge
- **WHEN** a fragment's edge references a node already present in the graph snapshot
- **THEN** the fragment merges normally

### Requirement: Fragment parsing is total over any JSON shape
Parsing a semantic fragment SHALL NOT throw for any syntactically valid JSON input. Every
optional field, including each component of `source_location`, SHALL be read only when it holds
the expected type and SHALL degrade to a default otherwise, so a type-confused field can never
escape validation as an exception. A structurally valid fragment carrying garbage in an optional
field SHALL be accepted; only a structural violation (missing or mistyped `nodes`/`edges`,
missing required node/edge fields) SHALL reject it.

#### Scenario: A string-typed source_location line does not crash
- **GIVEN** a fragment whose node has `"source_location": {"start_line": "9"}` (a string where a number is expected)
- **WHEN** the fragment is validated
- **THEN** parsing returns without throwing and the fragment is accepted
- **AND** a resident daemon that ingests the dropped fragment stays alive and responsive

#### Scenario: Unreadable location components degrade, never fabricate a site
- **GIVEN** a `source_location` whose components are string, null, or bool
- **WHEN** the fragment is parsed
- **THEN** each unreadable component reads as 0, and a location object with no readable numeric component is treated as absent rather than a line-0/column-0 site

#### Scenario: Out-of-range location numbers do not wrap
- **GIVEN** a `source_location` with a negative or above-uint32 component
- **WHEN** the fragment is parsed
- **THEN** that component degrades to 0 rather than wrapping to a large or truncated line number

### Requirement: Enrichment identity uses the planned kind, not the host-written kind
On ingest, the system SHALL set the kind of every fragment node whose `source_file` matches a
planned enrichment source to the kind deterministically classified from that source's path
(`document` or `media`), overriding any host-written kind, so cross-file identity protection does
not depend on the optional `type`/`kind` field. The system SHALL preserve a node the host
explicitly marked `concept`, and SHALL leave unchanged any node without a `source_file` or whose
`source_file` is not a planned source.

#### Scenario: A document authored without a kind is protected
- **GIVEN** a fragment node whose `source_file` matches a planned document source and whose `type` is omitted or aliased (e.g. `doc`)
- **WHEN** the fragment is ingested
- **THEN** the node's kind is `document`

#### Scenario: A media source node is stamped media
- **GIVEN** a fragment node whose `source_file` matches a planned media source
- **WHEN** the fragment is ingested
- **THEN** the node's kind is `media`

#### Scenario: An explicit concept is preserved
- **GIVEN** a `concept` node carrying a `source_file` that matches a planned source, beside a kind-less document node with the same `source_file`
- **WHEN** the fragment is ingested
- **THEN** the document node's kind becomes `document` and the concept node stays `concept`

#### Scenario: Two under-specified cross-file documents both survive dedup
- **GIVEN** two documents from different files with similar labels and no host `type`
- **WHEN** the fragments are ingested and semantic dedup runs
- **THEN** both nodes are stamped `document` and both survive

