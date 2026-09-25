#include "cgraph/graph_builder.hpp"

#include "cgraph/detect.hpp"
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

std::unordered_map<std::string, std::string> file_node_ids(const GraphSnapshot& graph) {
  std::unordered_map<std::string, std::string> ids;
  for (const auto& node : graph.nodes) {
    if (node.kind == "file" && !node.source_file.empty()) {
      ids.emplace(node.source_file, node.id);
    }
  }
  return ids;
}

namespace {

constexpr std::string_view kCallRelation = "CALLS";

[[nodiscard]] std::string edge_key(const Edge& edge) {
  return edge.source + "\n" + edge.relation + "\n" + edge.target;
}

// Member names that every standard library defines on its containers, strings,
// iterators, smart pointers and option types. A member call to one of these
// with an unknown receiver (`v.size()`, `m.find(k)`, `opt.value()`) is far more
// likely to be the library's member than the project's, so the method-only
// project-wide tier refuses to bind it: on CGraph's own source the one project
// method named `size` received 141 CALLS edges, one per `.size()` in the tree,
// and `find` received 68, every one a false dependent in `impact`. A same-file
// declaration (tier 1) is evidence and still binds; so does a receiver that
// names the class (`Stats::size()`, tier 2a), but only where the language config
// fills `receiver_label` through `call_receiver_field` -- java_config alone
// today -- so for the C family the same-file tier is the only one left.
// Deliberately excludes names a project plausibly owns (`open`, `close`,
// `read`, `write`, `get`, `set`, `add`, `apply`, `merge`, `load`, `store`):
// losing those edges would cost more recall than the precision is worth.
// Keys are make_id-normalized, so `.Count()` (Go, C#) and `.count()` both match.
[[nodiscard]] bool is_library_member_key(const std::string& key) {
  static const std::unordered_set<std::string> keys = [] {
    static constexpr std::string_view names[] = {
      // containers and strings: C++, Java, JS, Python, Rust
      "size", "length", "len", "empty", "is_empty", "isEmpty", "clear", "capacity", "reserve", "resize",
      "begin", "end", "cbegin", "cend", "rbegin", "rend", "front", "back", "first", "second", "at",
      "count", "contains", "has", "insert", "erase", "emplace", "emplace_back", "emplace_front",
      "try_emplace", "insert_or_assign", "push_back", "pop_back", "push_front", "pop_front", "append",
      "swap", "data", "c_str", "str", "substr", "substring", "find", "rfind", "find_first_of",
      "find_last_of", "lower_bound", "upper_bound", "equal_range", "starts_with", "ends_with",
      "startswith", "endswith", "keys", "values", "entries", "items", "iter", "into_iter", "collect",
      "indexOf", "lastIndexOf", "includes", "slice", "splice", "forEach", "for_each", "toString",
      "hashCode", "equals", "to_string", "to_owned", "as_str", "as_ref", "as_mut", "clone",
      // optionals, results, smart pointers, locks
      "value", "has_value", "value_or", "unwrap", "unwrap_or", "expect", "is_some", "is_none", "is_ok",
      "is_err", "reset", "release", "lock", "unlock", "try_lock",
    };
    std::unordered_set<std::string> normalized;
    for (const auto name : names) {
      normalized.insert(make_id(name));
    }
    return normalized;
  }();
  return keys.contains(key);
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
    // First occurrence wins here too, matching the node it belongs to.
    graph.fingerprints.insert(fragment.fingerprints.begin(), fragment.fingerprints.end());
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
  graph.fingerprints.insert(fragment.fingerprints.begin(), fragment.fingerprints.end());
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

  // What each file declares, by label: "<source path>\n<label>" -> node id, empty
  // when the file declares that label more than once. An import of a name binds
  // to the declaration only when it is unique.
  std::unordered_map<std::string, std::string> declared_by_file_label;
  for (const auto& node : graph.nodes) {
    if (node.kind != "function" && node.kind != "class" && node.kind != "type" && node.kind != "variable") {
      continue;
    }
    const auto [slot, inserted] = declared_by_file_label.emplace(node.source_file + "\n" + node.label, node.id);
    if (!inserted && slot->second != node.id) {
      slot->second.clear();
    }
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
      // The symbol the import names, when the resolved file declares exactly one
      // by that label; a file that declares it twice (an overload set) or not at
      // all keeps the import on the file itself.
      const auto declared = declared_by_file_label.find(source_of_file[*file_id] + "\n" + node.label);
      const bool unique = declared != declared_by_file_label.end() && !declared->second.empty();
      remap[node.id] = unique ? declared->second : *file_id;
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
  // An aliased import (`import { config as configModule }`) is referred to by
  // its alias in the importing file. The stub carried it; the relinked edge
  // keeps it, so build_relation_scopes can bind the name the file actually uses.
  std::unordered_map<std::string, std::string> alias_of;
  for (const auto& node : graph.nodes) {
    if (!removed.contains(node.id)) {
      continue;
    }
    if (const auto alias = node.properties.find("alias"); alias != node.properties.end()) {
      alias_of.emplace(node.id, alias->second);
    }
  }
  std::unordered_set<std::string> seen_edges;
  std::vector<Edge> rewritten;
  rewritten.reserve(graph.edges.size());
  for (auto edge : graph.edges) {
    if (dropped.contains(edge.source) || dropped.contains(edge.target)) {
      continue;  // edge to a dropped third-party import
    }
    if (const auto alias = alias_of.find(edge.target); alias != alias_of.end()) {
      edge.properties.emplace("alias", alias->second);
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
  std::unordered_map<std::string, std::string> method_name_key_by_id;
  for (const auto& node : graph.nodes) {
    const auto tagged = node.properties.find("method");
    if (method_ids.contains(node.id) || (tagged != node.properties.end() && tagged->second == "true")) {
      auto name_key = make_id(node.label);
      method_index[name_key].push_back(node.id);
      method_node_ids.insert(node.id);
      method_name_key_by_id.emplace(node.id, std::move(name_key));
    }
  }
  // owning class id -> (normalized method name -> that class's methods). A
  // member call whose receiver NAMES a class resolves against this instead of
  // the project-wide method index: `XML.toJSONObject(s)` says which class it
  // means, so it must not be dropped merely because seven other files also
  // declare a `toJSONObject`.
  std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>
      methods_by_owner;
  // The class node that owns each method, for checking a class-qualified callee
  // (`proj::Stats::size()`) against the resolved method's owner and that owner's
  // own namespace.
  std::unordered_map<std::string, std::string> owner_id_by_method;
  for (const auto& edge : graph.edges) {
    if (edge.relation != "method") {
      continue;
    }
    owner_id_by_method.emplace(edge.target, edge.source);
    const auto name_key = method_name_key_by_id.find(edge.target);
    if (name_key != method_name_key_by_id.end()) {
      methods_by_owner[edge.source][name_key->second].push_back(edge.target);
    }
  }
  std::unordered_set<std::string> node_ids;
  std::unordered_map<std::string, std::string> source_file_by_id;
  std::unordered_map<std::string, std::string> label_by_id;
  // Declared namespace scope per node (`scope` property, C-family only today).
  std::unordered_map<std::string, std::string> scope_by_id;
  // The outermost segment of every declared scope: the namespace roots this
  // project actually owns. A qualifier rooted anywhere else (`std::find`,
  // `fmt::format`, `boost::algorithm::trim`, `QString::number`) names a scope no
  // declaration here can be in, which is what makes it evidence against every
  // candidate rather than only against the ones that recorded something.
  std::unordered_set<std::string> project_scope_roots;
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
  // The confidence grading below looks caller/callee files up by source path per
  // raw call; the file node is the only thing that knows a path's id.
  const auto file_id_by_source = file_node_ids(graph);
  for (const auto& node : graph.nodes) {
    node_ids.insert(node.id);
    source_file_by_id.emplace(node.id, node.source_file);
    label_by_id.emplace(node.id, node.label);
    if (const auto scope = node.properties.find("scope"); scope != node.properties.end()) {
      const std::string_view text = scope->second;
      const auto separator = text.find("::");
      if (const auto root = text.substr(0, separator); root != "(anonymous)" && !root.empty()) {
        project_scope_roots.emplace(root);
      }
      scope_by_id.emplace(node.id, scope->second);
    }
    if (node.source_file.empty()) {
      continue;
    }
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
  // The file node id for a source path; empty when the path has no file node.
  static const std::string no_file_id;
  const auto file_id_for = [&](const std::string& source_file) -> const std::string& {
    const auto it = file_id_by_source.find(source_file);
    return it == file_id_by_source.end() ? no_file_id : it->second;
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
  // the set of module (file) targets it imports from. This grades the confidence
  // of a resolved call (EXTRACTED when the callee was imported, INFERRED
  // otherwise), and — for a SYMBOL import only — narrows an otherwise ambiguous
  // candidate set to the declaration the caller actually named (issue #70).
  // A module import stays confidence-only: `import pkg` does not single out a
  // declaration, and the bare-name call it would rescue is spelled `pkg.fn()`,
  // a member call this tier never sees.
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
    // Survivors of the scope gate below, when it narrows an overload set.
    std::vector<std::string> gated_rest;
    bool overload_counted = false;
    // The call carried a qualifier the project gave nothing to check it
    // against; counted only once the call actually produces an edge.
    bool qualifier_unchecked = false;

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
      overload_counted = true;
      return true;
    };

    // Narrow an ambiguous candidate set to the declarations the caller's file
    // imports by name, then re-apply the ordinary rule to the survivors: one
    // resolves (EXTRACTED — the import is the proof), several in one file are an
    // imported overload set, and anything else stays ambiguous. Two imports of
    // the same name cannot both be what the call meant, so they resolve to
    // nothing exactly as two unimported declarations do (issue #70).
    const auto resolve_imported_candidate = [&](const std::vector<std::string>& candidates) {
      const auto symbols = imported_symbols.find(file_id_for(caller_file));
      if (symbols == imported_symbols.end()) {
        return false;
      }
      std::vector<std::string> imported;
      for (const auto& candidate : candidates) {
        if (symbols->second.contains(candidate)) {
          imported.push_back(candidate);
        }
      }
      if (imported.empty()) {
        return false;
      }
      if (imported.size() == 1) {
        target_id = imported.front();
        confidence = Confidence::Extracted;
        return true;
      }
      // Several imported declarations of one name: an overload set is the only
      // shape that can legitimately mean all of them, and only when they share a
      // file. A collision spanning files stays dropped.
      const auto first_file = source_file_by_id.find(imported.front());
      if (first_file == source_file_by_id.end() || first_file->second.empty()) {
        return false;
      }
      const bool one_file = std::all_of(imported.begin(), imported.end(), [&](const std::string& id) {
        const auto it = source_file_by_id.find(id);
        return it != source_file_by_id.end() && it->second == first_file->second;
      });
      return one_file && resolve_overload_set(first_file->second);
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
          // The name collides across files, but the caller may have said which
          // declaration it meant: an `imports`/`re_exports` edge names a target
          // id outright. That is evidence, not a guess — the same standard the
          // receiver tier applies to `XML.toJSONObject` — so narrow the
          // candidates to the ones this file imports and re-apply the ordinary
          // rule to what is left. One survivor resolves; several stay dropped,
          // because an import that names two candidates picks neither (#70).
          if (!resolve_imported_candidate(targets->second)) {
            ++tally.dropped_ambiguous;
            continue;
          }
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
    // 2a. A member call whose receiver is a bare identifier naming a class in
    //     the project (`XML.toJSONObject(s)` — a static call, or an instance
    //     whose name matches its type). The receiver is EVIDENCE, not a guess:
    //     it says which class owns the method, so the lookup is scoped to that
    //     class instead of the project-wide method index. Without this, a name
    //     any other file also declares is dropped as ambiguous even though the
    //     call site named its class outright — which is why every
    //     `XML.toJSONObject` call in stleary/JSON-java produced no edge at all
    //     (`toJSONObject` is declared in eight files there).
    bool receiver_scoped_hit = false;
    if (target_id.empty() && raw_call.is_member_call && !raw_call.receiver_label.empty()) {
      const auto receiver_key = make_id(raw_call.receiver_label);
      // Resolve the receiver name to the declaration that actually owns a
      // method by this name. Requiring the receiver label to be globally unique
      // is too strict: `XML` names both the class and its same-named
      // constructor node, so a uniqueness test finds two candidates and gives
      // up. Owning the called method is the sharper test, and it is the
      // property we actually need. Two owners of the same name would be a real
      // ambiguity and stay dropped.
      std::vector<std::string> receiver_candidates;
      if (const auto file = local_by_file.find(caller_file); file != local_by_file.end()) {
        if (const auto slot = file->second.find(receiver_key);
            slot != file->second.end() && !slot->second.empty()) {
          receiver_candidates.push_back(slot->second);
        }
      }
      if (receiver_candidates.empty()) {
        if (const auto candidates = index.find(receiver_key); candidates != index.end()) {
          receiver_candidates = candidates->second;
        }
      }
      // The receiver text must match the declaration's name EXACTLY, case and
      // all. make_id folds case, which would also bind an instance named after
      // its type (`jsonWriter.setStrictness()` -> class `JsonWriter`). That is
      // a naming convention, not evidence: it is usually right, but it equally
      // binds a `writer` of some other type to an unrelated class `Writer`, and
      // a wrong CALLS edge is a false dependent in `impact`. Requiring exact
      // case keeps only the case this tier can actually prove — a receiver that
      // literally names the declaration, i.e. a static call.
      //
      // Measured on the two Java benchmarks: exact-case is worth the whole
      // JSON-java win (test reachability 0.853 -> 0.929, identical to the
      // case-folded variant) with 841 fewer edges. Case-folding additionally
      // lifts gson 0.839 -> 0.879, but on convention rather than proof; that is
      // a separate, opt-in change if it is ever wanted.
      std::string receiver_id;
      std::size_t owning = 0;
      for (const auto& candidate : receiver_candidates) {
        const auto owner = methods_by_owner.find(candidate);
        if (owner == methods_by_owner.end() || !owner->second.contains(key)) {
          continue;
        }
        const auto label = label_by_id.find(candidate);
        if (label == label_by_id.end() || label->second != raw_call.receiver_label) {
          continue;
        }
        ++owning;
        receiver_id = candidate;
      }
      if (owning == 1) {
        if (const auto owner = methods_by_owner.find(receiver_id); owner != methods_by_owner.end()) {
          if (const auto named = owner->second.find(key); named != owner->second.end() &&
                                                          !named->second.empty()) {
            // One method, or an overload set on this one class — issue #52's
            // rule applies within the class exactly as it does within a file.
            target_id = named->second.front();
            overload_rest = std::span(named->second).subspan(1);
            confidence = Confidence::Inferred;
            receiver_scoped_hit = true;
          }
        }
      }
    }

    bool member_method_hit = false;
    if (target_id.empty() && raw_call.is_member_call) {
      if (const auto methods = method_index.find(key); methods != method_index.end()) {
        if (is_library_member_key(key)) {
          // The bare name is one every standard library defines: with the
          // receiver unknown, a project method of that name is not evidence.
          // Counted only here, where a candidate existed to refuse; a name with
          // no project method at all is an ordinary unknown below.
          ++tally.dropped_library_member;
          continue;
        }
        if (methods->second.size() == 1) {
          target_id = methods->second.front();
          confidence = Confidence::Inferred;
          member_method_hit = true;
        } else {
          // Several methods share the name. When they all live in ONE file they
          // are an overload set on a single type — idiomatic in Java, C#, C++,
          // Kotlin — and issue #52's rule applies unchanged: without types any
          // member may be the callee, so edge to ALL of them rather than drop
          // the call. Tier 2 has always done this for non-member calls; a member
          // call needs it just as much, and without it every call to an
          // overloaded method (`tokener.nextTo(char)` / `nextTo(String)`)
          // silently loses its edge. Candidates spread across files are a real
          // cross-type collision and stay dropped — picking one would be a guess.
          const auto first_file = source_file_by_id.find(methods->second.front());
          const bool single_file_overload_set =
              first_file != source_file_by_id.end() && !first_file->second.empty() &&
              std::all_of(methods->second.begin(), methods->second.end(),
                          [&](const std::string& id) {
                            const auto it = source_file_by_id.find(id);
                            return it != source_file_by_id.end() &&
                                   it->second == first_file->second;
                          });
          if (single_file_overload_set && resolve_overload_set(first_file->second)) {
            member_method_hit = true;
          }
        }
      }
    }

    if (target_id.empty()) {
      // A member call that missed its own file and did not uniquely name a
      // method: the receiver type is unknown, so no wider guess applies.
      ++tally.dropped_unknown;
      continue;
    }
    // A qualified callee names its scope outright, and that is evidence about
    // which declaration the call means -- evidence that can only ever REFUSE a
    // candidate the tiers already produced. A candidate survives when the
    // qualifier's segments are a suffix of its `scope` segments
    // (`detail::helper()` from inside `proj` matches `proj::detail`;
    // `other::detail::helper()` does not), with anonymous namespaces transparent
    // because a qualified name from the same translation unit sees through
    // them; or when it is a method of a class bearing the last segment whose own
    // scope carries the rest (`proj::Stats::size()`).
    //
    // The gate runs where the qualifier has something to contradict. A root the
    // project declares nowhere (`std`, `fmt`, `boost`, `QString`) contradicts
    // every candidate by itself -- no declaration here is in that scope. Under a
    // root the project does own, at least one candidate must record a scope or
    // an owning class for the qualifier to check against; when none does, the
    // qualifier is checked against nothing, so refusing would drop a real edge
    // (a project scope the extractor never stamped) to buy no precision. Those
    // bindings are the ones the qualifier could not confirm, and they are
    // counted as `resolved_qualifier_unchecked`.
    if (!raw_call.qualifier.empty()) {
      const auto split_scope = [](std::string_view text) {
        std::vector<std::string_view> segments;
        while (!text.empty()) {
          const auto separator = text.find("::");
          const auto segment = text.substr(0, separator);
          if (segment != "(anonymous)") {
            segments.push_back(segment);
          }
          if (separator == std::string_view::npos) {
            break;
          }
          text.remove_prefix(separator + 2);
        }
        return segments;
      };
      const auto wanted = split_scope(raw_call.qualifier);
      const auto declared_scope_of = [&](const std::string& id) -> std::string_view {
        const auto scope = scope_by_id.find(id);
        return scope == scope_by_id.end() ? std::string_view{} : std::string_view(scope->second);
      };
      const auto is_suffix = [&](std::string_view declared, std::span<const std::string_view> prefix) {
        const auto have = split_scope(declared);
        if (prefix.size() > have.size()) {
          return false;
        }
        return std::equal(prefix.rbegin(), prefix.rend(), have.rbegin());
      };
      const auto records_scope = [&](const std::string& id) {
        return scope_by_id.contains(id) || owner_id_by_method.contains(id);
      };
      const auto in_scope = [&](const std::string& id) {
        if (!wanted.empty() && is_suffix(declared_scope_of(id), wanted)) {
          return true;
        }
        const auto owner = owner_id_by_method.find(id);
        if (owner == owner_id_by_method.end() || wanted.empty()) {
          return false;
        }
        const auto owner_label = label_by_id.find(owner->second);
        if (owner_label == label_by_id.end() || owner_label->second != wanted.back()) {
          return false;
        }
        return is_suffix(declared_scope_of(owner->second), std::span(wanted).first(wanted.size() - 1));
      };
      const bool foreign_root =
          !wanted.empty() && !project_scope_roots.contains(std::string(wanted.front()));
      const bool contradictable = foreign_root || records_scope(target_id) ||
                                  std::any_of(overload_rest.begin(), overload_rest.end(), records_scope);
      if (!contradictable) {
        qualifier_unchecked = true;
      } else {
        for (const auto& sibling : overload_rest) {
          if (in_scope(sibling)) {
            gated_rest.push_back(sibling);
          }
        }
        if (!in_scope(target_id)) {
          if (gated_rest.empty()) {
            if (overload_counted) {
              --tally.resolved_overload_first;  // the set produced no edge after all
            }
            ++tally.dropped_scope_mismatch;
            continue;
          }
          target_id = gated_rest.front();
          gated_rest.erase(gated_rest.begin());
        }
        overload_rest = std::span(gated_rest);
      }
    }
    if (target_id == raw_call.caller_id) {
      ++tally.dropped_self;
      continue;
    }
    if (qualifier_unchecked) {
      ++tally.resolved_qualifier_unchecked;
    }
    if (same_file_hit) {
      ++tally.resolved_same_file;
    } else if (member_method_hit || receiver_scoped_hit) {
      // Both are member-call resolutions against methods; the receiver-scoped
      // tier just had stronger evidence for which class's method it is.
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

RelationScopes build_relation_scopes(const GraphSnapshot& graph) {
  RelationScopes scopes;
  scopes.label_by_id.reserve(graph.nodes.size());
  scopes.file_id_by_source = file_node_ids(graph);

  // Per-file declared symbols (label -> id, empty when ambiguous), the same
  // index used for same-file call resolution. Heritage relations may resolve a
  // base type to a declaration in the same file.
  for (const auto& node : graph.nodes) {
    scopes.label_by_id.emplace(node.id, node.label);
    if (node.source_file.empty()) {
      continue;
    }
    if (node.kind != "function" && node.kind != "class" && node.kind != "type" && node.kind != "variable") {
      continue;
    }
    auto& by_label = scopes.local_by_file[node.source_file];
    const auto [slot, inserted] = by_label.emplace(make_id(node.label), node.id);
    if (!inserted && slot->second != node.id) {
      slot->second.clear();
    }
  }

  // Per-file imported names (file id -> label -> imported target id), built from
  // the import/re_export edges left by resolve_imports. This is the import-alias
  // map every relation target is resolved through.
  for (const auto& edge : graph.edges) {
    if (edge.relation != "imports" && edge.relation != "re_exports") {
      continue;
    }
    if (const auto label = scopes.label_by_id.find(edge.target); label != scopes.label_by_id.end()) {
      auto& names = scopes.imported_by_file[edge.source];
      names.emplace(make_id(label->second), edge.target);
      // `import { config as configModule }`: the file says `configModule`.
      if (const auto alias = edge.properties.find("alias"); alias != edge.properties.end()) {
        names.emplace(make_id(alias->second), edge.target);
      }
    }
  }
  return scopes;
}

std::string resolve_scoped_name(const RelationScopes& scopes, const std::string& source_file,
                                const std::string& name_key, bool allow_same_file) {
  if (const auto file_id = scopes.file_id_by_source.find(source_file); file_id != scopes.file_id_by_source.end()) {
    if (const auto file = scopes.imported_by_file.find(file_id->second); file != scopes.imported_by_file.end()) {
      if (const auto slot = file->second.find(name_key); slot != file->second.end()) {
        return slot->second;
      }
    }
  }
  if (allow_same_file) {
    if (const auto file = scopes.local_by_file.find(source_file); file != scopes.local_by_file.end()) {
      if (const auto slot = file->second.find(name_key); slot != file->second.end()) {
        return slot->second;  // empty when the file declares the name twice
      }
    }
  }
  return {};
}

void resolve_raw_relations(GraphSnapshot& graph, std::span<const RawRelation> raw_relations) {
  std::unordered_set<std::string> node_ids;
  node_ids.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    node_ids.insert(node.id);
  }
  const auto scopes = build_relation_scopes(graph);
  const auto& local_by_file = scopes.local_by_file;
  const auto& imported_by_file = scopes.imported_by_file;

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
  const auto file_id_by_source = file_node_ids(graph);
  for (const auto& edge : graph.edges) {
    if (edge.relation != "imports" && edge.relation != "re_exports") {
      continue;
    }
    if (const auto src = file_source_by_id.find(edge.target); src != file_source_by_id.end()) {
      included_files_by_file[edge.source].push_back(src->second);
    }
  }

  // A C/C++ `#include` is textual: every declaration a header includes is
  // visible to the file that includes the header. Resolving only through the
  // direct includes left CGraph's own `Node`, `Edge` and `RawCall` with no
  // incoming `references` at all -- graph_builder.cpp includes graph_builder.hpp,
  // which includes types.hpp -- so half the "unreferenced" types in `report
  // types` were used everywhere. The include graph is walked breadth-first from
  // the source file, bounded by kIncludeDepth, and a target resolves at the
  // NEAREST distance where exactly one declaration bears its name: two
  // declarations at that distance are an ambiguity and refuse the edge, a
  // nearer declaration shadows a farther one. Levels are memoized per file.
  constexpr std::size_t kIncludeDepth = 8;
  std::unordered_map<std::string, std::vector<std::vector<std::string>>> include_levels_by_file;
  const auto include_levels = [&](const std::string& source_file_id) -> const std::vector<std::vector<std::string>>& {
    auto it = include_levels_by_file.find(source_file_id);
    if (it != include_levels_by_file.end()) {
      return it->second;
    }
    std::vector<std::vector<std::string>> levels;
    std::unordered_set<std::string> visited{source_file_id};
    std::vector<std::string> frontier{source_file_id};
    for (std::size_t depth = 0; depth < kIncludeDepth && !frontier.empty(); ++depth) {
      std::vector<std::string> next_sources;
      std::vector<std::string> next_ids;
      for (const auto& file_id : frontier) {
        const auto inc = included_files_by_file.find(file_id);
        if (inc == included_files_by_file.end()) {
          continue;
        }
        for (const auto& included_source : inc->second) {
          const auto included_id = file_id_by_source.find(included_source);
          if (included_id == file_id_by_source.end() || !visited.insert(included_id->second).second) {
            continue;
          }
          next_sources.push_back(included_source);
          next_ids.push_back(included_id->second);
        }
      }
      if (next_sources.empty()) {
        break;
      }
      levels.push_back(std::move(next_sources));
      frontier = std::move(next_ids);
    }
    return include_levels_by_file.emplace(source_file_id, std::move(levels)).first->second;
  };
  const auto is_c_family = [](const std::string& source_file) {
    const auto language = detect_language(std::filesystem::path(source_file));
    return language == DetectedLanguage::C || language == DetectedLanguage::Cpp;
  };

  std::unordered_set<std::string> seen_edges;
  seen_edges.reserve(graph.edges.size());
  for (const auto& edge : graph.edges) {
    seen_edges.insert(edge_key(edge));
  }

  // Memoize make_id of the recurring target type names (they repeat heavily
  // across relations). Each memo returns the exact value the inline make_id call
  // produced, so resolution is unchanged.
  std::unordered_map<std::string, std::string> target_key_cache;
  const auto memo = [](std::unordered_map<std::string, std::string>& cache,
                       const std::string& s) -> const std::string& {
    auto it = cache.find(s);
    if (it == cache.end()) {
      it = cache.emplace(s, make_id(s)).first;
    }
    return it->second;
  };
  static const std::string no_file_id;

  for (const auto& relation : raw_relations) {
    if (relation.relation == "route" || relation.relation == "file_route" || relation.relation == "mounts" ||
        relation.relation == "aliases" || relation.relation == "http_call" || relation.relation == "http_wrapper" ||
        relation.relation == "url_const") {
      continue;  // HTTP contract facts: resolve_contracts mints endpoints from these
    }
    if (relation.source_id.empty() || relation.target_label.empty() || !node_ids.contains(relation.source_id)) {
      continue;
    }
    const auto& key = memo(target_key_cache, relation.target_label);
    // The file-id lookups below (imported_by_file / included_files_by_file) are
    // keyed by the source file's node id.
    const auto file_slot = file_id_by_source.find(relation.source_file);
    const auto& source_file_id = file_slot == file_id_by_source.end() ? no_file_id : file_slot->second;

    std::string target_id;
    // 1. The type the source file imports (the canonical resolution path).
    if (const auto file = imported_by_file.find(source_file_id); file != imported_by_file.end()) {
      if (const auto slot = file->second.find(key); slot != file->second.end()) {
        target_id = slot->second;
      }
    }
    // 1b. The type is declared in a file the source file #includes (C/C++
    //     whole-file import). For the C family the walk is transitive and
    //     nearest-unique (see include_levels); other languages keep the direct
    //     includes only, since a transitive import does not re-export names.
    if (target_id.empty()) {
      if (is_c_family(relation.source_file)) {
        for (const auto& level : include_levels(source_file_id)) {
          std::string found;
          bool ambiguous = false;
          for (const auto& included_source : level) {
            const auto file = local_by_file.find(included_source);
            if (file == local_by_file.end()) {
              continue;
            }
            const auto slot = file->second.find(key);
            if (slot == file->second.end()) {
              continue;
            }
            if (slot->second.empty() || (!found.empty() && found != slot->second)) {
              ambiguous = true;  // two declarations at this distance, or one file declaring it twice
              break;
            }
            found = slot->second;
          }
          if (ambiguous) {
            break;
          }
          if (!found.empty()) {
            target_id = found;
            break;
          }
        }
      } else if (const auto inc = included_files_by_file.find(source_file_id); inc != included_files_by_file.end()) {
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
  // Java reuses `method_declaration` for interface bodies, so its contract
  // methods arrive as ordinary `method` nodes and cannot be recognized by their
  // own tag (unlike Go's separately-emitted, namespaced `method_elem` nodes).
  // The owning declaration carries the distinction instead: a method owned by an
  // `interface` node is a promise, not an implementation.
  const auto is_iface_owner = [&](const std::string& id) {
    const auto it = nodes_by_id.find(id);
    if (it == nodes_by_id.end()) {
      return false;
    }
    const auto tag = it->second->properties.find("interface");
    return tag != it->second->properties.end() && tag->second == "true";
  };

  // interface id -> (method name key -> interface method node id)
  std::map<std::string, std::map<std::string, std::string>> iface_methods;
  // concrete type id -> (method name key -> method node ids)
  std::map<std::string, std::map<std::string, std::vector<std::string>>> type_methods;
  for (const auto& edge : graph.edges) {
    // A promise either by its own tag (Go) or by its owner being an interface
    // declaration (Java). Both mean the same thing to everything downstream.
    const bool promises = edge.relation == "method" &&
                          (is_iface_method(edge.target) || is_iface_owner(edge.source));
    if (promises) {
      iface_methods[edge.source].emplace(make_id(nodes_by_id.at(edge.target)->label), edge.target);
    } else if (edge.relation == "method_of") {
      // method -> receiver type (Go); invert into the type's method set.
      if (const auto method = nodes_by_id.find(edge.source); method != nodes_by_id.end()) {
        type_methods[edge.target][make_id(method->second->label)].push_back(edge.source);
      }
    } else if (edge.relation == "method") {
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
