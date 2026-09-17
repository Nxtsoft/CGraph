#include "cgraph/contract_schemas.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::cerr << "contract_schemas_test: " << message << '\n';
  return 1;
}

const cgraph::Node* node_labelled(const cgraph::Fragment& fragment, std::string_view kind, std::string_view label) {
  for (const auto& node : fragment.nodes) {
    if (node.kind == kind && node.label == label) {
      return &node;
    }
  }
  return nullptr;
}

bool has_edge(const cgraph::Fragment& fragment, std::string_view source, std::string_view target, std::string_view relation) {
  for (const auto& edge : fragment.edges) {
    if (edge.source == source && edge.target == target && edge.relation == relation) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> fields_of(const cgraph::Fragment& fragment, std::string_view owner_id) {
  std::vector<std::string> names;
  for (const auto& edge : fragment.edges) {
    if (edge.relation != "defines" || edge.source != owner_id) {
      continue;
    }
    for (const auto& node : fragment.nodes) {
      if (node.id == edge.target && node.kind == "field") {
        names.push_back(node.label);
      }
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

void dump(const cgraph::Fragment& fragment) {
  for (const auto& node : fragment.nodes) {
    std::cerr << "  node " << node.kind << " " << node.label << " @" << (node.source_location ? node.source_location->start_line : 0) << '\n';
  }
  for (const auto& edge : fragment.edges) {
    std::cerr << "  edge " << edge.source << " -" << edge.relation << "-> " << edge.target << '\n';
  }
}

int test_openapi_json() {
  const auto result = cgraph::extract_openapi_document({.source_file = "/svc/api/openapi.json", .source = R"json({
  "openapi": "3.1.0",
  "info": { "title": "Notes", "version": "1.0.0" },
  "paths": {
    "/api/v1/notebooks": {
      "get": { "operationId": "listNotebooks",
               "responses": { "200": { "content": { "application/json": { "schema": { "type": "array", "items": { "$ref": "#/components/schemas/Notebook" } } } } } } },
      "post": { "operationId": "createNotebook",
                "requestBody": { "content": { "application/json": { "schema": { "$ref": "#/components/schemas/CreateNotebook" } } } },
                "responses": { "201": { "content": { "application/json": { "schema": { "$ref": "#/components/schemas/Notebook" } } } } } }
    },
    "/api/v1/notebooks/{id}/notes": {
      "get": { "responses": { "200": { "description": "inline" } } },
      "delete": { "responses": { "204": {} } },
      "parameters": [{ "name": "id", "in": "path" }]
    },
    "x-internal": { "get": {} }
  },
  "components": {
    "schemas": {
      "Notebook": { "type": "object", "required": ["id", "title"],
                    "properties": { "id": { "type": "string", "format": "uuid" }, "title": { "type": "string" },
                                    "owner": { "$ref": "#/components/schemas/User" },
                                    "tags": { "type": "array", "items": { "type": "string" } } } },
      "CreateNotebook": { "type": "object", "properties": { "title": { "type": "string" } } },
      "User": { "type": "object", "properties": { "id": { "type": "string" } } },
      "AdminUser": { "allOf": [{ "$ref": "#/components/schemas/User" }, { "type": "object", "properties": { "role": { "type": "string" } } }] }
    }
  }
})json"});
  const auto& fragment = result.fragment;
  if (!fragment.warnings.empty()) {
    return fail("openapi: unexpected warnings");
  }
  const auto* file = node_labelled(fragment, "file", "api/openapi.json");
  const auto* list = node_labelled(fragment, "endpoint", "GET /api/v1/notebooks");
  const auto* create = node_labelled(fragment, "endpoint", "POST /api/v1/notebooks");
  const auto* notes = node_labelled(fragment, "endpoint", "GET /api/v1/notebooks/{id}/notes");
  const auto* del = node_labelled(fragment, "endpoint", "DELETE /api/v1/notebooks/{id}/notes");
  if (file == nullptr || list == nullptr || create == nullptr || notes == nullptr || del == nullptr) {
    dump(fragment);
    return fail("openapi: file node and four endpoints");
  }
  if (notes->id != "endpoint:GET /api/v1/notebooks/{}/notes" || notes->properties.at("path") != "/api/v1/notebooks/{id}/notes" ||
      notes->properties.at("documented") != "true" || list->properties.at("operation") != "listNotebooks" ||
      notes->source_location->start_line != 12) {
    return fail("openapi: canonical id, documented flag, operation id, and the path's line");
  }
  std::size_t endpoints = 0;
  for (const auto& node : fragment.nodes) {
    endpoints += node.kind == "endpoint" ? 1 : 0;
  }
  if (endpoints != 4) {
    return fail("openapi: `parameters` and `x-` keys are not methods");
  }
  const auto* notebook = node_labelled(fragment, "schema", "Notebook");
  const auto* user = node_labelled(fragment, "schema", "User");
  const auto* admin = node_labelled(fragment, "schema", "AdminUser");
  const auto* input = node_labelled(fragment, "schema", "CreateNotebook");
  if (notebook == nullptr || user == nullptr || admin == nullptr || input == nullptr) {
    return fail("openapi: four component schemas");
  }
  if (fields_of(fragment, notebook->id) != std::vector<std::string>{"id", "owner", "tags", "title"}) {
    dump(fragment);
    return fail("openapi: Notebook fields");
  }
  const auto* tags = node_labelled(fragment, "field", "tags");
  const auto* id_field = node_labelled(fragment, "field", "id");
  if (tags == nullptr || tags->properties.at("type_text") != "string[]" || tags->properties.at("optional") != "true" ||
      id_field == nullptr || id_field->properties.at("optional") != "false" || id_field->properties.at("type_text") != "string (uuid)") {
    return fail("openapi: field type_text and required");
  }
  if (!has_edge(fragment, list->id, notebook->id, "RESPONDS_WITH") || !has_edge(fragment, create->id, notebook->id, "RESPONDS_WITH") ||
      !has_edge(fragment, create->id, input->id, "ACCEPTS") || !has_edge(fragment, notebook->id, user->id, "references") ||
      !has_edge(fragment, admin->id, user->id, "inherits") || !has_edge(fragment, file->id, list->id, "contains") ||
      !has_edge(fragment, file->id, notebook->id, "contains")) {
    dump(fragment);
    return fail("openapi: RESPONDS_WITH / ACCEPTS / references / inherits / contains edges");
  }
  // Not a document: a warning and no nodes.
  const auto other = cgraph::extract_openapi_document({.source_file = "/svc/openapi-notes.json", .source = R"({"name": "x"})"});
  if (other.fragment.warnings.size() != 1 || !other.fragment.nodes.empty()) {
    return fail("openapi: a JSON file without the openapi key is refused with a warning");
  }
  return 0;
}

int test_protobuf() {
  const auto result = cgraph::extract_protobuf({.source_file = "/svc/proto/notes.proto", .source = R"proto(
syntax = "proto3";
package notes.v1;
import "google/protobuf/timestamp.proto";
option java_package = "com.example";

// A notebook.
message Notebook {
  string id = 1;
  string title = 2 [deprecated = true];
  repeated Note notes = 3;
  map<string, Tag> tags = 4;
  optional Owner owner = 5;
  message Owner { string user_id = 1; }
  oneof visibility {
    bool is_public = 6;
    string share_token = 7;
  }
  reserved 8, 9;
}
message Note { string body = 1; google.protobuf.Timestamp created_at = 2; }
message Tag { string name = 1; }
enum Kind { KIND_UNSPECIFIED = 0; KIND_LAB = 1; }
message ListRequest { Kind kind = 1; }
message ListResponse { repeated Notebook notebooks = 1; }

service Notebooks {
  rpc List (ListRequest) returns (ListResponse);
  rpc Watch (ListRequest) returns (stream Notebook) { option idempotency_level = NO_SIDE_EFFECTS; }
}
)proto"});
  const auto& fragment = result.fragment;
  const auto* notebook = node_labelled(fragment, "schema", "Notebook");
  const auto* owner = node_labelled(fragment, "schema", "Notebook.Owner");
  const auto* note = node_labelled(fragment, "schema", "Note");
  const auto* tag = node_labelled(fragment, "schema", "Tag");
  const auto* kind = node_labelled(fragment, "schema", "Kind");
  const auto* service = node_labelled(fragment, "type", "Notebooks");
  if (notebook == nullptr || owner == nullptr || note == nullptr || tag == nullptr || kind == nullptr || service == nullptr) {
    dump(fragment);
    return fail("proto: messages, a nested message, an enum, the service");
  }
  if (notebook->source_location->start_line != 8 || kind->properties.at("enum") != "true" || service->properties.at("protobuf") != "service") {
    return fail("proto: lines and properties");
  }
  if (fields_of(fragment, notebook->id) != std::vector<std::string>{"id", "is_public", "notes", "owner", "share_token", "tags", "title"}) {
    dump(fragment);
    return fail("proto: Notebook fields incl. oneof members, not the nested message's");
  }
  if (fields_of(fragment, kind->id) != std::vector<std::string>{"KIND_LAB", "KIND_UNSPECIFIED"} ||
      fields_of(fragment, owner->id) != std::vector<std::string>{"user_id"}) {
    return fail("proto: enum values and nested message fields");
  }
  const auto* notes_field = node_labelled(fragment, "field", "notes");
  const auto* tags_field = node_labelled(fragment, "field", "tags");
  if (notes_field == nullptr || notes_field->properties.at("type_text") != "repeated Note" || tags_field == nullptr ||
      tags_field->properties.at("type_text") != "map<string,Tag>") {
    return fail("proto: field type_text");
  }
  const auto* list = node_labelled(fragment, "endpoint", "POST /notes.v1.Notebooks/List");
  const auto* watch = node_labelled(fragment, "endpoint", "POST /notes.v1.Notebooks/Watch");
  const auto* request = node_labelled(fragment, "schema", "ListRequest");
  const auto* response = node_labelled(fragment, "schema", "ListResponse");
  if (list == nullptr || watch == nullptr || request == nullptr || response == nullptr ||
      list->id != "endpoint:POST /notes.v1.Notebooks/List" || list->properties.at("protocol") != "grpc") {
    dump(fragment);
    return fail("proto: rpc endpoints at the gRPC path");
  }
  if (!has_edge(fragment, service->id, list->id, "defines") || !has_edge(fragment, list->id, request->id, "ACCEPTS") ||
      !has_edge(fragment, list->id, response->id, "RESPONDS_WITH") || !has_edge(fragment, watch->id, notebook->id, "RESPONDS_WITH") ||
      !has_edge(fragment, notebook->id, note->id, "references") || !has_edge(fragment, notebook->id, tag->id, "references") ||
      !has_edge(fragment, notebook->id, owner->id, "references") || !has_edge(fragment, response->id, notebook->id, "references")) {
    dump(fragment);
    return fail("proto: defines / ACCEPTS / RESPONDS_WITH / references edges");
  }
  for (const auto& edge : fragment.edges) {
    if (edge.relation == "references" && edge.source == note->id) {
      return fail("proto: an imported type (google.protobuf.Timestamp) has no target");
    }
  }
  return 0;
}

int test_graphql_sdl() {
  const auto result = cgraph::extract_graphql_sdl({.source_file = "/svc/schema.graphql", .source = R"gql(
# Notes API
"""
A notebook.
"""
type Notebook implements Node & Timestamped @key(fields: "id") {
  id: ID!
  title: String!
  "Optional owner"
  owner: User
  notes(first: Int = 10, after: String): [Note!]!
  kind: Kind!
}
interface Node { id: ID! }
interface Timestamped { createdAt: String! }
type User { id: ID!, name: String }
type Note { body: String! }
enum Kind { LAB FIELD }
input CreateNotebookInput { title: String!, kind: Kind = LAB }
union SearchResult = Notebook | Note
scalar DateTime
extend type Notebook { archived: Boolean }
type Query {
  notebooks(kind: Kind, first: Int = 20): [Notebook!]!
  notebook(id: ID!): Notebook
}
type Mutation {
  createNotebook(input: CreateNotebookInput!): Notebook! @auth(requires: ADMIN)
}
schema {
  query: Query
  mutation: Mutation
}
)gql"});
  const auto& fragment = result.fragment;
  const auto* notebook = node_labelled(fragment, "schema", "Notebook");
  const auto* node = node_labelled(fragment, "schema", "Node");
  const auto* user = node_labelled(fragment, "schema", "User");
  const auto* kind = node_labelled(fragment, "schema", "Kind");
  const auto* input = node_labelled(fragment, "schema", "CreateNotebookInput");
  const auto* search = node_labelled(fragment, "schema", "SearchResult");
  const auto* scalar = node_labelled(fragment, "schema", "DateTime");
  const auto* query = node_labelled(fragment, "schema", "Query");
  if (notebook == nullptr || node == nullptr || user == nullptr || kind == nullptr || input == nullptr || search == nullptr ||
      scalar == nullptr || query == nullptr) {
    dump(fragment);
    return fail("graphql: type, interface, enum, input, union, scalar and root type nodes");
  }
  if (notebook->source_location->start_line != 6 || notebook->properties.at("graphql") != "type" || input->properties.at("graphql") != "input") {
    return fail("graphql: lines and kinds");
  }
  if (fields_of(fragment, notebook->id) != std::vector<std::string>{"archived", "id", "kind", "notes", "owner", "title"}) {
    dump(fragment);
    return fail("graphql: Notebook fields incl. the extension's, descriptions and directives skipped");
  }
  const auto* notes_field = node_labelled(fragment, "field", "notes");
  const auto* owner_field = node_labelled(fragment, "field", "owner");
  if (notes_field == nullptr || notes_field->properties.at("type_text") != "[Note!]!" || notes_field->properties.at("optional") != "false" ||
      owner_field == nullptr || owner_field->properties.at("optional") != "true") {
    return fail("graphql: field type_text and nullability");
  }
  if (fields_of(fragment, kind->id) != std::vector<std::string>{"FIELD", "LAB"}) {
    return fail("graphql: enum values");
  }
  const auto* list = node_labelled(fragment, "endpoint", "QUERY notebooks");
  const auto* one = node_labelled(fragment, "endpoint", "QUERY notebook");
  const auto* create = node_labelled(fragment, "endpoint", "MUTATION createNotebook");
  if (list == nullptr || one == nullptr || create == nullptr || list->id != "endpoint:QUERY notebooks" ||
      create->properties.at("protocol") != "graphql") {
    dump(fragment);
    return fail("graphql: root fields are endpoints");
  }
  if (!fields_of(fragment, query->id).empty()) {
    return fail("graphql: a root type's fields are operations, not fields");
  }
  if (!has_edge(fragment, query->id, list->id, "defines") || !has_edge(fragment, list->id, notebook->id, "RESPONDS_WITH") ||
      !has_edge(fragment, list->id, kind->id, "ACCEPTS") || !has_edge(fragment, create->id, input->id, "ACCEPTS") ||
      !has_edge(fragment, create->id, notebook->id, "RESPONDS_WITH") || !has_edge(fragment, notebook->id, node->id, "implements") ||
      !has_edge(fragment, notebook->id, user->id, "references") || !has_edge(fragment, search->id, notebook->id, "references") ||
      !has_edge(fragment, input->id, kind->id, "references")) {
    dump(fragment);
    return fail("graphql: defines / RESPONDS_WITH / ACCEPTS / implements / references edges");
  }
  for (const auto& edge : fragment.edges) {
    if (edge.relation == "references" && edge.source == user->id) {
      return fail("graphql: built-in scalars are not referenced schemas");
    }
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += test_openapi_json();
  failures += test_protobuf();
  failures += test_graphql_sdl();
  return failures == 0 ? 0 : 1;
}
