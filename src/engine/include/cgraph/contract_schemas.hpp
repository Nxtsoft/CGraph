#pragma once

#include "cgraph/extractor.hpp"
#include "cgraph/language_config.hpp"

// Contract documents (CGR-13, slice 3): the files that describe a service's wire
// contract outright, read without a tree-sitter grammar.
//
//   OpenAPI documents (`openapi*.json`, `swagger*.json`): every `paths.<path>.<method>`
//   becomes an `endpoint` node (canonical id, `documented: true`) and every
//   `components.schemas.<Name>` (Swagger 2: `definitions`) a `schema` node with a
//   `field` per property; `RESPONDS_WITH` / `ACCEPTS` edges follow the 2xx response
//   and request-body `$ref`s, `references` / `inherits` follow property and `allOf`
//   `$ref`s.
//
//   Protocol Buffers (`.proto`): every `message` and `enum` is a `schema` node with
//   a `field` per member (nested messages as `Outer.Inner`); every `service` is a
//   `type` node and each `rpc` an `endpoint` `POST /<package>.<Service>/<Method>`
//   (the gRPC path) with `ACCEPTS` and `RESPONDS_WITH` to its messages.
//
//   GraphQL SDL (`.graphql`, `.gql`, `.graphqls`): every type, interface, input,
//   enum, union and scalar is a `schema` node with a `field` per member; each field
//   of a root operation type (`Query`, `Mutation`, `Subscription`, or the `schema {}`
//   block's) is an `endpoint` `QUERY <field>` / `MUTATION <field>` /
//   `SUBSCRIPTION <field>` with `RESPONDS_WITH` its return type and `ACCEPTS` its
//   input arguments; `implements` follows interface clauses.
//
// Each extractor emits the document's `file` node, `contains` from it to every
// node, and `defines` from a service or root type to its endpoints. Schema ids
// are file-scoped (`make_id(<file>:schema:<Name>)`), like every declared symbol;
// endpoint ids are the repo-free canonical ids of contracts.hpp, so a documented
// endpoint is the same node as the one a route serves or a client consumes.
// The openapi-typescript generated form (`export interface paths`) is handled by
// the TypeScript extractor, since it is TypeScript.
namespace cgraph {

[[nodiscard]] ExtractionResult extract_openapi_document(const ExtractionContext& context);
[[nodiscard]] ExtractionResult extract_protobuf(const ExtractionContext& context);
[[nodiscard]] ExtractionResult extract_graphql_sdl(const ExtractionContext& context);

}  // namespace cgraph
