#include "cgraph/contracts.hpp"

#include "cgraph/graph_builder.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

constexpr std::array<std::string_view, 8> kHttpVerbs = {
    "get", "post", "put", "patch", "delete", "head", "options", "all",
};
constexpr std::string_view kEndpointKind = "endpoint";
constexpr std::string_view kRouteRelation = "route";
constexpr std::string_view kFileRouteRelation = "file_route";
constexpr std::string_view kMountsRelation = "mounts";
constexpr std::string_view kAliasRelation = "aliases";
constexpr std::string_view kHandledBy = "handled_by";
constexpr std::string_view kRoutePrefix = "route_prefix";
// A chain mounted under many parents serves its routes at every mount path.
// Real code mounts a router once or twice; past this the mount graph is not a
// router tree but something the walk should not keep unrolling.
constexpr std::size_t kMaxMountPaths = 16;

[[nodiscard]] std::string to_upper(std::string_view text) {
  std::string upper(text);
  for (auto& ch : upper) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return upper;
}

[[nodiscard]] std::string edge_identity(const Edge& edge) {
  return edge.source + '\n' + edge.relation + '\n' + edge.target;
}

struct Mount {
  std::string parent;  // the mounting chain's variable node id
  std::string prefix;  // the mount path, empty when the framework takes none
};

}  // namespace

bool is_http_verb(std::string_view verb) {
  return std::ranges::find(kHttpVerbs, verb) != kHttpVerbs.end();
}

std::string join_route_path(std::string_view prefix, std::string_view path) {
  std::string joined = "/";
  const auto append = [&](std::string_view part) {
    for (const char ch : part) {
      if (ch == '/') {
        if (joined.back() != '/') {
          joined.push_back('/');
        }
        continue;
      }
      joined.push_back(ch);
    }
  };
  append(prefix);
  if (joined.back() != '/') {
    joined.push_back('/');
  }
  append(path);
  if (joined.size() > 1 && joined.back() == '/') {
    joined.pop_back();
  }
  return joined;
}

std::optional<std::string> next_route_path(std::string_view source_file) {
  const std::filesystem::path file{std::string(source_file)};
  if (file.stem().generic_string() != "route") {
    return std::nullopt;
  }
  static constexpr std::array<std::string_view, 5> kExtensions = {".ts", ".js", ".tsx", ".jsx", ".mjs"};
  if (std::ranges::find(kExtensions, file.extension().generic_string()) == kExtensions.end()) {
    return std::nullopt;
  }
  std::vector<std::string> parts;
  for (const auto& part : file.parent_path()) {
    parts.push_back(part.generic_string());
  }
  // The LAST `app` directory: a repo checked out under `~/app/` still routes from
  // its own `app/` tree.
  const auto app = std::find(parts.rbegin(), parts.rend(), "app");
  if (app == parts.rend()) {
    return std::nullopt;
  }
  std::string path;
  for (auto it = app.base(); it != parts.end(); ++it) {
    const auto& segment = *it;
    if (segment.empty() || (segment.front() == '(' && segment.back() == ')') || segment.front() == '@') {
      continue;  // route group `(marketing)` or parallel-route slot `@modal`: not in the URL
    }
    if ((segment.starts_with("[...") || segment.starts_with("[[...")) && segment.ends_with("]")) {
      path += "/*";  // catch-all `[...slug]`, optional catch-all `[[...slug]]`
      continue;
    }
    if (segment.size() > 2 && segment.front() == '[' && segment.back() == ']') {
      path += "/:" + segment.substr(1, segment.size() - 2);  // dynamic `[id]`
      continue;
    }
    path += "/" + segment;
  }
  return path.empty() ? std::string{"/"} : path;
}

