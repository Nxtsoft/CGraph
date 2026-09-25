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
  // The receiver's text when it is a bare identifier (`XML` in
  // `XML.toJSONObject(s)`). Empty for a complex receiver (a chain, a call
  // result, an indexed expression) where no single name is being denoted.
  //
  // A member call is normally unresolvable project-wide because the receiver's
  // TYPE is unknown. But a bare identifier naming a class in the project is not
  // unknown — a static call names its class outright, and that is evidence, not
  // a guess. Resolution uses it to scope the method lookup to that class.
  std::string receiver_label;
  // The scope a qualified callee names, as written (`std` in `std::find(...)`,
  // `a::b` in `a::b::f()`), empty for an unqualified or member callee. A
  // qualifier is evidence about which declaration the call means: the leaf
  // name alone binds `std::find` to any project function called `find`,
  // which is a false dependent on every call into the standard library.
  std::string qualifier;
};

// A type/heritage relationship discovered during extraction, resolved against
// the project after merge. `inherits`/`implements` come from a class or
// interface heritage clause; `references` from a class/interface member's type
// annotation. The target is a bare type name resolved by import (and, for
// heritage only, a same-file declaration) — mirroring Graphify, which never
// resolves these by a project-wide name guess.
//
// Two further kinds carry HTTP contract facts for resolve_contracts (see
// contracts.hpp) and are skipped by resolve_raw_relations: `route` (source = an
// inline handler, target = its router chain's identifier, context = "<verb>
// <path>") and `mounts` (source = the mounting chain's variable node, target =
// the mounted chain's identifier, context = the mount path or empty).
struct RawRelation {
  std::string source_id;      // the class / interface / method node id
  std::string target_label;   // the referenced type name
  std::string relation;       // "inherits" | "implements" | "references" | "route" | "mounts"
  std::string context;        // "type" | "parameter_type" | "return_type" | "field" | "generic_arg" | route/mount text
  std::string source_file;
  bool allow_same_file = false;  // heritage may resolve to a same-file declaration; references may not
};

struct ExtractionContext {
  // Where the bytes were read from; every emitted node's `source_file`, and the
  // key its hash is recorded under.
  std::string source_file;
  // The same file relative to the project root. Every node id derives from this,
  // never from `source_file`, so the same tree extracted from two absolute roots
  // yields byte-identical ids and no id carries the machine's directory layout.
  std::string relative_path;
  std::string_view source;
};

using ImportHandler = std::function<void(const TSNode&, const ExtractionContext&, Fragment&)>;
using ResolveFunctionName = std::function<std::string(const TSNode&, const ExtractionContext&)>;
using ResolveCalleeName = std::function<std::string(const TSNode&, const ExtractionContext&)>;
// The scope text of a qualified callee (`std::filesystem` for
// `std::filesystem::exists(p)`), or empty when the callee is unqualified.
using ResolveCalleeScope = std::function<std::string(const TSNode&, const ExtractionContext&)>;
// The third argument is the innermost enclosing function scope (empty at file /
// class / type scope) — the caller id for any RawCall the walk emits. Handlers
// that emit no calls ignore it. The last argument collects relation facts the
// walk finds on non-symbol nodes (JavaScript's router mounts); handlers that
// emit none ignore it.
using ExtraWalk = std::function<void(const TSNode&, const ExtractionContext&, const std::string&, Fragment&, std::vector<RawCall>&, std::vector<RawRelation>&)>;
// True when a function node is a method by its surrounding context rather than
// its grammar shape (Rust's `function_item` inside an `impl_item` — the node
// type alone cannot tell a method from a free function). Complements
// method_node_types, which handles grammar-shape methods (Go).
using MethodPredicate = std::function<bool(const TSNode&, const ExtractionContext&)>;
// Decides whether a nested anonymous function -- an arrow that is not a
// module-level `const Foo = () => {}` -- is nonetheless a graph node and a call
// scope. The walker treats every such arrow as a boundary (Graphify seeds no
// calls from callbacks); a language opts specific shapes back in, such as the
// handler an HTTP route registration passes inline. The name still comes from
// `resolve_function_name`, which must return one for the same node.
using NestedFunctionScope = std::function<bool(const TSNode&, const ExtractionContext&)>;
// Invoked for each class/interface node (with its already-assigned node id) to
// emit heritage and member type-reference facts.
using RelationHandler = std::function<void(const TSNode&, const ExtractionContext&, const std::string&, std::vector<RawRelation>&)>;
// Rewrites a file's source text BEFORE parsing, preserving every byte offset
// (so node positions stay correct). Rust uses it to blank `cfg_*! { ... }`
// item-wrapper macros with spaces, so the items inside — invisible as opaque
// macro token-trees otherwise — parse in place with their real enclosing impl
// and line numbers. Returns the rewritten source, or an empty string to skip.
using PreprocessSource = std::function<std::string(std::string_view)>;

