# Tasks

## 1. Document extractors
- [x] 1.1 `contract_schemas_test.cpp`: an OpenAPI 3 JSON fixture (four endpoints across two paths,
      `parameters` and `x-` keys not methods, canonical id with `{}` and the document's `{id}`
      label, operation id, line of the path; four schemas with fields, `type_text` incl. array
      and format, `optional` from `required`; `RESPONDS_WITH` through an array `items` `$ref`,
      `ACCEPTS`, `references`, `inherits` via `allOf`, `contains`; a non-document refused with a
      warning). A proto3 fixture (messages, nested `Notebook.Owner`, `oneof` members, `map<K,V>`,
      `repeated`, options and `reserved` skipped, an enum's values, a service with two rpcs at
      the gRPC path incl. `stream` and an options body, `ACCEPTS` / `RESPONDS_WITH` /
      `references`, an imported type unresolved). A GraphQL fixture (type with `implements A & B`
      and a directive, interface, enum, input with defaults, union, scalar, `extend type`,
      descriptions and comments, `schema {}` block; root fields as `QUERY` / `MUTATION`
      endpoints with `RESPONDS_WITH` unwrapping `[Note!]!` and `ACCEPTS` inputs; no fields on
      root types; built-ins unreferenced).
- [x] 1.2 `Emitter`, `LineIndex`, the tokenizer, `extract_openapi_document`, `extract_protobuf`,
      `extract_graphql_sdl`; detection and names for `OpenApi`, `Protobuf`, `GraphQL`;
      non-grammar dispatch; `detect_test.cpp` covers the names and that `openapi.yaml` and
      `package.json` stay undetected.

## 2. openapi-typescript
- [x] 2.1 `javascript_extractor_test.cpp`: four documented endpoints from `paths` (`never` methods
      skipped, `{id}` canonicalised, operation id, line), no `field` per path, three component
      schemas with fields and `optional`, `RESPONDS_WITH` through `components["schemas"]["X"][]`,
      `ACCEPTS`, `references`, `contains` and `defines`.
- [x] 2.2 `is_openapi_typescript`, `interface_named`, `member_named`, `components_schema_name`,
      `operation_schema_refs`, `openapi_typescript_paths`, `openapi_typescript_components`,
      wired at the top of `ts_member_handler`.

## 3. One node, drift, types view
- [x] 3.1 `contracts_test.cpp::test_documented_endpoint_joins`: a served documented endpoint keeps
      its document anchor and gains `handled_by`; a consumed one gains `CONSUMES` without
      `served: false`; the tally.
- [x] 3.2 `seam_test.cpp`: a third, document-only graph → `DOCUMENTED_IN` to the document shadow,
      a documented-only endpoint kept without `served`, the `documents 2` service line, the
      exact `drift:` line, fuse with the third snapshot, byte-stable regeneration.
- [x] 3.3 `report_test.cpp::test_types_view_schemas`: a schema and its TypeScript mirror are a
      duplicate row at Jaccard 1.0; a renamed mirror is an identical shape.
      `dedup_test.cpp`: three sibling schemas survive.
- [x] 3.4 `resolve_contracts` counts `endpoints_documented`; `discover_seam` documented role and
      drift log; `is_type_kind` gains `schema`; dedup exempts `schema`.

## 4. Docs and verification
- [x] 4.1 README, skill, host contract, CLAUDE.md.
- [x] 4.2 `ctest --test-dir build/default`: 80/80 pass. GCC 14 `-fsyntax-only` on mars passes for
      every changed source and test.
- [x] 4.3 Field test, turing-webapp at `origin/main` (01e03897, 10-10.7 s): 613 documented
      endpoints from `lib/generated/api-types.d.ts` (455 paths; GET 235, POST 196, DELETE 79,
      PATCH 69, PUT 34), 388 of them consumed by the webapp, 0 `field` nodes named like paths
      (455 before); `endpoints_external` falls from 82 to 35 once documented ids exist.
- [x] 4.4 `seam discover` over turing-webapp + turing-api (`origin/dev` 26a691c): matched 410 as
      before; `drift: 1 documented but served by no service here, 39 served but in no document;
      0 only documented`. The one is `GET /api/v1/product-categories/member-access`, removed since
      the August document; of the 39, 18 are the webapp's own Next.js routes and the rest api
      routes added since (`DELETE /api/v1/category-variables/positions/{}`, ...). The first run
      said 82 / 120: the document spells `.get('/')` under a prefix with a trailing slash, so
      canonical ids now drop it (pinned in `test_canonical_ids`).
- [x] 4.5 CGraph's own graph unchanged (1558 / 3538).
- [ ] 4.6 CI green; merge.
