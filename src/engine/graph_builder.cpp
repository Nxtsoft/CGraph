#include "cgraph/graph_builder.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cgraph {

// External linkage (declared in graph_builder.hpp): callers outside merge need
// the exact node identity merge assigns. Behavior is unchanged from the prior
// internal helper.
std::string node_key(const Node& node) {
  if (!node.id.empty()) {
    return node.id;
  }
  return make_id(node.source_file + ":" + node.kind + ":" + node.label);
}

namespace {

constexpr std::string_view kCallRelation = "CALLS";

[[nodiscard]] std::string edge_key(const Edge& edge) {
  return edge.source + "\n" + edge.relation + "\n" + edge.target;
}

// Language built-in callables. Graphify never resolves a call to one of these
// names to a project node (a `new Map()` or `console`/`parseInt` call must not
// be wired to a coincidentally same-named user symbol), so we skip them too.
[[nodiscard]] bool is_builtin_global(std::string_view label) {
  static const std::unordered_set<std::string_view> names = {
      // JavaScript / TypeScript ECMAScript built-ins
      "String", "Number", "Boolean", "Object", "Array", "Symbol", "BigInt",
      "Date", "RegExp", "Error", "TypeError", "RangeError", "SyntaxError",
      "ReferenceError", "EvalError", "URIError",
      "Promise", "Map", "Set", "WeakMap", "WeakSet", "JSON", "Math",
      "Reflect", "Proxy", "Intl",
      "parseInt", "parseFloat", "isNaN", "isFinite",
      "encodeURIComponent", "decodeURIComponent", "encodeURI", "decodeURI",
      // Browser / Node common globals
      "URL", "URLSearchParams", "FormData", "Blob", "File",
      "Headers", "Request", "Response", "AbortController", "AbortSignal",
      "TextEncoder", "TextDecoder", "console",
      // Python built-in callables
      "str", "int", "float", "bool", "list", "dict", "set", "tuple", "bytes",
      "len", "range", "enumerate", "zip", "map", "filter", "sum", "min", "max",
      "print", "open", "isinstance", "type", "super", "sorted", "reversed",
      "any", "all", "abs", "round", "next", "iter", "hash", "id", "repr",
      "callable", "getattr", "setattr", "hasattr", "delattr", "vars", "dir",
  };
  return names.contains(label);
}

// True when a node's kind can be the target of a call. This is deliberately the
// SAME set the per-file table admits (see resolve_raw_calls), so the two
// resolution tiers agree on what a symbol is; the project-wide tier had no filter
// at all, which is the defect. `class` is eligible because `Foo()` is a genuine
// constructor call in Python and JavaScript, and `type`/`variable` because a
// module-level binding can hold a callable. A `field` is not: a struct member
// named `connect` is not what `::connect(...)` invokes.
[[nodiscard]] bool is_callable_kind(std::string_view kind) {
  return kind == "function" || kind == "class" || kind == "type" || kind == "variable";
}

// Index for project-wide call resolution. The per-file table filters candidates
// to declared symbol kinds; this one filters to *callable* kinds, which is
// stricter and is what a call target has to be. Without it a call resolved to
// whatever unique node happened to share the name -- measured on this repo, 12 of
// 122 CALLS edges pointed at a struct field, so `impact` reported false
// dependents (`unix_endpoint_is_live` "called" a field named `connect` because it
// invokes the ::connect syscall).
[[nodiscard]] std::unordered_map<std::string, std::vector<std::string>> label_index(const GraphSnapshot& graph) {
  std::unordered_map<std::string, std::vector<std::string>> index;
  for (const auto& node : graph.nodes) {
    if (!is_callable_kind(node.kind)) {
      continue;
    }
    index[make_id(node.label)].push_back(node.id);
  }
  return index;
}

}  // namespace

GraphSnapshot merge_fragments(std::span<const Fragment> fragments) {
  GraphSnapshot graph;
  graph.build_state = BuildState::DeterministicReady;

  // Maintain the dedup indexes once across every fragment. Calling
  // merge_fragment() per fragment rebuilt these sets from the whole accumulated
  // graph each time, making a bulk merge O(fragments * nodes) — the dominant
  // cost of a cold build. First-occurrence-wins order is unchanged: a duplicate
  // id (within or across fragments) fails the same insert and is skipped.
  std::unordered_set<std::string> node_ids;
  std::unordered_set<std::string> edge_ids;
  std::unordered_set<std::string> hyperedge_ids;

  for (const auto& fragment : fragments) {
    for (auto node : fragment.nodes) {
      node.id = node_key(node);
      if (node_ids.insert(node.id).second) {
        graph.nodes.push_back(std::move(node));
      }
    }
    for (const auto& edge : fragment.edges) {
      if (edge_ids.insert(edge_key(edge)).second) {
        graph.edges.push_back(edge);
      }
    }
    for (const auto& hyperedge : fragment.hyperedges) {
      if (hyperedge_ids.insert(hyperedge.id).second) {
        graph.hyperedges.push_back(hyperedge);
      }
    }
  }
  return graph;
}

