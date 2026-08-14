## ADDED Requirements

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
