#include "cgraph/contracts.hpp"

#include "cgraph/graph_builder.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/spring_actuator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <functional>
#include <optional>
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
constexpr std::string_view kHttpCallRelation = "http_call";
constexpr std::string_view kHttpWrapperRelation = "http_wrapper";
constexpr std::string_view kHttpCallArgsRelation = "http_call_args";
constexpr std::string_view kUrlConstRelation = "url_const";
constexpr std::string_view kLangGraphClientRelation = "langgraph_client";
constexpr std::string_view kLangGraphCallRelation = "langgraph_call";
constexpr std::string_view kMapsTableRelation = "maps_table";
constexpr std::string_view kSqlTableKind = "sql_table";
constexpr std::string_view kHandledBy = "handled_by";
constexpr std::string_view kConsumes = "CONSUMES";
constexpr std::string_view kRoutePrefix = "route_prefix";
constexpr std::string_view kProvidesContractRelation = "provides_contract";
constexpr std::string_view kUsesContractRelation = "uses_contract";
constexpr std::array<std::string_view, 5> kContractKinds = {"table", "label", "header", "claim", "env"};
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

// "<METHOD or empty> <path>" as the extractor spells a route or call context.
struct MethodPath {
  std::string method;
  std::string path;
};

[[nodiscard]] MethodPath split_context(const std::string& context) {
  const auto space = context.find(' ');
  if (space == std::string::npos) {
    return MethodPath{.method = context, .path = {}};
  }
  return MethodPath{.method = context.substr(0, space), .path = context.substr(space + 1)};
}

[[nodiscard]] std::string to_lower(std::string_view text) {
  std::string lower(text);
  for (auto& ch : lower) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return lower;
}

// `table` and `label` live in a database; the other kinds are global names.
[[nodiscard]] bool database_scoped(std::string_view kind) { return kind == "table" || kind == "label"; }

}  // namespace

bool is_contract_kind(std::string_view kind) {
  return std::ranges::find(kContractKinds, kind) != kContractKinds.end();
}

std::optional<std::string> contract_id(std::string_view kind, std::string_view name, std::string_view database) {
  if (!is_contract_kind(kind) || name.empty()) {
    return std::nullopt;
  }
  if (database_scoped(kind)) {
    if (database.find(':') != std::string_view::npos) {
      return std::nullopt;
    }
    const auto scope = database.empty() ? kLocalDatabase : database;
    return std::string(kind) + ":" + std::string(scope) + ":" + std::string(name);
  }
  // HTTP header names are case-insensitive (RFC 9110 5.1): `X-Tenant-Id` read by
  // the server and `x-tenant-id` sent by a client are one header.
  return std::string(kind) + ":" + (kind == "header" ? to_lower(name) : std::string(name));
}

std::string_view contract_kind_of(std::string_view id) {
  const auto colon = id.find(':');
  if (colon == std::string_view::npos || colon + 1 >= id.size()) {
    return {};
  }
  const auto kind = id.substr(0, colon);
  return kind == kEndpointKind || is_contract_kind(kind) ? kind : std::string_view{};
}

bool is_database_local_contract(std::string_view id) {
  const auto kind = contract_kind_of(id);
  return database_scoped(kind) && id.substr(kind.size() + 1).starts_with(std::string(kLocalDatabase) + ":");
}

bool is_bridged_contract(std::string_view id) {
  return !contract_kind_of(id).empty() && !is_database_local_contract(id);
}

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