void merge_fragment(GraphSnapshot& graph, const Fragment& fragment) {
  std::unordered_set<std::string> seen_node_ids;
  std::unordered_set<std::string> graph_node_ids;
  std::unordered_set<std::string> graph_edge_ids;

  for (const auto& node : graph.nodes) {
    graph_node_ids.insert(node.id);
  }
  for (const auto& edge : graph.edges) {
    graph_edge_ids.insert(edge_key(edge));
  }

  for (auto node : fragment.nodes) {
    node.id = node_key(node);
    if (!seen_node_ids.insert(node.id).second) {
      continue;
    }
    if (graph_node_ids.insert(node.id).second) {
      graph.nodes.push_back(std::move(node));
    }
  }

  for (const auto& edge : fragment.edges) {
    const auto key = edge_key(edge);
    if (graph_edge_ids.insert(key).second) {
      graph.edges.push_back(edge);
    }
  }

  for (const auto& hyperedge : fragment.hyperedges) {
    const auto duplicate = std::ranges::any_of(graph.hyperedges, [&hyperedge](const Hyperedge& existing) {
      return existing.id == hyperedge.id;
    });
    if (!duplicate) {
      graph.hyperedges.push_back(hyperedge);
    }
  }
}

void resolve_imports(GraphSnapshot& graph, std::span<const PathAlias> aliases) {
  namespace fs = std::filesystem;

  // Index every project file by its extension-stripped path, and index.* files
  // also by their directory, so specifiers that omit the extension or point at a
  // package directory ("./utils", "../lib/foo") resolve to the real file node.
  std::unordered_map<std::string, std::string> file_id_by_key;
  std::unordered_map<std::string, std::string> source_of_file;
  std::vector<std::pair<std::string, std::string>> files_by_path;  // (normalized path, id) for suffix matching
  for (const auto& node : graph.nodes) {
    if (node.kind != "file") {
      continue;
    }
    source_of_file.emplace(node.id, node.source_file);
    const fs::path path = fs::path(node.source_file).lexically_normal();
    file_id_by_key[path.generic_string()] = node.id;
    file_id_by_key.emplace((path.parent_path() / path.stem()).generic_string(), node.id);
    // A directory import resolves to its index module: JS `./utils` ->
    // utils/index.ts, Python `import pkg` -> pkg/__init__.py.
    if (path.stem() == "index" || path.stem() == "__init__") {
      file_id_by_key.emplace(path.parent_path().generic_string(), node.id);
      files_by_path.emplace_back(path.parent_path().generic_string(), node.id);
    }
    files_by_path.emplace_back(path.generic_string(), node.id);
    // Extension-stripped form, so a Python dotted spec ("itsdangerous/signer",
    // no extension to spell) can suffix-match the real file.
    files_by_path.emplace_back((path.parent_path() / path.stem()).generic_string(), node.id);
  }

  // Resolve a header-style include spec ("cgraph/types.hpp") to the project file
  // whose path ends with it — how an include directory resolves a header without
  // the consumer knowing the include roots. Returns a match only when exactly one
  // file qualifies, so an ambiguous spec yields no (wrong) edge.
  const auto suffix_match = [&](const std::string& spec) -> std::optional<std::string> {
    const std::string needle = "/" + spec;
    std::optional<std::string> match;
    for (const auto& [path, id] : files_by_path) {
      if (path == spec || path.ends_with(needle)) {
        if (match && *match != id) {
          return std::nullopt;  // ambiguous across distinct files
        }
        match = id;  // a second path form of the same file is not ambiguity
      }
    }
    return match;
  };

  const auto lookup = [&](const std::string& candidate) -> std::optional<std::string> {
    const fs::path path = fs::path(candidate).lexically_normal();
    if (const auto found = file_id_by_key.find(path.generic_string()); found != file_id_by_key.end()) {
      return found->second;
    }
    // TypeScript NodeNext imports spell the extension as ".js" while the source
    // file is ".ts"; fall back to the extension-stripped stem.
    if (const auto stem = (path.parent_path() / path.stem()).generic_string();
        stem != path.generic_string()) {
      if (const auto found = file_id_by_key.find(stem); found != file_id_by_key.end()) {
        return found->second;
      }
    }
    return std::nullopt;
  };

  const auto resolve_file = [&](const std::string& import_path) -> std::optional<std::string> {
    if (import_path.empty()) {
      return std::nullopt;
    }
    if (const auto direct = lookup(import_path)) {
      return direct;
    }
    // A bare specifier may be a tsconfig path alias (`@/lib/utils`). Expand it to
    // its real project path and retry — resolving dependencies a relative-only
    // resolver (and Graphify) leaves dangling.
    for (const auto& candidate : expand_path_alias(aliases, import_path)) {
      if (const auto resolved = lookup(candidate)) {
        return resolved;
      }
    }
    // Last resort: header-style suffix match (C/C++ #include resolved via an
    // include directory). Only used when direct/alias lookup found nothing.
    return suffix_match(import_path);
  };

  // Rust `use` paths carry no extension and can live at `<path>.rs` OR
  // `<path>/mod.rs`; both spellings of one module are matched here, unique
  // across the project or nothing (two candidates would make the edge a guess,
  // exactly like suffix_match's ambiguity rule -- including the invalid-Rust
  // case where both layouts of one module exist).
  const auto rust_module_match = [&](const std::string& spec) -> std::optional<std::string> {
    const std::string file_needle = "/" + spec + ".rs";
    const std::string mod_needle = "/" + spec + "/mod.rs";
    const std::string file_exact = spec + ".rs";
    const std::string mod_exact = spec + "/mod.rs";
    std::optional<std::string> match;
    for (const auto& [path, id] : files_by_path) {
      if (path == file_exact || path == mod_exact || path.ends_with(file_needle) || path.ends_with(mod_needle)) {
        if (match && *match != id) {
          return std::nullopt;  // ambiguous
        }
        match = id;
      }
    }
    return match;
  };

  // Cargo package roots, for `use` paths that address a crate by name: each
  // `<dir>/src/` directory in the project claims the crate name `<dir>` (with
  // `-` matching `_`, Cargo's package-name/dir convention). An integration test
  // (`tests/*.rs`) compiles as a separate crate and can ONLY spell the library
  // by its package name — `use repro2::add;` — a spelling no module-layout walk
  // can reach (issue #58). A name claimed by more than one src root resolves to
  // nothing rather than guessing between crates.
  std::unordered_map<std::string, std::unordered_set<std::string>> crate_src_roots;
  for (const auto& [path, id] : files_by_path) {
    const auto pos = path.rfind("/src/");
    if (pos == std::string::npos) {
      continue;
    }
    const auto prefix = path.substr(0, pos);
    const auto slash = prefix.rfind('/');
    auto dir = slash == std::string::npos ? prefix : prefix.substr(slash + 1);
    if (dir.empty()) {
      continue;
    }
    std::ranges::replace(dir, '-', '_');
    crate_src_roots[dir].insert(path.substr(0, pos + 4));  // ".../<dir>/src"
  }

  // Resolve "<crate>/<segments...>" inside the named crate's src root. Returns
  // the module file id and whether the full path named a module (vs. its parent
  // module with the leaf as a declared item, the same two-step every Rust stub
  // goes through).
  const auto rust_extern_crate_match =
      [&](const std::string& spec, bool leaf_may_be_item) -> std::pair<std::optional<std::string>, bool> {
    const auto slash = spec.find('/');
    const auto first = slash == std::string::npos ? spec : spec.substr(0, slash);
    const auto rest = slash == std::string::npos ? std::string{} : spec.substr(slash + 1);
    const auto roots = crate_src_roots.find(first);
    if (roots == crate_src_roots.end() || roots->second.size() != 1) {
      return {std::nullopt, false};
    }
    const auto& root = *roots->second.begin();
    const auto crate_root_file = [&]() -> std::optional<std::string> {
      if (const auto lib = lookup(root + "/lib.rs")) {
        return lib;
      }
      return lookup(root + "/main.rs");
    };
    if (rest.empty()) {
      return {crate_root_file(), true};
    }
    if (const auto as_file = lookup(root + "/" + rest + ".rs")) {
      return {as_file, true};
    }
    if (const auto as_mod = lookup(root + "/" + rest + "/mod.rs")) {
      return {as_mod, true};
    }
    if (leaf_may_be_item) {
      const auto parent_end = rest.rfind('/');
      if (parent_end == std::string::npos) {
        return {crate_root_file(), false};  // `use crate_name::Item;`
      }
      const auto parent = rest.substr(0, parent_end);
      if (const auto as_file = lookup(root + "/" + parent + ".rs")) {
        return {as_file, false};
      }
      if (const auto as_mod = lookup(root + "/" + parent + "/mod.rs")) {
        return {as_mod, false};
      }
    }
    return {std::nullopt, false};
  };

  std::unordered_set<std::string> node_ids;
  node_ids.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    node_ids.insert(node.id);
  }

  // Map each resolvable stub onto the real file (module) or declared symbol
  // (import) it refers to. Stubs that resolve to no project file are third-party
  // package imports (`import {x} from "react"`); Graphify never materialises
  // those, so we drop the stub and its edges rather than leaving leaf clutter.
  std::unordered_map<std::string, std::string> remap;
  std::unordered_set<std::string> removed;
  std::unordered_set<std::string> dropped;
  for (const auto& node : graph.nodes) {
    if (node.kind != "import" && node.kind != "module") {
      continue;
    }
    const auto prop = node.properties.find("import_path");
    if (prop == node.properties.end()) {
      continue;
    }
    // A Rust stub (module_layout=rust) cannot tell from syntax whether its last
    // segment names a module or an item declared in one: try the full path as a
    // module file first (`use a::b;`), then the parent path as the module with
    // the leaf as its declared item (`use a::b::Item;`).
    const auto layout = node.properties.find("module_layout");
    const bool rust_layout = layout != node.properties.end() && layout->second == "rust";
    std::optional<std::string> file_id;
    bool resolved_as_module = node.kind == "module";
    if (rust_layout) {
      file_id = rust_module_match(prop->second);
      if (file_id) {
        resolved_as_module = true;
      } else if (node.kind == "import") {
        if (const auto slash = prop->second.rfind('/'); slash != std::string::npos) {
          file_id = rust_module_match(prop->second.substr(0, slash));
        }
      }
      if (!file_id) {
        // The path's first segment may name a Cargo package (the only spelling
        // available to an integration test, and the common cross-crate spelling
        // in a workspace).
        const auto [crate_file, crate_module] =
            rust_extern_crate_match(prop->second, node.kind == "import");
        if (crate_file) {
          file_id = crate_file;
          resolved_as_module = crate_module || node.kind == "module";
        }
      }
    } else {
      file_id = resolve_file(prop->second);
    }
    if (!file_id) {
      removed.insert(node.id);
      dropped.insert(node.id);  // external package: delete node and its edges
      continue;
    }
    if (resolved_as_module) {
      remap[node.id] = *file_id;
    } else {
      const auto real = make_id(source_of_file[*file_id] + ":" + node.label);
      remap[node.id] = node_ids.contains(real) ? real : *file_id;
    }
    removed.insert(node.id);
  }

  // Follow `pub use` re-export chains (issue #60). An item import that could only
  // be resolved to a module *file* (its `file:Item` symbol node does not exist)
  // has landed on a module that re-exports the item rather than defining it --
  // `use tokio::task::LocalSet` reaching task/mod.rs, which carries `pub use
  // local::LocalSet`. Redirect such imports to the re-export's real target so the
  // reverse walk reaches the defining (and changed) file. Private `use` is not a
  // re-export and is never followed.
  if (!remap.empty()) {
    std::unordered_set<std::string> file_ids;
    file_ids.reserve(source_of_file.size());
    for (const auto& [id, _] : source_of_file) {
      file_ids.insert(id);
    }
    // Which file owns each re-export stub, and what it re-exports by name.
    std::unordered_map<std::string, std::string> reexport_owner;  // stub id -> file id
    for (const auto& edge : graph.edges) {
      if (edge.relation == "re_exports") {
        reexport_owner.emplace(edge.target, edge.source);
      }
    }
    // owner file id -> (item name key -> the re-export's resolved target)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> reexported_by_file;
    for (const auto& node : graph.nodes) {
      if (node.kind != "import" && node.kind != "module") {
        continue;
      }
      if (const auto tag = node.properties.find("reexport");
          tag == node.properties.end() || tag->second != "true") {
        continue;
      }
      const auto owner = reexport_owner.find(node.id);
      const auto target = remap.find(node.id);
      if (owner == reexport_owner.end() || target == remap.end()) {
        continue;
      }
      reexported_by_file[owner->second].emplace(make_id(node.label), target->second);
    }
    if (!reexported_by_file.empty()) {
      // The item stubs that fell back to a module file, keyed for the follow.
      for (const auto& node : graph.nodes) {
        if (node.kind != "import") {
          continue;
        }
        auto slot = remap.find(node.id);
        if (slot == remap.end() || !file_ids.contains(slot->second)) {
          continue;  // unresolved, or resolved straight to a real symbol already
        }
        const auto name_key = make_id(node.label);
        std::string current = slot->second;
        std::unordered_set<std::string> visited;
        for (int hop = 0; hop < 8 && file_ids.contains(current); ++hop) {
          if (!visited.insert(current).second) {
            break;  // cyclic re-export: stop rather than spin
          }
          const auto file = reexported_by_file.find(current);
          if (file == reexported_by_file.end()) {
            break;
          }
          const auto reexp = file->second.find(name_key);
          if (reexp == file->second.end() || reexp->second == current) {
            break;
          }
          current = reexp->second;
        }
        if (current != slot->second) {
          slot->second = current;
        }
      }
    }
  }

  if (removed.empty()) {
    return;
  }

  const auto canonical = [&](const std::string& id) {
    const auto found = remap.find(id);
    return found == remap.end() ? id : found->second;
  };
  std::unordered_set<std::string> seen_edges;
  std::vector<Edge> rewritten;
  rewritten.reserve(graph.edges.size());
  for (auto edge : graph.edges) {
    if (dropped.contains(edge.source) || dropped.contains(edge.target)) {
      continue;  // edge to a dropped third-party import
    }
    edge.source = canonical(edge.source);
    edge.target = canonical(edge.target);
    if (edge.source == edge.target) {
      continue;  // a file importing from itself collapses to a self-edge
    }
    if (seen_edges.insert(edge_key(edge)).second) {
      rewritten.push_back(std::move(edge));
    }
  }
  graph.edges = std::move(rewritten);
  std::erase_if(graph.nodes, [&removed](const Node& node) { return removed.contains(node.id); });
}

