## MODIFIED Requirements

### Requirement: Lexical multi-seed focal resolution for free-text queries
When resolving a focal node for a `context` or `query` request, the engine SHALL first attempt
exact (id, label, bare symbol) and substring matching, and SHALL fall back to lexical term-overlap
matching only when those produce no match. The lexical fallback SHALL compare stemmed terms — each
query and label subtoken reduced by a one-shot strip of a fixed suffix list, with the stem keeping
at least four characters — so a query inflection matches the identifier it names, and SHALL rank
nodes by inverse-document-frequency-weighted overlap of those stems — each stem weighted by
`log(1 + N/(1+df))`, df being the stem's document frequency over node-label subtokens in the
snapshot — so a rare identifier outranks a ubiquitous word. The focal SHALL resolve from the top
match, deterministically (ties broken by centrality then id). For a `context` request, the gather
SHALL be seeded from the top-N ranked matches and union their neighborhoods; N is pinned to the
measured optimum for the ranking in force (three — wider pools measurably dilute packing). When
the query shares no stemmed term with any node label (and, for a prose query, none with any
indexed code body), the focal SHALL remain unresolved — the response returns suggestions and the
call is recorded as a zero hit. Stemming applies to seed ranking only: the substring pre-pass, the
adaptive gather gate, and the knapsack value keep exact-match semantics.

The lexical fallback SHALL route by query shape. A query without whitespace is identifier-like
and SHALL rank by the label overlap above alone. A query containing whitespace is prose (issue
text, a review comment, a stack trace) and SHALL additionally rank every indexed code symbol by
Okapi BM25 (k1 = 1.2, b = 0.75) over the stemmed lexical terms of its source span, and SHALL fuse
the label ranking with the top 100 body-ranked symbols by reciprocal rank (k = 60), ties broken
by label rank then id; seeds and the focal are taken from the fused order. The body index SHALL be
runtime-only — never exported to `graph.json`, never persisted — and SHALL be attached to every
snapshot by `publish_graph_snapshot` on the single-writer path. A file SHALL be indexed only when
its bytes on disk hash to the snapshot's recorded source hash; a snapshot without source hashes
has an empty body index and ranks by label alone. On each publish a file's tokenized entry SHALL
be reused when its source hash and indexed node spans are unchanged, so an incremental update
re-reads only the files it re-extracted. Memory and enrichment nodes SHALL never be indexed.
Each snapshot SHALL carry a code generation that a copy keeps and a newly built snapshot does not;
publishing a snapshot whose index was verified for its own code generation (an overlay, memory or
enrichment mutation of a published copy) SHALL keep that index without any per-node work. A file
whose read or tokenization fails SHALL be skipped, never terminate the daemon, and be listed under
`status` `body_index.failures` (with `files_failed`), alongside the indexed `files` and
`documents`. For a structural phrase (`callers of X` and the like) whose operand does not resolve,
the body ranking SHALL score only the operand, never the relation verb. In the `query` op's search
route, `total` SHALL keep counting symbols matched by name (substring or label terms); body-ranked
candidates with no name match SHALL be ranked into `nodes` but counted separately in
`body_candidates` (present when non-zero), so `returned` may exceed `total`; the call is a zero
hit, with `suggestions`, only when both are zero.

#### Scenario: Natural-language query resolves via lexical overlap
- **WHEN** a `context` request supplies a free-text query that is not an exact match or a substring
  of any node id or label, but shares lexical terms with one or more symbols
- **THEN** a focal node is resolved from the highest-weighted match and a non-empty context bundle
  is returned, instead of the empty `focus:null` response

#### Scenario: An inflected query term reaches the identifier it names
- **GIVEN** a query term and a label subtoken that differ only by a common suffix
  (e.g. "packs" / "packing")
- **WHEN** the lexical fallback runs
- **THEN** they match through their shared stem and the node is rankable, where exact matching
  would have found nothing

#### Scenario: A rare query term outranks a ubiquitous one
- **GIVEN** a query with two terms, one appearing in a single node label and one appearing in many,
  ranked on the label path (an identifier-like query, or a snapshot with no body index)
- **WHEN** the lexical fallback ranks the matches
- **THEN** the node matching the rare term ranks above the nodes matching only the common term and
  becomes the focus

#### Scenario: Exact lookups are unchanged
- **WHEN** a request supplies an exact node id, an exact label, or a bare symbol name that resolves
  by the existing exact/substring path
- **THEN** that node is resolved exactly as before and the lexical fallback does not run

#### Scenario: Off-topic query stays an honest zero hit
- **WHEN** a query shares no stemmed term with any node label, nor (for a prose query) with any
  indexed code body
- **THEN** the focal stays unresolved, the response returns `suggestions`, and the call is recorded
  as a context/query zero hit

#### Scenario: Multi-seed gather unions several ego graphs
- **WHEN** a free-text `context` query overlaps several symbols and resolves via the lexical fallback
- **THEN** the gathered candidate set is the union of the neighborhoods of the top-N ranked seeds,
  deduplicated by shallowest reach, and a relevant node reachable only from a lower-ranked seed is
  included

#### Scenario: Resolution is deterministic
- **WHEN** the same free-text query is resolved twice against the same snapshot
- **THEN** the same focal node (and seed set) is selected, with equal-weight ties broken by
  centrality then id

#### Scenario: A prose query resolves through code bodies
- **GIVEN** a snapshot whose source hashes match the files on disk, and a symbol whose label shares
  no word with the query but whose source span does
- **WHEN** a `context` or `query` request supplies that query as prose
- **THEN** the symbol is ranked from the fused label and body rankings and becomes the focus, where
  label matching alone would have resolved nothing

#### Scenario: An identifier-like query keeps the label path
- **WHEN** the query contains no whitespace and is not an exact or substring match
- **THEN** it is ranked by label overlap alone, even when the body index holds that identifier

#### Scenario: The body index follows edits through the single-writer path
- **WHEN** a file is re-extracted and the new snapshot is published
- **THEN** a prose query matching the new span text resolves to the edited symbol, the replaced
  text no longer matches, and unchanged files are not read again

#### Scenario: Bytes that differ from the extracted ones are never indexed
- **WHEN** a file's bytes on disk no longer hash to the snapshot's recorded source hash
- **THEN** that file contributes no body documents until a snapshot that extracted those bytes is
  published

#### Scenario: On a prose query, agreement between the rankings decides the focus
- **GIVEN** a snapshot with a body index and a prose query where node A is the label ranking's
  second match and the body ranking's first, and node D is the label ranking's first match with no
  indexed body
- **WHEN** a `context` request resolves the query
- **THEN** A is the focus (ranked by both) and D, the rare-term label match, is still one of the
  seeds whose neighborhood is gathered

#### Scenario: Body-only search results are counted apart from name matches
- **WHEN** a prose `query` matches no symbol by name but some symbols by body
- **THEN** `total` is 0, `body_candidates` counts the body-ranked symbols returned in `nodes`, no
  `suggestions` are attached, and the call is not recorded as a zero hit

#### Scenario: An unresolved structural phrase scores only its operand on bodies
- **WHEN** `callers of X` falls through to search because `X` resolves no symbol
- **THEN** the body ranking scores `X` alone, so code whose body merely says "call" is not pulled in

#### Scenario: Publishing an unchanged copy does no index work
- **WHEN** a published snapshot is copied, given only memory, overlay or enrichment nodes, and
  published again
- **THEN** it carries the very same body index, and no node or file is visited
