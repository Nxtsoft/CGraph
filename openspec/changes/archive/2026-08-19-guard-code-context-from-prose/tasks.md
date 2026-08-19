# Tasks

## 1. Experiment first (research/enrichment-seeding, production enrichment flow)

- [x] 1.1 Produced the overlay via the real host loop: enrich-plan (33 chunks / 260 docs),
      6 parallel host agents authoring fragments, enrich-ingest (33 merged / 0 rejected
      after fixing the local-concept contract violations), plan drained to 0.
- [x] 1.2 Measured the as-is regression: −2.8..−6.8 recall points at every budget on the
      enriched graph. Isolated all four mechanisms; idf pollution was the dominant term.
- [x] 1.3 Measured the firewall: enriched vs unenriched recall bit-identical. Measured
      bridge seeding in three orderings: never a win; parked with evidence.

## 2. Implement (red first)

- [x] 2.1 daemon_ops_test prose-firewall block: doc label containing the full query must not
      become focus (red on the shipped engine: exit 99) and enrichment never packs; the
      named code node resolves instead. Green with the guards.
- [x] 2.2 The four `is_enrichment_node_id` guards: lexical scoring+df, substring focal,
      context candidates, gather frontier.

## 3. Prove it

- [x] 3.1 e2e gate: 6000 pin re-transcribed at the final tree (0.3925); other pins hold.
- [x] 3.2 SKILL.md contract note (local concept definitions).
- [x] 3.3 Full `ctest --preset default` green (66/66); sanitizers via CI (local ASan hang
      recorded in add-rust-import-resolution).