void resolve_raw_calls(GraphSnapshot& graph, std::span<const RawCall> raw_calls,
                       CallResolution* outcomes) {
  const auto index = label_index(graph);
  // Methods only, for member-call resolution: nodes marked by the `method`
  // containment relation (class-contained functions) or the extractor's
  // grammar-level tag (Go's method_declaration). A member call's bare name must
  // never bind to a free function or a variable — the receiver is unknown, and
  // that mistake is how `.connect()` used to reach a syscall wrapper.
  std::unordered_set<std::string> method_ids;
  for (const auto& edge : graph.edges) {
    if (edge.relation == "method") {
      method_ids.insert(edge.target);
    }
  }
  std::unordered_map<std::string, std::vector<std::string>> method_index;
  // Every method node id, for validating same-file member-call bindings below.
  std::unordered_set<std::string> method_node_ids;
  for (const auto& node : graph.nodes) {
    const auto tagged = node.properties.find("method");
    if (method_ids.contains(node.id) || (tagged != node.properties.end() && tagged->second == "true")) {
      method_index[make_id(node.label)].push_back(node.id);
      method_node_ids.insert(node.id);
    }
  }
  std::unordered_set<std::string> node_ids;
  std::unordered_map<std::string, std::string> source_file_by_id;
  node_ids.reserve(graph.nodes.size());
  source_file_by_id.reserve(graph.nodes.size());

  // Per-file declared symbols: source_file -> (normalized label -> node id). An
  // empty id marks a label that is declared more than once in the file, so it
  // resolves to no single target.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> local_by_file;
  // "<source_file>\n<normalized label>" -> every declaration bearing that name in
  // that file, in declaration order. Consulted only for an overload set, where
  // the per-file slot has been cleared as ambiguous: without types, ANY member
  // may be the callee, so the call edges to all of them (all INFERRED). One
  // arbitrary pick would leave the other overloads invisible to reverse
  // dependency walks — a change inside them could never reach their callers
  // (issue #52).
  std::unordered_map<std::string, std::vector<std::string>> overload_declarations;
  // Cache make_id(source_file) per distinct source path. The confidence grading
  // below re-normalizes caller/callee file paths per raw call (hundreds of
  // thousands of calls over a few thousand distinct files); memoizing keeps the
  // result byte-identical while collapsing the redundant utf8proc work.
  std::unordered_map<std::string, std::string> file_id_by_source;
  for (const auto& node : graph.nodes) {
    node_ids.insert(node.id);
    source_file_by_id.emplace(node.id, node.source_file);
    if (node.source_file.empty()) {
      continue;
    }
    file_id_by_source.try_emplace(node.source_file, std::string{});
    if (node.kind != "function" && node.kind != "class" && node.kind != "type" && node.kind != "variable") {
      continue;
    }
    auto& by_label = local_by_file[node.source_file];
    const auto label_key = make_id(node.label);
    // Remember every declaration of each name per file, so an overload set can
    // still resolve to something concrete rather than dropping every call to it.
    overload_declarations[node.source_file + "\n" + label_key].push_back(node.id);
    const auto [slot, inserted] = by_label.emplace(label_key, node.id);
    if (!inserted && slot->second != node.id) {
      slot->second.clear();  // ambiguous within the file
    }
  }
  // Resolve make_id(source_file) through the cache, computing lazily on first use.
  const auto file_id_for = [&](const std::string& source_file) -> const std::string& {
    auto it = file_id_by_source.find(source_file);
    if (it == file_id_by_source.end()) {
      it = file_id_by_source.emplace(source_file, make_id(source_file)).first;
    } else if (it->second.empty() && !source_file.empty()) {
      it->second = make_id(source_file);
    }
    return it->second;
  };

  // Memoize make_id(callee_label): the same callee name recurs across many calls
  // (every invocation of the same function), so normalizing it once per distinct
  // label removes the bulk of the per-call utf8proc work while keeping the key
  // byte-identical to the original make_id(callee_label).
  std::unordered_map<std::string, std::string> callee_key_cache;
  const auto callee_key_for = [&](const std::string& label) -> const std::string& {
    auto it = callee_key_cache.find(label);
    if (it == callee_key_cache.end()) {
      it = callee_key_cache.emplace(label, make_id(label)).first;
    }
    return it->second;
  };

  // Import evidence per caller file id: the set of symbol targets it imports and
  // the set of module (file) targets it imports from. This only grades the
  // confidence of a resolved call (EXTRACTED when the callee was imported,
  // INFERRED otherwise) — it never resolves a call. Resolution is purely by
  // name, exactly as Graphify does: a call is wired by same-file declaration or
  // by a project-wide unique label, and an import that is ambiguous or missing
  // changes only the confidence, not whether the edge exists.
  std::unordered_map<std::string, std::unordered_set<std::string>> imported_symbols;
  std::unordered_map<std::string, std::unordered_set<std::string>> imported_modules;
  for (const auto& edge : graph.edges) {
    if (edge.relation == "imports" || edge.relation == "re_exports") {
      imported_symbols[edge.source].insert(edge.target);
    } else if (edge.relation == "imports_from") {
      imported_modules[edge.source].insert(edge.target);
    }
  }

  // Seed the dedupe set with existing edges so resolution is O(calls), not
  // O(calls * edges).
  std::unordered_set<std::string> seen_edges;
  seen_edges.reserve(graph.edges.size());
  for (const auto& edge : graph.edges) {
    seen_edges.insert(edge_key(edge));
  }

  CallResolution tally;
  for (const auto& raw_call : raw_calls) {
    if (raw_call.callee_label.empty() || is_builtin_global(raw_call.callee_label)) {
      continue;
    }
    // The caller must resolve to a real graph node. Module-level calls with no
    // enclosing symbol are skipped rather than attached to a synthetic file id
    // that would dangle (no consumer can render an edge to a missing node).
    if (raw_call.caller_id.empty() || !node_ids.contains(raw_call.caller_id)) {
      continue;
    }
    // Counted from here: a call with a real caller and a callee name that is not a
    // language built-in is a call this resolver is answerable for.
    ++tally.total;
    const auto& key = callee_key_for(raw_call.callee_label);
    const auto& caller_file = source_file_by_id[raw_call.caller_id];

    std::string target_id;
    auto confidence = Confidence::Extracted;
    bool same_file_hit = false;
    // Sibling targets beyond target_id, filled only for an overload set: the
    // call edges to EVERY member, because any of them may be the callee.
    std::span<const std::string> overload_rest;

    // Resolve an overload set: target the first declaration and remember the
    // rest, all graded INFERRED. Which member a call means cannot be known
    // without types, so the edges assert possibility, not certainty — one
    // arbitrary pick would leave the other overloads invisible to reverse
    // dependency walks (issue #52).
    //
    // Dropping instead would be a regression. Before labels became bare
    // names, an overload set collapsed onto one node and the call resolved,
    // so `add(int)` / `add(String)` in one Java class had working call
    // edges; making the overloads distinct nodes must not take those away.
    // Overloading is idiomatic in Java, C#, Kotlin, Scala, Groovy and C++.
    const auto resolve_overload_set = [&](const std::string& declaring_file) {
      const auto members = overload_declarations.find(declaring_file + "\n" + key);
      if (members == overload_declarations.end()) {
        return false;
      }
      target_id = members->second.front();
      overload_rest = std::span(members->second).subspan(1);
      confidence = Confidence::Inferred;
      ++tally.resolved_overload_first;
      return true;
    };

    // 1. A symbol declared in the caller's own file (local helper, sibling fn).
    if (const auto file = local_by_file.find(caller_file); file != local_by_file.end()) {
      if (const auto slot = file->second.find(key); slot != file->second.end()) {
        // A member call must never bind to a free function — tier 2b already
        // enforces this project-wide, and the same-file tier has to as well:
        // Rust test files conventionally name a test fn after the method it
        // exercises (`fn blocking_acquire()` testing `sem.blocking_acquire()`
        // in tokio's sync_semaphore.rs), so the same-file name match is the
        // caller's own sibling test, not the receiver's method. Skip the
        // non-method binding and let the method-only tier resolve it.
        const bool member_call_on_non_method =
            raw_call.is_member_call && !slot->second.empty() && !method_node_ids.contains(slot->second);
        if (!member_call_on_non_method) {
          target_id = slot->second;
          if (target_id.empty()) {
            // An empty slot marks an overload set declared in the caller's file.
            resolve_overload_set(caller_file);
          }
          if (target_id.empty()) {
            ++tally.dropped_ambiguous;
            continue;
          }
          same_file_hit = true;
        }
      }
    }

    // 2. A project-wide unique label. An unknown name resolves to nothing; an
    //    ambiguous one is dropped UNLESS every candidate lives in one file — a
    //    true overload set (idiomatic in C++, Java, C#), which resolves exactly
    //    as the same-file tier does. A collision spanning files stays dropped:
    //    picking a module would be a guess, and that exactly-one-module rule is
    //    what keeps cross-file calls honest. Member calls (`obj.method()`) are
    //    excluded: the bare property name has no import evidence and collides
    //    with any top-level function of the same name, so it stays scoped to
    //    the caller's own file (handled above).
    if (target_id.empty() && !raw_call.is_member_call) {
      const auto targets = index.find(key);
      if (targets == index.end()) {
        ++tally.dropped_unknown;
        continue;
      }
      if (targets->second.size() != 1) {
        const auto first_file = source_file_by_id.find(targets->second.front());
        const bool single_file_overload_set =
            first_file != source_file_by_id.end() && !first_file->second.empty() &&
            std::all_of(targets->second.begin(), targets->second.end(), [&](const std::string& id) {
              const auto it = source_file_by_id.find(id);
              return it != source_file_by_id.end() && it->second == first_file->second;
            });
        if (!single_file_overload_set || !resolve_overload_set(first_file->second)) {
          ++tally.dropped_ambiguous;
          continue;
        }
      } else {
        target_id = targets->second.front();
        // Grade confidence: EXTRACTED when the caller's file actually imports
        // the resolved symbol or its module, INFERRED when it is only a name
        // match.
        const auto& caller_file_id = file_id_for(caller_file);
        const auto& callee_file_id = file_id_for(source_file_by_id[target_id]);
        const auto symbols = imported_symbols.find(caller_file_id);
        const auto modules = imported_modules.find(caller_file_id);
        const bool has_import_evidence =
            (symbols != imported_symbols.end() && symbols->second.contains(target_id)) ||
            (modules != imported_modules.end() && modules->second.contains(callee_file_id));
        confidence = has_import_evidence ? Confidence::Extracted : Confidence::Inferred;
      }
    }

    // 2b. A member call that missed its own file resolves project-wide only
    //     against METHODS, under the same exactly-one-candidate rule, and only
    //     ever graded INFERRED (no import evidence can exist for a bare property
    //     name). This is what connects `r.Match(...)` in a test file to the
    //     method's declaration (issue #44) without letting `.map()` bind to a
    //     free function that happens to share the name.
    bool member_method_hit = false;
    if (target_id.empty() && raw_call.is_member_call) {
      if (const auto methods = method_index.find(key);
          methods != method_index.end() && methods->second.size() == 1) {
        target_id = methods->second.front();
        confidence = Confidence::Inferred;
        member_method_hit = true;
      }
    }

    if (target_id.empty()) {
      // A member call that missed its own file and did not uniquely name a
      // method: the receiver type is unknown, so no wider guess applies.
      ++tally.dropped_unknown;
      continue;
    }
    if (target_id == raw_call.caller_id) {
      ++tally.dropped_self;
      continue;
    }
    if (same_file_hit) {
      ++tally.resolved_same_file;
    } else if (member_method_hit) {
      ++tally.resolved_member_method;
    } else {
      ++tally.resolved_project_unique;
    }
    Edge edge{
        .source = raw_call.caller_id,
        .target = target_id,
        .relation = std::string(kCallRelation),
        .confidence = confidence,
    };
    if (seen_edges.insert(edge_key(edge)).second) {
      graph.edges.push_back(std::move(edge));
    }
    // The remaining members of an overload set: the call may mean any of them,
    // so each gets the same INFERRED edge (dedupe applies per target).
    for (const auto& sibling : overload_rest) {
      if (sibling == raw_call.caller_id) {
        continue;
      }
      Edge sibling_edge{
          .source = raw_call.caller_id,
          .target = sibling,
          .relation = std::string(kCallRelation),
          .confidence = Confidence::Inferred,
      };
      if (seen_edges.insert(edge_key(sibling_edge)).second) {
        graph.edges.push_back(std::move(sibling_edge));
      }
    }
  }
  if (outcomes != nullptr) {
    *outcomes = tally;
  }
}

