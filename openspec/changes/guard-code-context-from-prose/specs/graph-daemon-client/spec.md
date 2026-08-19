## ADDED Requirements

### Requirement: Code context is prose-free
The `context` op SHALL resolve its focus from, and assemble its bundle exclusively from,
non-enrichment nodes. Enrichment nodes (the host-authored `doc:`, `concept:`, `media:`, and
`topic:` id namespaces) never participate in code focal resolution or code context assembly:
they are excluded
from substring focal matching, from lexical seed scoring and its document frequencies, from
`context` candidate collection, and from gather frontier expansion. Prose remains reachable
through `query` search results (ranked below structural results), `explain`, `path`, and
`impact`. Adding a semantic overlay to a graph SHALL NOT change any `context` response for a
code query.

#### Scenario: A dominating document label never becomes the code focus
- **GIVEN** an enriched graph where a document's label contains the full free-text query and
  lexically dominates every code label
- **WHEN** a `context` request runs with that query
- **THEN** the focus is the code node the query names, not the document

#### Scenario: Enrichment never enters a code context bundle
- **WHEN** a `context` request runs on an enriched graph with a generous budget
- **THEN** no `included` entry carries an enrichment-namespace id

#### Scenario: Enrichment is recall-neutral for code queries
- **WHEN** the end-to-end retrieval gate runs against the same graph with and without a
  semantic overlay
- **THEN** measured grade-2 recall is identical at every budget

#### Scenario: Prose labels do not vote on term rarity
- **GIVEN** a query term that is rare among code labels but common in document labels
- **WHEN** the lexical fallback ranks seeds
- **THEN** the term's weight reflects its code-label document frequency only
