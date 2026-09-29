## MODIFIED Requirements

### Requirement: Shadow code-refs collapse onto real service nodes
When fusing, every node id from a supplied service graph SHALL be scoped as `<service>::<id>`, except `endpoint:`, `service:` and `schema:` ids, which SHALL stay shared so a provider and its consumers meet at one node. The seam's `code-ref` shadow nodes SHALL be dropped, and every seam edge that names a shadow SHALL be rewritten to the scoped id of the service the shadow records, so the `CONSUMED_AT` and `MIRRORED_BY` edges attach to the real service nodes (with their full neighborhood and real source location) and the contract is drillable into the surrounding code. Two services that own a node with the same project-relative id SHALL fuse into two nodes, each keeping only its own service's edges.

#### Scenario: Contract edge binds to the real node, not a shadow
- **WHEN** a seam fragment contains a `code-ref` shadow for a consumer call site and the consumer's service graph is supplied to fuse
- **THEN** the fused graph contains no `code-ref` node, and the `CONSUMED_AT` edge targets the consumer service's scoped node for that call site

#### Scenario: Two services with the same relative file stay apart
- **GIVEN** services `api` and `agents` whose graphs both contain `src_db_client_ts`, each imported by its own file, and both consuming `endpoint:GET /x`
- **WHEN** they are fused
- **THEN** the fused graph holds `api::src_db_client_ts` and `agents::src_db_client_ts` with their own source files, no import edge crosses between them, and both importers consume the one `endpoint:GET /x`