void resolve_raw_relations(GraphSnapshot& graph, std::span<const RawRelation> raw_relations) {
  std::unordered_set<std::string> node_ids;
  std::unordered_map<std::string, std::string> label_by_id;
  node_ids.reserve(graph.nodes.size());

  // Per-file declared symbols (label -> id, empty when ambiguous), the same
  // index used for same-file call resolution. Heritage relations may resolve a
  // base type to a declaration in the same file.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> local_by_file;
  for (const auto& node : graph.nodes) {
    node_ids.insert(node.id);
    label_by_id.emplace(node.id, node.label);
    if (node.source_file.empty()) {
      continue;
    }
    if (node.kind != "function" && node.kind != "class" && node.kind != "type" && node.kind != "variable") {
      continue;
    }
    auto& by_label = local_by_file[node.source_file];
    const auto [slot, inserted] = by_label.emplace(make_id(node.label), node.id);
    if (!inserted && slot->second != node.id) {
      slot->second.clear();
    }
  }

  // Per-file imported names (file id -> label -> imported target id), built from
  // the import/re_export edges left by resolve_imports. This is the import-alias
  // map every relation target is resolved through.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> imported_by_file;
  for (const auto& edge : graph.edges) {
    if (edge.relation != "imports" && edge.relation != "re_exports") {
      continue;
    }
    if (const auto label = label_by_id.find(edge.target); label != label_by_id.end()) {
      imported_by_file[edge.source].emplace(make_id(label->second), edge.target);
    }
  }

  // C/C++ `#include` imports a whole file, not named symbols, so a referenced
  // type (a base class, a parameter type) is declared in an included *file*
  // rather than imported by name. Map each importer file to the source paths of
  // the files it includes, so a relation target can resolve to a declaration in
  // any of them.
  std::unordered_map<std::string, std::string> file_source_by_id;
  for (const auto& node : graph.nodes) {
    if (node.kind == "file") {
      file_source_by_id.emplace(node.id, node.source_file);
    }
  }
  std::unordered_map<std::string, std::vector<std::string>> included_files_by_file;
  for (const auto& edge : graph.edges) {
    if (edge.relation != "imports" && edge.relation != "re_exports") {
      continue;
    }
    if (const auto src = file_source_by_id.find(edge.target); src != file_source_by_id.end()) {
      included_files_by_file[edge.source].push_back(src->second);
    }
  }

  std::unordered_set<std::string> seen_edges;
  seen_edges.reserve(graph.edges.size());
  for (const auto& edge : graph.edges) {
    seen_edges.insert(edge_key(edge));
  }

  // Memoize make_id of the recurring per-relation strings (target type names and
  // source paths both repeat heavily across relations). Each memo returns the
  // exact value the inline make_id call produced, so resolution is unchanged.
  std::unordered_map<std::string, std::string> target_key_cache;
  std::unordered_map<std::string, std::string> source_file_id_cache;
  const auto memo = [](std::unordered_map<std::string, std::string>& cache,
                       const std::string& s) -> const std::string& {
    auto it = cache.find(s);
    if (it == cache.end()) {
      it = cache.emplace(s, make_id(s)).first;
    }
    return it->second;
  };

  for (const auto& relation : raw_relations) {
    if (relation.source_id.empty() || relation.target_label.empty() || !node_ids.contains(relation.source_id)) {
      continue;
    }
    const auto& key = memo(target_key_cache, relation.target_label);
    // make_id(relation.source_file) was computed twice per relation below; the
    // file-id lookups (imported_by_file / included_files_by_file) are both keyed
    // by it, so normalize the source path once and reuse the result.
    const auto& source_file_id = memo(source_file_id_cache, relation.source_file);

    std::string target_id;
    // 1. The type the source file imports (the canonical resolution path).
    if (const auto file = imported_by_file.find(source_file_id); file != imported_by_file.end()) {
      if (const auto slot = file->second.find(key); slot != file->second.end()) {
        target_id = slot->second;
      }
    }
    // 1b. The type is declared in a file the source file #includes (C/C++
    //     whole-file import). Resolve against declarations in each included file.
    if (target_id.empty()) {
      if (const auto inc = included_files_by_file.find(source_file_id); inc != included_files_by_file.end()) {
        for (const auto& included_source : inc->second) {
          const auto file = local_by_file.find(included_source);
          if (file == local_by_file.end()) {
            continue;
          }
          const auto slot = file->second.find(key);
          if (slot != file->second.end() && !slot->second.empty()) {
            target_id = slot->second;
            break;
          }
        }
      }
    }
    // 2. Heritage may also resolve to a same-file declaration (`class A extends
    //    B` where B is declared in the same module and not imported).
    if (target_id.empty() && relation.allow_same_file) {
      if (const auto file = local_by_file.find(relation.source_file); file != local_by_file.end()) {
        if (const auto slot = file->second.find(key); slot != file->second.end()) {
          target_id = slot->second;
        }
      }
    }

    if (target_id.empty() || target_id == relation.source_id) {
      continue;  // unresolvable (third-party / same-file reference) or self-edge
    }
    Edge edge{
        .source = relation.source_id,
        .target = target_id,
        .relation = relation.relation,
        .confidence = Confidence::Extracted,
    };
    if (!relation.context.empty()) {
      edge.properties.emplace("context", relation.context);
    }
    if (seen_edges.insert(edge_key(edge)).second) {
      graph.edges.push_back(std::move(edge));
    }
  }
}


