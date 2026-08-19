## MODIFIED Requirements

### Requirement: Lexical multi-seed focal resolution for free-text queries
When resolving a focal node for a `context` or `query` request, the engine SHALL first attempt
exact (id, label, bare symbol) and substring matching, and SHALL fall back to lexical term-overlap
matching only when those produce no match. The lexical fallback SHALL rank nodes by
inverse-document-frequency-weighted overlap of the query's lexical terms with the node label —
each term weighted by `log(1 + N/(1+df))`, df being the term's document frequency over node-label
subtokens in the snapshot — so a rare identifier outranks a ubiquitous word, and SHALL resolve the
focal from the top match, deterministically (ties broken by centrality then id). For a `context`
request, the gather SHALL be seeded from the top-N idf-ranked matches and union their
neighborhoods; N is pinned to the measured optimum for the ranking in force (three, with idf —
wider pools measurably dilute packing). When the query shares no lexical term with any node label,
the focal SHALL remain unresolved — the response returns suggestions and the call is recorded as a
zero hit.

#### Scenario: Natural-language query resolves via lexical overlap
- **WHEN** a `context` request supplies a free-text query that is not an exact match or a substring
  of any node id or label, but shares lexical terms with one or more symbols
- **THEN** a focal node is resolved from the highest-weighted match and a non-empty context bundle
  is returned, instead of the empty `focus:null` response

#### Scenario: A rare query term outranks a ubiquitous one
- **GIVEN** a query with two terms, one appearing in a single node label and one appearing in many
- **WHEN** the lexical fallback ranks the matches
- **THEN** the node matching the rare term ranks above the nodes matching only the common term and
  becomes the focus

#### Scenario: Exact lookups are unchanged
- **WHEN** a request supplies an exact node id, an exact label, or a bare symbol name that resolves
  by the existing exact/substring path
- **THEN** that node is resolved exactly as before and the lexical fallback does not run

#### Scenario: Off-topic query stays an honest zero hit
- **WHEN** a query shares no lexical term with any node label
- **THEN** the focal stays unresolved, the response returns `suggestions`, and the call is recorded
  as a context/query zero hit

#### Scenario: Multi-seed gather unions several ego graphs
- **WHEN** a free-text `context` query overlaps several symbols and resolves via the lexical fallback
- **THEN** the gathered candidate set is the union of the neighborhoods of the top-N idf-ranked
  seeds, deduplicated by shallowest reach, and a relevant node reachable only from a lower-ranked
  seed is included

#### Scenario: Resolution is deterministic
- **WHEN** the same free-text query is resolved twice against the same snapshot
- **THEN** the same focal node (and seed set) is selected, with equal-weight ties broken by
  centrality then id