void resolve_contracts(GraphSnapshot& graph, std::span<const RawRelation> raw_relations,
                       ContractResolution* stats) {
  ContractResolution local;
  ContractResolution& tally = stats != nullptr ? *stats : local;
  tally = ContractResolution{};

  std::unordered_map<std::string, const Node*> by_id;
  by_id.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    by_id.emplace(node.id, &node);
  }
  std::unordered_set<std::string> seen_edges;
  seen_edges.reserve(graph.edges.size());
  for (const auto& edge : graph.edges) {
    seen_edges.insert(edge_identity(edge));
  }
  const auto scopes = build_relation_scopes(graph);

  // Same-file chains by exact label. The shared scope index keys names through
  // make_id, which folds case, so `const app = new Elysia()` beside
  // `export type App = typeof app` (turing-api's app.ts) reads as ambiguous
  // there. A router is always a `variable`, and JavaScript is case-sensitive,
  // so the chain lookup is exact and variable-only. Empty when a file declares
  // two variables of one name.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> variables_by_file;
  for (const auto& node : graph.nodes) {
    if (node.kind != "variable" || node.source_file.empty()) {
      continue;
    }
    const auto [slot, inserted] = variables_by_file[node.source_file].emplace(node.label, node.id);
    if (!inserted && slot->second != node.id) {
      slot->second.clear();
    }
  }

  // Which chain a router identifier names, as seen from the file that uses it:
  // the file's import of that name (by alias when imported `as` one) first,
  // then its own variable of that exact name. Only a `variable` can be a chain
  // (`const app = new Elysia()`); a `.use(cors())` plugin call never reaches
  // here (its argument is not an identifier), and a `.use(authMiddleware)`
  // naming a function is middleware, not a mount.
  const auto name_in_scope = [&](const RawRelation& relation) -> std::string {
    auto id = resolve_scoped_name(scopes, relation.source_file, make_id(relation.target_label), false);
    if (id.empty()) {
      if (const auto file = variables_by_file.find(relation.source_file); file != variables_by_file.end()) {
        if (const auto slot = file->second.find(relation.target_label); slot != file->second.end()) {
          id = slot->second;
        }
      }
    }
    return id;
  };
  const auto chain_named = [&](const RawRelation& relation) -> std::string {
    const auto id = name_in_scope(relation);
    if (id.empty() || id == relation.source_id) {
      return {};
    }
    const auto node = by_id.find(id);
    return node != by_id.end() && node->second->kind == "variable" ? id : std::string{};
  };

  std::vector<Node> new_nodes;
  std::vector<Edge> new_edges;
  const auto add_edge = [&](std::string source, std::string target, std::string_view relation,
                            std::string_view property, std::string value) {
    Edge edge{
        .source = std::move(source),
        .target = std::move(target),
        .relation = std::string(relation),
        .confidence = Confidence::Extracted,
    };
    if (!value.empty()) {
      edge.properties.emplace(std::string(property), std::move(value));
    }
    if (seen_edges.insert(edge_identity(edge)).second) {
      new_edges.push_back(std::move(edge));
    }
  };

  // 1. Mounts: child chain id -> the chains that mount it, with mount paths. An
  //    alias (`export const deckModule = deckRoutes as unknown as Elysia`) is
  //    the same chain under a second name: a mount with no path and no edge.
  std::unordered_map<std::string, std::vector<Mount>> parents_of;
  for (const auto& relation : raw_relations) {
    const bool alias = relation.relation == kAliasRelation;
    if (relation.relation != kMountsRelation && !alias) {
      continue;
    }
    ++tally.mounts;
    if (!by_id.contains(relation.source_id)) {
      ++tally.mounts_unresolved;
      continue;
    }
    const auto child = chain_named(relation);
    if (child.empty()) {
      // An identifier no import or declaration in the file explains, or one
      // naming middleware rather than a router. Neither adds a mount path.
      if (name_in_scope(relation).empty()) {
        ++tally.mounts_unresolved;
      }
      continue;
    }
    parents_of[child].push_back(Mount{.parent = relation.source_id, .prefix = relation.context});
    if (!alias) {
      add_edge(relation.source_id, child, kMountsRelation, "prefix", relation.context);
    }
  }

  // 2. Every path a chain is served under: its own prefix beneath each mount
  //    path of each parent, recursively, up to the top-level chains. A chain
  //    nobody mounts is a top-level chain. A mount cycle (`a.use(b); b.use(a)`)
  //    treats the revisited chain as a top-level one rather than spinning.
  std::unordered_map<std::string, std::vector<std::string>> memo;
  std::unordered_set<std::string> on_stack;
  const std::function<const std::vector<std::string>&(const std::string&)> paths_of =
      [&](const std::string& chain) -> const std::vector<std::string>& {
    if (const auto known = memo.find(chain); known != memo.end()) {
      return known->second;
    }
    std::string own;
    if (const auto node = by_id.find(chain); node != by_id.end()) {
      if (const auto prefix = node->second->properties.find(std::string(kRoutePrefix));
          prefix != node->second->properties.end()) {
        own = prefix->second;
      }
    }
    std::vector<std::string> result;
    const auto parents = parents_of.find(chain);
    if (parents == parents_of.end() || !on_stack.insert(chain).second) {
      result.push_back(join_route_path("", own));
    } else {
      for (const auto& mount : parents->second) {
        for (const auto& above : paths_of(mount.parent)) {
          result.push_back(join_route_path(join_route_path(above, mount.prefix), own));
          if (result.size() >= kMaxMountPaths) {
            break;
          }
        }
        if (result.size() >= kMaxMountPaths) {
          break;
        }
      }
      on_stack.erase(chain);
      std::ranges::sort(result);
      result.erase(std::unique(result.begin(), result.end()), result.end());
    }
    return memo.emplace(chain, std::move(result)).first->second;
  };

  // 3. Routes: one endpoint per (method, full path); a handler registered on a
  //    chain served under two paths gets two endpoints, both handled by it.
  std::unordered_set<std::string> minted;
  for (const auto& relation : raw_relations) {
    const bool file_routed = relation.relation == kFileRouteRelation;
    if (relation.relation != kRouteRelation && !file_routed) {
      continue;
    }
    ++tally.routes;
    const auto handler = by_id.find(relation.source_id);
    if (handler == by_id.end()) {
      ++tally.routes_unresolved;
      continue;
    }
    const auto space = relation.context.find(' ');
    const auto verb = relation.context.substr(0, space);
    const auto route = space == std::string::npos ? std::string{} : relation.context.substr(space + 1);
    if (!is_http_verb(verb)) {
      ++tally.routes_unresolved;
      continue;
    }
    std::vector<std::string> bases;
    if (file_routed) {
      bases.push_back("/");  // a Next.js route file: the path is already absolute
    } else {
      // An empty chain is one the extractor could not root (`function
      // register(app) { app.get(...) }`); a named one may still be a router the
      // file neither declares nor imports. Either way the full path is
      // unknowable here, and an endpoint with a wrong path is worse than none.
      const auto chain = relation.target_label.empty() ? std::string{} : chain_named(relation);
      if (chain.empty()) {
        ++tally.routes_unresolved;
        continue;
      }
      bases = paths_of(chain);
    }
    const auto method = to_upper(verb);
    for (const auto& base : bases) {
      const auto path = join_route_path(base, route);
      const auto label = method + " " + path;
      const auto id = "endpoint:" + label;
      if (!by_id.contains(id) && minted.insert(id).second) {
        ++tally.endpoints;
        Node endpoint{
            .id = id,
            .label = label,
            .source_file = handler->second->source_file,
            .source_location = handler->second->source_location,
            .kind = std::string(kEndpointKind),
            .confidence = Confidence::Extracted,
        };
        endpoint.properties.emplace("method", method);
        endpoint.properties.emplace("path", path);
        new_nodes.push_back(std::move(endpoint));
        if (const auto file_id = make_id(handler->second->source_file); by_id.contains(file_id)) {
          add_edge(file_id, id, "contains", "", {});
        }
      }
      add_edge(id, relation.source_id, kHandledBy, "", {});
    }
  }

  graph.nodes.insert(graph.nodes.end(), std::make_move_iterator(new_nodes.begin()),
                     std::make_move_iterator(new_nodes.end()));
  graph.edges.insert(graph.edges.end(), std::make_move_iterator(new_edges.begin()),
                     std::make_move_iterator(new_edges.end()));
}

}  // namespace cgraph
