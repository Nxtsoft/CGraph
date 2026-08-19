## Why

Enriching a project today makes its code retrieval WORSE. Measured on this repo's graph with
a full production-flow semantic overlay (33 host-authored fragments, 256 documents, 69
concepts, 1096 prose->code edges — produced by the real enrich-plan/author/enrich-ingest
loop, research/enrichment-seeding): the shipped engine loses 2.8–6.8 end-to-end grade-2
recall points at every budget versus the same graph without the overlay
(0.2487/0.3629/0.3927/0.4425 -> 0.2209/0.2949/0.3529/0.3977 at 2000/4000/6000/8000).

Four mechanisms, each isolated by measurement: document labels carry exactly a query's
natural-language vocabulary, so prose wins focal resolution (substring and lexical) over the
code the query is about; packed prose snippets spend budget that cannot answer a code query;
each document's ~40-edge ego graph floods the candidate pool; and — the dominant term —
hundreds of prose labels deflate the idf of the query's own words, silently reordering CODE
seed ranking even when no prose node ranks.

The observable contract tests verify: on an enriched graph, prose never resolves a code
focus and never appears in a code context bundle, and enriched-vs-unenriched recall is
identical.

## What Changes

- Four guards in `daemon_ops.cpp`, all reusing `is_enrichment_node_id` (the doc:/concept:/
  media:/topic: namespace rule from rank-structural-over-enrichment): enrichment nodes are
  excluded from (1) lexical seed scoring AND the idf document frequencies, (2) the substring
  focal pre-pass, (3) code-context candidates, and (4) gather frontier expansion.
- Prose stays fully reachable where it belongs: `query` search results (ranked below
  structural, unchanged), `explain`, `path`, and `impact` are untouched.
- A regression block in daemon_ops_test: a document whose label contains the full query and
  lexically dominates it must not become the focus and must not be packed; the code node the
  query names resolves instead.
- cgraph-enrich SKILL.md gains the fragment-contract note surfaced by the production run:
  every referenced concept id must be defined locally in each fragment (12/33 first-pass
  fragments were rejected for bare cross-fragment references).
- Measured with the firewall: enriched and unenriched recall are bit-identical at every
  budget (perfect neutrality). Bridge-style seeding (translating a winning doc match into
  its linked code) was implemented and measured in three orderings and never changed a
  single row's outcome or hurt — parked in research/enrichment-seeding, not shipped.
- Non-goals: changing `query`-op enrichment ranking (#32 owns that); an enrichment-aware
  recall gate (the committed fixture stays code-only); any enrichment-as-recall-lever
  mechanism.

## Impact

- `src/engine/daemon_ops.cpp` — the four guards.
- `tests/smoke/daemon_ops_test.cpp` — prose-firewall regression block.
- `tests/smoke/retrieval_quality_test.cpp` — 6000 pin re-transcribed at this tree (0.3925).
- `integrations/skills/cgraph-enrich/SKILL.md` — local-concept-definition contract note.
- Capability: `graph-daemon-client`. Exports, parity surfaces, query-op ranking untouched.