struct InternedSymbols {
  std::vector<TSSymbol> class_nodes;
  std::vector<TSSymbol> function_nodes;
  std::vector<TSSymbol> method_nodes;
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
  // Function declarations that are METHODS by grammar shape alone (Go's
  // method_declaration). Class-contained functions are already marked by the
  // `method` containment relation; this covers receiver syntax with no
  // enclosing class node. Members of this list must also be listed in
  // function_node_types — it only tags, it does not extract.
  std::vector<std::string> method_node_types;
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
  // The field naming a call's RECEIVER when the grammar exposes it directly on
  // the call node instead of wrapping it in a member-access node. Java's
  // `method_invocation` splits into `object` + `name` fields with no wrapper, so
  // call_member_node_types never matches and `obj.method()` would be recorded as
  // a plain call. When this field is set and present on the call node, the call
  // is a member call — same meaning as the wrapper case: the receiver type is
  // unknown, so the bare name must not be matched project-wide.
  std::string call_receiver_field;
  // Class-like declarations that are CONTRACTS rather than concrete types
  // (Java's `interface_declaration`). Their methods are promises, not
  // implementations: dispatch resolution reads them as an interface's method
  // set and must not count them toward a concrete type's own methods.
  std::vector<std::string> interface_node_types;
  // Resolves a non-member callee to its leaf name through the grammar
  // (`cgraph::run_one_shot()` -> `run_one_shot`). A qualified call is NOT a member
  // call: the name is fully determined by the qualification, so it stays eligible
  // for project-wide resolution, unlike `obj.method()` whose receiver type is
  // unknown.
  //
  // This is a resolver rather than a string rule because `::` legitimately appears
  // in nine distinct callee node types, and reducing the callee's TEXT at a
  // separator corrupts several of them: `ns::make<zoo::Beast>` becomes `Beast>`,
  // which make_id normalizes to `Beast` -- fabricating a call to an unrelated
  // struct while losing the real call. Returning empty drops the call.
  ResolveCalleeName resolve_callee_name;
  // Returns the qualifier of a qualified callee so resolution can require the
  // resolved declaration to live in that scope (see RawCall::qualifier).
  ResolveCalleeScope resolve_callee_scope;
  ImportHandler import_handler;
  ResolveFunctionName resolve_function_name;
  ExtraWalk extra_walk;
  RelationHandler relation_handler;
  MethodPredicate method_predicate;
  NestedFunctionScope nested_function_scope;
  PreprocessSource preprocess_source;
  InternedSymbols symbols;
  bool extract_members = false;
  std::function<void(const TSNode&, const ExtractionContext&, const std::string&, Fragment&)> member_handler;
  bool class_requires_body = false;
};

void intern_node_symbols(LanguageConfig& config, const TSLanguage* language);
[[nodiscard]] bool contains_symbol(const std::vector<TSSymbol>& symbols, TSSymbol symbol);

// The one id-collision guard for extracted nodes. `seed` is the id's natural
// spelling (`file:label` for a symbol, `file:Owner::member` for a field);
// make_id collapses it, so unrelated spellings land on one id -- `First::size`
// and `first_size` both normalize to `first_size`. merge_fragments keeps the
// first node with a given id and drops the rest without a warning
// (graph_builder.cpp), so a collision silently loses a symbol. When the natural
// id is taken, fall back to the declaration's line, then its column, then a
// counter, all stable for a given file so the id stays deterministic.
[[nodiscard]] std::string unique_node_id(
    const std::string& seed, const SourceLocation& location, const Fragment& fragment);

// The one field emitter: every language's members become `field` nodes and
// `defines` edges here, through unique_node_id, so a member never lands on an
// id another declaration in the file already holds.
//
// A field yields to a symbol, never the reverse: add_symbol_node relocates an
// already-emitted field that holds the id a function or type wants, because a
// function's id is what an agent asks impact and explain about, and it must not
// change because a struct one line up happens to have a snake_case-equivalent
// member.
std::string add_field_node(
    const ExtractionContext& context,
    const std::string& owner_id,
    std::string_view owner_name,
    std::string label,
    const SourceLocation& location,
    Properties properties,
    Fragment& fragment);

}  // namespace cgraph
