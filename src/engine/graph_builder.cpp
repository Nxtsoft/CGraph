#include "cgraph/graph_builder.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
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

// True when a node's kind can be the target of a call. `class` stays eligible
// because `Foo()` is a genuine constructor call in Python and JavaScript, so a
// class-targeting CALLS edge there is correct. A `field` never is: a struct
// member named `connect` is not what `::connect(...)` invokes.
[[nodiscard]] bool is_callable_kind(std::string_view kind) {
  return kind == "function" || kind == "class";
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
    if (path.stem() == "index") {
      file_id_by_key.emplace(path.parent_path().generic_string(), node.id);
    }
    files_by_path.emplace_back(path.generic_string(), node.id);
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
        if (match) {
          return std::nullopt;  // ambiguous
        }
        match = id;
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
    const auto file_id = resolve_file(prop->second);
    if (!file_id) {
      removed.insert(node.id);
      dropped.insert(node.id);  // external package: delete node and its edges
      continue;
    }
    if (node.kind == "module") {
      remap[node.id] = *file_id;
    } else {
      const auto real = make_id(source_of_file[*file_id] + ":" + node.label);
      remap[node.id] = node_ids.contains(real) ? real : *file_id;
    }
    removed.insert(node.id);
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
  std::unordered_set<std::string> node_ids;
  std::unordered_map<std::string, std::string> source_file_by_id;
  node_ids.reserve(graph.nodes.size());
  source_file_by_id.reserve(graph.nodes.size());

  // Per-file declared symbols: source_file -> (normalized label -> node id). An
  // empty id marks a label that is declared more than once in the file, so it
  // resolves to no single target.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> local_by_file;
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
    const auto [slot, inserted] = by_label.emplace(make_id(node.label), node.id);
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

    // 1. A symbol declared in the caller's own file (local helper, sibling fn).
    if (const auto file = local_by_file.find(caller_file); file != local_by_file.end()) {
      if (const auto slot = file->second.find(key); slot != file->second.end()) {
        target_id = slot->second;
        // An empty slot marks a name declared more than once in the file, so it
        // resolves to no single target -- ambiguous, not unknown.
        if (target_id.empty()) {
          ++tally.dropped_ambiguous;
          continue;
        }
        same_file_hit = true;
      }
    }

    // 2. A project-wide unique label. An ambiguous name (more than one
    //    declaration) or an unknown one resolves to nothing and is dropped —
    //    this exactly-one-candidate rule is what keeps cross-file calls honest.
    //    Member calls (`obj.method()`) are excluded: the bare property name has
    //    no import evidence and collides with any top-level function of the same
    //    name, so it stays scoped to the caller's own file (handled above).
    if (target_id.empty() && !raw_call.is_member_call) {
      const auto targets = index.find(key);
      if (targets == index.end()) {
        ++tally.dropped_unknown;
        continue;
      }
      if (targets->second.size() != 1) {
        ++tally.dropped_ambiguous;
        continue;
      }
      target_id = targets->second.front();
      // Grade confidence: EXTRACTED when the caller's file actually imports the
      // resolved symbol or its module, INFERRED when it is only a name match.
      const auto& caller_file_id = file_id_for(caller_file);
      const auto& callee_file_id = file_id_for(source_file_by_id[target_id]);
      const auto symbols = imported_symbols.find(caller_file_id);
      const auto modules = imported_modules.find(caller_file_id);
      const bool has_import_evidence =
          (symbols != imported_symbols.end() && symbols->second.contains(target_id)) ||
          (modules != imported_modules.end() && modules->second.contains(callee_file_id));
      confidence = has_import_evidence ? Confidence::Extracted : Confidence::Inferred;
    }

    if (target_id.empty()) {
      // A member call that missed its own file: the receiver type is unknown, so
      // the project-wide tier deliberately does not apply.
      ++tally.dropped_unknown;
      continue;
    }
    if (target_id == raw_call.caller_id) {
      ++tally.dropped_self;
      continue;
    }
    ++(same_file_hit ? tally.resolved_same_file : tally.resolved_project_unique);
    Edge edge{
        .source = raw_call.caller_id,
        .target = target_id,
        .relation = std::string(kCallRelation),
        .confidence = confidence,
    };
    if (seen_edges.insert(edge_key(edge)).second) {
      graph.edges.push_back(std::move(edge));
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

}  // namespace cgraph
