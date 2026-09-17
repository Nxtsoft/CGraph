# Feature: contract documents as endpoints and schemas, with drift (CGR-13, slice 3)

## Why

Slices 1 and 2 (#97, #99) read the contract out of code: routes as `endpoint` nodes, client
calls as `CONSUMES` edges. Many services also *state* their contract in a document, and a
client is typed against it. The field-test pair has no `.proto`, `.graphql` or OpenAPI file at
all; what it has is `lib/generated/api-types.d.ts`, the openapi-typescript rendering of
turing-api's OpenAPI document (455 paths, generated 2026-08-19), which the webapp's
openapi-fetch client imports as `paths`. Today the TypeScript extractor turns that file into a
`paths` interface with 455 `field` nodes named `"/api/v1/..."` and learns nothing from it. The
document says what the API promised; the api graph says what it serves; the gap between them is
contract drift, which the plan named as trace-mcp's `get_contract_drift` and CGraph's missing
piece.

## What Changes

- **Contract document extractors** (`contract_schemas.cpp`, new; registered as non-grammar
  languages). OpenAPI JSON (`*openapi*.json`, `*swagger*.json`; OpenAPI 3 and Swagger 2): every
  `paths.<path>.<method>` is an `endpoint` node with the canonical id, the document's spelling
  as label and `path`, `documented: true` and the `operationId`; every component schema is a
  `schema` node with a `field` per property (`type_text` from `type`/`format`/`$ref`, `optional`
  from `required`); `RESPONDS_WITH` / `ACCEPTS` follow 2xx response and request-body `$ref`s,
  `references` follow property `$ref`s, `inherits` follows `allOf`. Protocol Buffers: every
  `message` (nested as `Outer.Inner`) and `enum` is a `schema` with fields (`repeated`, `map<K,V>`,
  `oneof` members, options skipped); every `service` is a `type` and each `rpc` an endpoint at
  the gRPC path `POST /<package>.<Service>/<Method>` with `ACCEPTS` / `RESPONDS_WITH`; field
  types name messages through `references`. GraphQL SDL: every type, interface, input, enum,
  union and scalar is a `schema` with fields (`type_text` as written, `optional` from `!`); each
  field of a root operation type (`Query`/`Mutation`/`Subscription` or the `schema {}` block's)
  is an endpoint `QUERY <field>` / `MUTATION <field>` / `SUBSCRIPTION <field>` with
  `RESPONDS_WITH` its return type and `ACCEPTS` its input arguments; `implements`, union
  members and `extend type` are followed; descriptions and directives are skipped. Every
  document emits its `file` node, `contains` to its nodes, and `defines` from a service or root
  type to its endpoints.
- **openapi-typescript** (`javascript_extractor.cpp`). A TypeScript file declaring both
  `export interface paths` and `export interface operations` is the generated form: `paths`
  members become documented endpoints (a method typed `never` is absent), `components.schemas`
  members become `schema` nodes with fields, and `operations` request bodies and 2xx responses
  naming `components["schemas"]["X"]` become `ACCEPTS` / `RESPONDS_WITH`. The `paths`,
  `operations` and `components` interfaces yield no `field` nodes.
- **One node per contract.** A documented endpoint has the same canonical id as a served or
  consumed one, so `resolve_contracts` attaches `handled_by` and `CONSUMES` to the document's
  node instead of minting; `route_resolution.endpoints_documented` counts them.
- **Drift in `seam discover`.** An endpoint carrying `documented` is a third role: `DOCUMENTED_IN`
  (endpoint → the document's `code-ref`), kept in the fragment even when neither served nor
  consumed, and a served or documented copy's spelling wins over a consumer placeholder. The log
  gains `documents N` per service and a `drift:` line: documented but served by no service among
  the graphs, served but in no document, only documented.
- **`report types` sees schemas** (`is_type_kind` gains `schema`): an API schema and its
  hand-written TypeScript mirror surface as a duplicate or identical shape. `schema` nodes never
  enter fuzzy dedup (`NotebookResponse` beside `NotebooksResponse`).
- Detection: `.proto`, `.graphql` / `.gql` / `.graphqls`, and `.json` named with `openapi` or
  `swagger`. YAML documents are not detected (no YAML parser in the engine).

### Non-goals
- OpenAPI in YAML (the common form): needs a parser dependency; deferred until a field repo has one.
- Sniffing every `.json` for an `openapi` key: lockfiles and configs would be read for nothing.
- Cross-file proto imports and GraphQL schema stitching: references resolve within one document.
- GraphQL operation documents in client code (`gql\`query { … }\``) as consumers of `QUERY` endpoints.
- Response/request shapes inlined in a document (`schemas: never`): no schema node exists to link.

## Impact

- **Touches:** `contract_schemas.hpp/.cpp` (new), `detect.hpp/.cpp`, `non_grammar_extractors.cpp`,
  `javascript_extractor.cpp`, `contracts.cpp`, `operation_stats.hpp/.cpp`, `seam.cpp`,
  `dedup.cpp`, `report.cpp`, CMake lists, `contract_schemas_test.cpp` (new),
  `javascript_extractor_test.cpp`, `contracts_test.cpp`, `seam_test.cpp`, `dedup_test.cpp`,
  `report_test.cpp`, `detect_test.cpp`, docs.
- **Graph shape.** A repository with none of these files is unchanged. One with an
  openapi-typescript file loses its per-path `field` nodes and gains `endpoint` nodes; one with
  documents gains `file`, `endpoint`, `schema`, `field` and `type` nodes for them.
- **Measured** on the field-test pair (tasks.md 4.x).

## Capabilities

### Modified Capabilities
- `contract-discovery` — contract documents declare endpoints and schemas; documented endpoints share the node a route serves or a client consumes.
- `cross-service-seam` — `seam discover` carries the documented role and reports drift.
- `deterministic-graph-pipeline` — detection of the three document kinds; `route_resolution.endpoints_documented`; `schema` exempt from fuzzy dedup and a type owner in `report types`.
