#pragma once

#include "cgraph/types.hpp"

#include <tree_sitter/api.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

struct RawCall {
  std::string caller_id;
  std::string callee_label;
  std::string source_file;
  std::optional<SourceLocation> source_location;
  // A member call (`obj.method()`, `this.foo()`): the label is the bare
  // property name. These resolve only against the caller's own file — the
  // receiver type is unknown, so a project-wide name match would be a guess.
  bool is_member_call = false;
};

// A type/heritage relationship discovered during extraction, resolved against
// the project after merge. `inherits`/`implements` come from a class or
// interface heritage clause; `references` from a class/interface member's type
// annotation. The target is a bare type name resolved by import (and, for
// heritage only, a same-file declaration) — mirroring Graphify, which never
// resolves these by a project-wide name guess.
struct RawRelation {
  std::string source_id;      // the class / interface / method node id
  std::string target_label;   // the referenced type name
  std::string relation;       // "inherits" | "implements" | "references"
  std::string context;        // "type" | "parameter_type" | "return_type" | "field" | "generic_arg"
  std::string source_file;
  bool allow_same_file = false;  // heritage may resolve to a same-file declaration; references may not
};

struct ExtractionContext {
  std::string source_file;
  std::string_view source;
};

using ImportHandler = std::function<void(const TSNode&, const ExtractionContext&, Fragment&)>;
using ResolveFunctionName = std::function<std::string(const TSNode&, const ExtractionContext&)>;
using ExtraWalk = std::function<void(const TSNode&, const ExtractionContext&, Fragment&, std::vector<RawCall>&)>;
// Invoked for each class/interface node (with its already-assigned node id) to
// emit heritage and member type-reference facts.
using RelationHandler = std::function<void(const TSNode&, const ExtractionContext&, const std::string&, std::vector<RawRelation>&)>;

struct InternedSymbols {
  std::vector<TSSymbol> class_nodes;
  std::vector<TSSymbol> function_nodes;
  std::vector<TSSymbol> type_nodes;
  std::vector<TSSymbol> import_nodes;
  std::vector<TSSymbol> call_nodes;
};

struct LanguageConfig {
  std::string name;
  std::string grammar_name;
  std::vector<std::string> extensions;
  std::vector<std::string> class_node_types;
  std::vector<std::string> function_node_types;
  std::vector<std::string> type_node_types;
  std::vector<std::string> import_node_types;
  std::vector<std::string> call_node_types;
  std::vector<std::string> name_fields;
  std::vector<std::string> body_fields;
  std::vector<std::string> call_accessor_fields;
  // Node types of a member/property access used as a call target
  // (`obj.method()` -> "member_expression"), and the field holding the bare
  // property name ("property"). When the accessor child is one of these types,
  // the call is recorded as a member call carrying just the property name.
  std::vector<std::string> call_member_node_types;
  std::string call_member_field;
  // Scope-resolution separator for a qualified callee (`cgraph::run_one_shot()`
  // -> `run_one_shot`), and the callee node types it may be applied to. A
  // qualified call is NOT a member call: the name is fully determined, so it
  // stays eligible for project-wide resolution, unlike `obj.method()` whose
  // receiver type is unknown.
  //
  // The reduction is a string operation, so it MUST be gated on the callee's
  // grammar node type. Applied to arbitrary callee text it corrupts anything else
  // containing the separator: a template instantiation `wrapper<zoo::Beast>`
  // reduces to `Beast>`, which make_id normalizes to `Beast` -- fabricating a call
  // to an unrelated struct while losing the real call to `wrapper`. Only a node
  // type listed here is reduced; everything else is taken verbatim.
  std::vector<std::string> call_qualified_node_types;
  std::string call_scope_separator;
  ImportHandler import_handler;
  ResolveFunctionName resolve_function_name;
  ExtraWalk extra_walk;
  RelationHandler relation_handler;
  InternedSymbols symbols;
};

void intern_node_symbols(LanguageConfig& config, const TSLanguage* language);
[[nodiscard]] bool contains_symbol(const std::vector<TSSymbol>& symbols, TSSymbol symbol);

}  // namespace cgraph