std::string canonical_route_path(std::string_view path) {
  // A trailing slash is spelling, not identity: an OpenAPI document renders
  // `.get('/')` under `/composites` as `/api/v1/composites/`, the router
  // serves `/api/v1/composites`, and a client calls either.
  while (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }
  std::string canonical;
  canonical.reserve(path.size());
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto segment = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    const bool parameter =
        (!segment.empty() && segment.front() == ':') ||
        (segment.size() >= 2 && segment.front() == '{' && segment.back() == '}') ||
        (segment.size() >= 2 && segment.front() == '[' && segment.back() == ']');
    canonical += parameter ? std::string_view{"{}"} : segment;
    if (slash == std::string_view::npos) {
      break;
    }
    canonical.push_back('/');
    start = slash + 1;
  }
  return canonical;
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
  const auto file_ids = file_node_ids(graph);
  // The file node containing a handler, so a minted endpoint can hang off it.
  const auto file_of = [&](const Node& handler) -> std::optional<std::string> {
    const auto file_id = file_ids.find(handler.source_file);
    if (file_id == file_ids.end()) {
      return std::nullopt;
    }
    return file_id->second;
  };
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
  // Python's `include_router(users.router)` after `from app.routers import
  // users`: `users` is an imported module file, `router` its own variable.
  // Only `module.name`; a longer chain resolves to nothing.
  const auto python_source = [](const RawRelation& relation) {
    return std::filesystem::path(relation.source_file).extension() == ".py";
  };
  const auto python_dotted = [&](const RawRelation& relation) {
    return relation.target_label.find('.') != std::string::npos && python_source(relation);
  };
  const auto python_module_attribute = [&](const RawRelation& relation) -> std::string {
    const auto& label = relation.target_label;
    const auto dot = label.find('.');
    if (label.find('.', dot + 1) != std::string::npos) {
      return {};
    }
    const auto module =
        by_id.find(resolve_scoped_name(scopes, relation.source_file, make_id(label.substr(0, dot)), false));
    if (module == by_id.end() || module->second->kind != "file") {
      return {};
    }
    const auto file = variables_by_file.find(module->second->source_file);
    if (file == variables_by_file.end()) {
      return {};
    }
    const auto slot = file->second.find(label.substr(dot + 1));
    return slot == file->second.end() ? std::string{} : slot->second;
  };
  const auto name_in_scope = [&](const RawRelation& relation) -> std::string {
    if (python_dotted(relation)) {
      return python_module_attribute(relation);
    }
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
      return true;
    }
    return false;
  };

  // 1. Mounts: child chain id -> the chains that mount it, with mount paths. An
  //    alias (`export const deckModule = deckRoutes as unknown as Elysia`) is
  //    the same chain under a second name: a mount with no path and no edge.
  std::unordered_map<std::string, std::vector<Mount>> parents_of;
  // Chains mounted somewhere the extractor could not place: a mount with no
  // mounting chain (Python's `app.include_router(r, prefix=settings.P)`, or one
  // inside an app factory), or a Python mount on a name that is no chain. A chain with no other mount is served at a path
  // nobody knows, so its routes mint nothing rather than a wrong top-level path.
  std::unordered_set<std::string> unplaced;
  for (const auto& relation : raw_relations) {
    const bool alias = relation.relation == kAliasRelation;
    if (relation.relation != kMountsRelation && !alias) {
      continue;
    }
    ++tally.mounts;
    if (relation.source_id.empty() && !alias) {
      ++tally.mounts_unresolved;
      if (const auto child = name_in_scope(relation); !child.empty()) {
        unplaced.insert(child);
      }
      continue;
    }
    if (!by_id.contains(relation.source_id)) {
      ++tally.mounts_unresolved;
      // Python: the mounting name is no chain this file declares (`app =
      // create_app()`, or an `api_router` imported from the file that mounts
      // it), so the child is served somewhere nobody can place. A JavaScript
      // child keeps its own path, as it always has.
      if (python_source(relation)) {
        if (const auto child = name_in_scope(relation); !child.empty()) {
          unplaced.insert(child);
        }
      }
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
    if (parents == parents_of.end() && unplaced.contains(chain)) {
      // Served only under a mount nobody could place: no known path.
    } else if (parents == parents_of.end() || !on_stack.insert(chain).second) {
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

  // Endpoint nodes minted this resolve, by id. A route mints with the
  // provider's spelling as label; a consumer that finds no provider mints with
  // the canonical spelling and `served: false`.
  std::unordered_map<std::string, std::size_t> minted;  // id -> index in new_nodes
  const auto mint = [&](const std::string& id, std::string label, const std::string& method,
                        const std::string& path, const Node* handler) -> bool {
    if (by_id.contains(id) || minted.contains(id)) {
      return false;
    }
    Node endpoint{
        .id = id,
        .label = std::move(label),
        .kind = std::string(kEndpointKind),
        .confidence = Confidence::Extracted,
    };
    endpoint.properties.emplace("method", method);
    endpoint.properties.emplace("path", path);
    if (handler != nullptr) {
      endpoint.source_file = handler->source_file;
      endpoint.source_location = handler->source_location;
    } else {
      endpoint.properties.emplace("served", "false");
    }
    minted.emplace(id, new_nodes.size());
    new_nodes.push_back(std::move(endpoint));
    return true;
  };

  // 3. Routes: one endpoint per (method, full path); a handler registered on a
  //    chain served under two paths gets two endpoints, both handled by it.
  //    Spring Boot Actuator routes are decided across the build file and the
  //    application config, so they are derived here from those files' facts.
  const auto actuator_routes = spring_actuator_routes(raw_relations);
  std::vector<RawRelation> with_actuator;
  if (!actuator_routes.empty()) {
    with_actuator.reserve(raw_relations.size() + actuator_routes.size());
    with_actuator.assign(raw_relations.begin(), raw_relations.end());
    with_actuator.insert(with_actuator.end(), actuator_routes.begin(), actuator_routes.end());
  }
  const std::span<const RawRelation> route_facts = actuator_routes.empty() ? raw_relations : with_actuator;
  for (const auto& relation : route_facts) {
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
    const auto [verb, route] = split_context(relation.context);
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
      if (bases.empty()) {
        ++tally.routes_unresolved;  // its chain hangs only off an unplaced mount
        continue;
      }
    }
    const auto method = to_upper(verb);
    for (const auto& base : bases) {
      const auto path = join_route_path(base, route);
      const auto id = "endpoint:" + method + " " + canonical_route_path(path);
      if (mint(id, method + " " + path, method, path, handler->second)) {
        ++tally.endpoints;
        if (const auto file_id = file_of(*handler->second)) {
          add_edge(*file_id, id, "contains", "", {});
        }
      } else if (const auto slot = minted.find(id); slot != minted.end()) {
        // A consumer minted it first, or a second handler serves the same route:
        // the served spelling and anchor win.
        auto& endpoint = new_nodes[slot->second];
        if (endpoint.properties.erase("served") > 0) {
          endpoint.label = method + " " + path;
          endpoint.properties["path"] = path;
          endpoint.source_file = handler->second->source_file;
          endpoint.source_location = handler->second->source_location;
          if (const auto file_id = file_of(*handler->second)) {
            add_edge(*file_id, id, "contains", "", {});
          }
        }
      }
      add_edge(id, relation.source_id, kHandledBy, "", {});
    }
  }

  // 4. Wrappers: functions whose own client call appends their first parameter
  //    to a fixed prefix. `apiFetch('/notebooks')` is then a consumer of
  //    `/api/v1/notebooks`.
  //    A wrapper whose path is another parameter (`request(method, path)`)
  //    spells it `<METHOD> <prefix> #<index>`, and one whose method is a
  //    parameter spells the method `@<index>` (`@<index>=<VERB>` when the
  //    parameter defaults to a verb). A method may end in `?` (the wrapper's
  //    own options are unreadable) and `~<index>` (a caller's options at that
  //    index override it: `{ ...init }`), and may be choices (`POST|DELETE`).
  //    Calls to any of those are read from their `http_call_args` facts (step 6b).
  struct Wrapper {
    std::vector<std::string> methods;  // fixed by the wrapper's own call (`method: 'POST'`), else empty
    std::string prefix;
    int method_parameter = -1;
    std::string method_default;  // `method = 'GET'`: what a call leaving the method out sends
    std::size_t path_parameter = 0;
    int options_parameter = -1;  // the parameter whose `method` overrides the wrapper's
    bool unknown = false;
  };
  const auto split_verbs = [](const std::string& spelled) {
    std::vector<std::string> verbs;
    for (std::size_t start = 0; start < spelled.size();) {
      const auto bar = spelled.find('|', start);
      verbs.push_back(spelled.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
      start = bar == std::string::npos ? spelled.size() : bar + 1;
    }
    return verbs;
  };
  std::unordered_map<std::string, Wrapper> wrappers;
  for (const auto& relation : raw_relations) {
    if (relation.relation != kHttpWrapperRelation || !by_id.contains(relation.source_id)) {
      continue;
    }
    auto [method, prefix] = split_context(relation.context);
    Wrapper wrapper;
    if (const auto mark = prefix.rfind(" #"); mark != std::string::npos) {
      wrapper.path_parameter = static_cast<std::size_t>(std::stoul(prefix.substr(mark + 2)));
      prefix.resize(mark);
    }
    if (const auto mark = method.rfind('~'); mark != std::string::npos) {
      wrapper.options_parameter = std::stoi(method.substr(mark + 1));
      method.resize(mark);
    }
    if (method.ends_with('?')) {
      wrapper.unknown = true;
      method.pop_back();
    }
    if (method.starts_with("@")) {
      const auto equals = method.find('=');
      wrapper.method_parameter = std::stoi(method.substr(1, equals == std::string::npos ? std::string::npos : equals - 1));
      wrapper.method_default = equals == std::string::npos ? std::string{} : method.substr(equals + 1);
      method.clear();
    }
    wrapper.methods = split_verbs(method);
    wrapper.prefix = std::move(prefix);
    wrappers.emplace(relation.source_id, std::move(wrapper));
  }
  const auto positional = [](const Wrapper& wrapper) {
    return wrapper.path_parameter != 0 || wrapper.method_parameter >= 0 || wrapper.options_parameter >= 0 ||
           wrapper.unknown || wrapper.methods.size() > 1;
  };
  // A path without a leading slash (`v1/users`) is joined to a base URL by its
  // client; only a prefix ending in a slash (an axios baseURL, `${API}/${path}`)
  // says where.
  const auto relative_joins = [](const std::string& prefix, const std::string& path) {
    return path.empty() || path.front() == '/' || path.starts_with("${") || (!prefix.empty() && prefix.back() == '/');
  };

  // 5. URL constants by name, project-wide. A path spelled `${API_BASE}...`
  //    refers to a constant its file imports; when exactly one file defines a
  //    URL constant of that name the value is inlined (itself expanded), else
  //    the path is unresolvable. Two files defining the same name differently
  //    is the ambiguity that refuses it.
  std::unordered_map<std::string, std::unordered_set<std::string>> url_consts;
  for (const auto& relation : raw_relations) {
    if (relation.relation == kUrlConstRelation) {
      url_consts[relation.target_label].insert(relation.context);
    }
  }
  const auto expand = [&](std::string path) -> std::optional<std::string> {
    for (int depth = 0; depth < 4 && path.starts_with("${"); ++depth) {
      const auto close = path.find('}');
      if (close == std::string::npos) {
        return std::nullopt;
      }
      const auto values = url_consts.find(path.substr(2, close - 2));
      if (values == url_consts.end() || values->second.size() != 1) {
        return std::nullopt;
      }
      path = *values->second.begin() + path.substr(close + 1);
    }
    return path.starts_with("${") ? std::nullopt : std::optional<std::string>{std::move(path)};
  };

  // 6. Consumers: every client call with a resolvable path becomes a CONSUMES
  //    edge from its caller to the endpoint, minting the endpoint when this
  //    repo does not serve it. A call through a name that resolves to no
  //    wrapper is an ordinary function taking a path-like string, not a
  //    consumer, and is skipped without a tally.
  //    A `langgraph_call` (`createLangGraphClient(token).runs.stream(...)` with
  //    the factory imported) is a client call like `axios.post` once the
  //    factory resolves, through the file's imports, to a function whose
  //    `langgraph_client` fact says it returns an SDK `Client`; through any
  //    other name it is neither an edge nor a count.
  std::unordered_set<std::string> langgraph_factories;
  for (const auto& relation : raw_relations) {
    if (relation.relation == kLangGraphClientRelation) {
      langgraph_factories.insert(relation.source_id);
    }
  }
  for (const auto& relation : raw_relations) {
    const bool sdk_call = relation.relation == kLangGraphCallRelation;
    if (relation.relation != kHttpCallRelation && !sdk_call) {
      continue;
    }
    if (sdk_call &&
        !langgraph_factories.contains(resolve_scoped_name(scopes, relation.source_file, make_id(relation.target_label), false))) {
      continue;
    }
    const auto [explicit_method, raw_call_path] = split_context(relation.context);
    std::string method = explicit_method;
    std::string prefix;
    const bool primitive =
        sdk_call || relation.target_label == "fetch" || relation.target_label.find('.') != std::string::npos;
    if (!primitive) {
      const auto callee = resolve_scoped_name(scopes, relation.source_file, make_id(relation.target_label), true);
      const auto wrapper = callee.empty() ? wrappers.end() : wrappers.find(callee);
      if (wrapper == wrappers.end() || positional(wrapper->second)) {
        continue;  // positional wrappers are read from http_call_args below
      }
      // A wrapper here takes no options from its callers: its own method is sent.
      prefix = wrapper->second.prefix;
      method = wrapper->second.methods.empty() ? std::string{} : wrapper->second.methods.front();
    } else if (method.empty() && relation.target_label != "fetch") {
      // `api.GET(...)`, `axios.post(...)`: the verb is the property.
      method = to_upper(relation.target_label.substr(relation.target_label.rfind('.') + 1));
    }
    ++tally.calls;
    const auto expanded_prefix = expand(prefix);
    const auto expanded_path = raw_call_path.empty() ? std::optional<std::string>{} : expand(raw_call_path);
    if (!expanded_prefix || !expanded_path || expanded_path->empty() || !by_id.contains(relation.source_id)) {
      // A URL held in a local variable, an absolute external URL, a base
      // constant no single file defines, or a caller no node names.
      ++tally.calls_unresolved;
      continue;
    }
    prefix = *expanded_prefix;
    const auto& call_path = *expanded_path;
    if (!relative_joins(prefix, call_path)) {
      ++tally.calls_unresolved;
      continue;
    }
    if (method.empty()) {
      method = "GET";
    }
    // A call whose own path is parameters only (`apiFetch(\`/${entity}/${id}\`)`,
    // a generic helper) names no route: `/api/v1/{}/{}` would match everything.
    if (canonical_route_path(call_path).find_first_not_of("/{}") == std::string::npos) {
      ++tally.calls_unresolved;
      continue;
    }
    const auto path = canonical_route_path(join_route_path(prefix, call_path));
    const auto id = "endpoint:" + method + " " + path;
    if (mint(id, method + " " + path, method, path, nullptr)) {
      ++tally.endpoints_external;
    }
    if (add_edge(relation.source_id, id, kConsumes, "", {})) {
      ++tally.consumes;
    }
  }

  // 6b. Calls to a wrapper that takes its path from a later parameter, its
  //     method from a parameter (`mlBackendRequest('POST', \`/project/${id}/setup\`)`)
  //     or from the options it spreads, or whose own method is unreadable or a
  //     choice: the argument descriptors the extractor recorded fill those slots.
  for (const auto& relation : raw_relations) {
    if (relation.relation != kHttpCallArgsRelation) {
      continue;
    }
    const auto callee = resolve_scoped_name(scopes, relation.source_file, make_id(relation.target_label), true);
    const auto wrapper = callee.empty() ? wrappers.end() : wrappers.find(callee);
    if (wrapper == wrappers.end() || !positional(wrapper->second)) {
      continue;  // not a wrapper, or one whose first argument is the path (step 6)
    }
    ++tally.calls;
    std::vector<std::string> arguments;
    for (std::size_t start = 0;;) {
      const auto tab = relation.context.find('\t', start);
      arguments.push_back(relation.context.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
      if (tab == std::string::npos) {
        break;
      }
      start = tab + 1;
    }
    const auto& shape = wrapper->second;
    const auto argument = [&](std::size_t index, char kind) -> std::optional<std::string> {
      if (index >= arguments.size() || arguments[index].empty() || arguments[index].front() != kind) {
        return std::nullopt;
      }
      return arguments[index].substr(1);
    };
    // The wrapper's own method, from its parameter or fixed; then the caller's
    // options at the index the wrapper spreads them from, whose `method`
    // overrides it. Left out, or options with no `method`, keep the wrapper's,
    // fetch's GET when it has none; a method nobody can read is counted, never
    // guessed.
    std::vector<std::string> methods = shape.methods;
    bool unknown = shape.unknown;
    if (shape.method_parameter >= 0) {
      const auto index = static_cast<std::size_t>(shape.method_parameter);
      const auto verb = index >= arguments.size() ? std::optional<std::string>{shape.method_default} : argument(index, 'V');
      methods.clear();
      if (verb && !verb->empty()) {
        methods.push_back(*verb);
      } else {
        unknown = true;  // a method this call does not spell out
      }
    }
    if (shape.options_parameter >= 0 && static_cast<std::size_t>(shape.options_parameter) < arguments.size()) {
      const auto verb = argument(static_cast<std::size_t>(shape.options_parameter), 'O');
      if (!verb) {
        ++tally.calls_unresolved;
        continue;
      }
      if (!verb->empty()) {
        methods = split_verbs(*verb);
        unknown = false;
      }
    }
    if (unknown) {
      ++tally.calls_unresolved;
      continue;
    }
    if (methods.empty()) {
      methods.push_back("GET");
    }
    const auto raw_call_path = argument(shape.path_parameter, 'P');
    const auto expanded_prefix = expand(shape.prefix);
    const auto expanded_path = raw_call_path ? expand(*raw_call_path) : std::nullopt;
    if (!expanded_prefix || !expanded_path || expanded_path->empty() || !by_id.contains(relation.source_id) ||
        !relative_joins(*expanded_prefix, *expanded_path) ||
        canonical_route_path(*expanded_path).find_first_not_of("/{}") == std::string::npos) {
      ++tally.calls_unresolved;
      continue;
    }
    const auto path = canonical_route_path(join_route_path(*expanded_prefix, *expanded_path));
    for (const auto& method : methods) {
      const auto id = "endpoint:" + method + " " + path;
      if (mint(id, method + " " + path, method, path, nullptr)) {
        ++tally.endpoints_external;
      }
      if (add_edge(relation.source_id, id, kConsumes, "", {})) {
        ++tally.consumes;
      }
    }
  }

  // 7. ORM tables: a model declaration (`pgTable('competitors', ...)`) maps the
  //    SQL table a migration creates, so impact from the table reaches the model
  //    and, through its importers, the handlers. A name no migration creates
  //    (the schema lives outside the repo) links nothing.
  for (const auto& relation : raw_relations) {
    if (relation.relation != kMapsTableRelation || !by_id.contains(relation.source_id)) {
      continue;
    }
    const auto table = by_id.find(make_id("sql_table:" + relation.target_label));
    if (table != by_id.end() && table->second->kind == kSqlTableKind) {
      add_edge(relation.source_id, table->first, kMapsTableRelation, "", {});
    }
  }

  // 8. Contracts other than endpoints: tables, graph labels, headers, claims
  //    and env names. Providers first, so a contract's label is a provider's
  //    spelling; a contract only used here is minted with the user's spelling
  //    and `served: false`. The nodes carry no make_id'd id: the same contract
  //    in another repo's graph is the same id (a database-local table only
  //    once a workspace or seam declares the database).
  std::unordered_set<std::string> contract_nodes;  // ids minted this resolve
  for (const bool providing : {true, false}) {
    for (const auto& relation : raw_relations) {
      if (relation.relation != (providing ? kProvidesContractRelation : kUsesContractRelation)) {
        continue;
      }
      ++tally.contract_facts;
      const auto colon = relation.context.find(':');
      const auto kind = colon == std::string::npos ? std::string{} : relation.context.substr(0, colon);
      const auto name = colon == std::string::npos ? std::string{} : relation.context.substr(colon + 1);
      const auto source = by_id.find(relation.source_id);
      const auto id = contract_id(kind, name, database_scoped(kind) ? relation.target_label : std::string{});
      if (!id || source == by_id.end()) {
        ++tally.contract_facts_unresolved;  // malformed context, unknown kind, or no node at the code
        continue;
      }
      if (!by_id.contains(*id) && contract_nodes.insert(*id).second) {
        Node contract{
            .id = *id,
            .label = name,
            .kind = kind,
            .confidence = Confidence::Extracted,
        };
        contract.properties.emplace("name", name);
        if (database_scoped(kind)) {
          contract.properties.emplace(
              "database", relation.target_label.empty() ? std::string(kLocalDatabase) : relation.target_label);
        }
        if (providing) {
          contract.source_file = source->second->source_file;
          contract.source_location = source->second->source_location;
          ++tally.contracts_provided;
        } else {
          contract.properties.emplace("served", "false");
          ++tally.contracts_external;
        }
        new_nodes.push_back(std::move(contract));
      }
      if (providing) {
        if (const auto file_id = file_of(*source->second)) {
          add_edge(*file_id, *id, "contains", "", {});
        }
        add_edge(*id, relation.source_id, kHandledBy, "", {});
      } else if (add_edge(relation.source_id, *id, kConsumes, "", {})) {
        ++tally.contract_consumes;
      }
    }
  }

  // Documented endpoints come from contract documents at extraction, so a route
  // or a consumer of the same id attached to them above rather than minting.
  for (const auto& node : graph.nodes) {
    if (node.kind == kEndpointKind && node.properties.contains("documented")) {
      ++tally.endpoints_documented;
    }
  }

  graph.nodes.insert(graph.nodes.end(), std::make_move_iterator(new_nodes.begin()),
                     std::make_move_iterator(new_nodes.end()));
  graph.edges.insert(graph.edges.end(), std::make_move_iterator(new_edges.begin()),
                     std::make_move_iterator(new_edges.end()));
}

}  // namespace cgraph