void resolve_interface_dispatch(GraphSnapshot& graph, std::span<const RawCall> raw_calls) {
  // Interface method nodes, grouped by their owning interface (the `method`
  // edge the extractor emitted alongside the tagged node).
  std::unordered_map<std::string, const Node*> nodes_by_id;
  for (const auto& node : graph.nodes) {
    nodes_by_id.emplace(node.id, &node);
  }
  const auto is_iface_method = [&](const std::string& id) {
    const auto it = nodes_by_id.find(id);
    if (it == nodes_by_id.end()) {
      return false;
    }
    const auto tag = it->second->properties.find("interface_method");
    return tag != it->second->properties.end() && tag->second == "true";
  };

  // interface id -> (method name key -> interface method node id)
  std::map<std::string, std::map<std::string, std::string>> iface_methods;
  // concrete type id -> (method name key -> method node ids)
  std::map<std::string, std::map<std::string, std::vector<std::string>>> type_methods;
  for (const auto& edge : graph.edges) {
    if (edge.relation == "method" && is_iface_method(edge.target)) {
      iface_methods[edge.source].emplace(make_id(nodes_by_id.at(edge.target)->label), edge.target);
    } else if (edge.relation == "method_of") {
      // method -> receiver type (Go); invert into the type's method set.
      if (const auto method = nodes_by_id.find(edge.source); method != nodes_by_id.end()) {
        type_methods[edge.target][make_id(method->second->label)].push_back(edge.source);
      }
    } else if (edge.relation == "method" && !is_iface_method(edge.target)) {
      // class -> method (Python/TS class methods) participates the same way.
      if (const auto method = nodes_by_id.find(edge.target); method != nodes_by_id.end()) {
        type_methods[edge.source][make_id(method->second->label)].push_back(edge.target);
      }
    }
  }
  if (iface_methods.empty()) {
    return;
  }

  std::unordered_set<std::string> seen_edges;
  seen_edges.reserve(graph.edges.size());
  for (const auto& edge : graph.edges) {
    seen_edges.insert(edge_key(edge));
  }
  const auto add_edge = [&](std::string source, std::string target, const char* relation,
                            Confidence confidence) {
    Edge edge{.source = std::move(source), .target = std::move(target), .relation = relation,
              .confidence = confidence};
    if (seen_edges.insert(edge_key(edge)).second) {
      graph.edges.push_back(std::move(edge));
    }
  };

  // implements: T satisfies I when I's method-name set is a subset of T's.
  // Name-only satisfaction (no signatures — deliberately structural-lite);
  // the exactly-one guard below is what keeps call rescue honest, not this.
  for (const auto& [iface_id, promised] : iface_methods) {
    for (const auto& [type_id, methods] : type_methods) {
      if (type_id == iface_id || promised.empty()) {
        continue;
      }
      const bool satisfies = std::ranges::all_of(
          promised, [&](const auto& entry) { return methods.contains(entry.first); });
      if (!satisfies) {
        continue;
      }
      add_edge(type_id, iface_id, "implements", Confidence::Inferred);
      for (const auto& [name_key, iface_method_id] : promised) {
        for (const auto& impl_id : methods.at(name_key)) {
          add_edge(iface_method_id, impl_id, "dispatches_to", Confidence::Inferred);
        }
      }
    }
  }

  // Trait-scoped dispatch (issue #60). The subset rule above misses `impl Trait
  // for Type` blocks whenever the trait promises methods the impl does not
  // override -- default/provided methods, and the async ext-trait plumbing where
  // a concrete type implements only `poll_read` while the contract carries a
  // dozen provided combinators. The Rust extractor emits an `impl_trait` edge
  // (impl method -> the trait node it names) for exactly this: bind the method to
  // its declared contract's same-named promise, independent of the subset check.
  // Its owning type is recorded so `implements` still lands for the reverse walk.
  std::unordered_map<std::string, std::string> type_of_method;
  for (const auto& edge : graph.edges) {
    if (edge.relation == "method_of") {
      type_of_method.emplace(edge.source, edge.target);
    }
  }
  for (const auto& edge : graph.edges) {
    if (edge.relation != "impl_trait") {
      continue;
    }
    const auto iface = iface_methods.find(edge.target);
    if (iface == iface_methods.end()) {
      continue;  // the named trait has no materialized contract methods
    }
    const auto method = nodes_by_id.find(edge.source);
    if (method == nodes_by_id.end()) {
      continue;
    }
    const auto promise = iface->second.find(make_id(method->second->label));
    if (promise == iface->second.end()) {
      continue;  // this method is not one the trait promises by name
    }
    add_edge(promise->second, edge.source, "dispatches_to", Confidence::Inferred);
    if (const auto owner = type_of_method.find(edge.source); owner != type_of_method.end()) {
      add_edge(owner->second, edge.target, "implements", Confidence::Inferred);
    }
  }

  // Rescue member calls that name exactly one interface method project-wide:
  // `r.Match(...)` was ambiguous among 8 concrete `Match`es on gorilla/mux, but
  // `matcher.Match` is the single interface contract they satisfy — the call
  // binds to the contract, dispatches_to carries it to the implementations.
  std::map<std::string, std::string> unique_iface_method_by_name;
  std::unordered_set<std::string> ambiguous_names;
  for (const auto& [iface_id, promised] : iface_methods) {
    for (const auto& [name_key, method_id] : promised) {
      if (ambiguous_names.contains(name_key)) {
        continue;
      }
      const auto [slot, inserted] = unique_iface_method_by_name.emplace(name_key, method_id);
      if (!inserted && slot->second != method_id) {
        unique_iface_method_by_name.erase(slot);
        ambiguous_names.insert(name_key);
      }
    }
  }
  std::unordered_set<std::string> node_ids;
  node_ids.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    node_ids.insert(node.id);
  }
  for (const auto& raw_call : raw_calls) {
    if (!raw_call.is_member_call || raw_call.caller_id.empty() || !node_ids.contains(raw_call.caller_id)) {
      continue;
    }
    const auto slot = unique_iface_method_by_name.find(make_id(raw_call.callee_label));
    if (slot == unique_iface_method_by_name.end() || slot->second == raw_call.caller_id) {
      continue;
    }
    add_edge(raw_call.caller_id, slot->second, kCallRelation.data(), Confidence::Inferred);
  }
}

}  // namespace cgraph
