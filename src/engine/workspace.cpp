#include "cgraph/workspace.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cgraph {
namespace {

constexpr std::string_view kEndpointPrefix = "endpoint:";
// Candidate contracts tried when bridging a `path` across two repos. The
// endpoints a source reaches are ordered by distance, so the nearest few are the
// ones a real call goes through; without a cap a hub function would ask its repo
// for hundreds of paths.
constexpr std::size_t kMaxBridgeCandidates = 8;
constexpr int kDefaultImpactDepth = 3;
constexpr std::size_t kDefaultImpactLimit = 50;

[[nodiscard]] nlohmann::json ok_response(nlohmann::json result) {
  return nlohmann::json{{"ok", true}, {"result", std::move(result)}};
}

[[nodiscard]] nlohmann::json error_response(std::string message, std::string code = {}) {
  nlohmann::json response{{"ok", false}, {"error", std::move(message)}};
  if (!code.empty()) {
    response["code"] = std::move(code);
  }
  return response;
}

[[nodiscard]] bool is_endpoint_id(const std::string& id) {
  return id.starts_with(kEndpointPrefix);
}

// The `result` object of a repo's answer, or nullptr when the repo failed.
[[nodiscard]] const nlohmann::json* result_of(const nlohmann::json& envelope) {
  if (!envelope.value("ok", false)) {
    return nullptr;
  }
  const auto found = envelope.find("result");
  return found != envelope.end() && found->is_object() ? &*found : nullptr;
}

// A repo answered "I have this node" when its result does not say `found:false`.
[[nodiscard]] bool found_in(const nlohmann::json& result) {
  return result.value("found", true);
}

struct RepoAnswer {
  const WorkspaceRepo* repo = nullptr;
  nlohmann::json envelope;
  bool reachable = false;
  std::string error;
};

// Asks every repo the same op, in manifest order.
[[nodiscard]] std::vector<RepoAnswer> ask_all(
    const Workspace& workspace, const std::string& op, const nlohmann::json& params, const RepoAsk& ask) {
  std::vector<RepoAnswer> answers;
  answers.reserve(workspace.repos.size());
  for (const auto& repo : workspace.repos) {
    RepoAnswer answer{.repo = &repo};
    std::string error;
    if (auto envelope = ask(repo, op, params, error)) {
      answer.envelope = std::move(*envelope);
      answer.reachable = true;
    } else {
      answer.error = error.empty() ? "daemon unreachable" : error;
    }
    answers.push_back(std::move(answer));
  }
  return answers;
}

// Every repo that could not be reached, or that answered with an error, as the
// `unreachable` array the caller must be able to see.
[[nodiscard]] nlohmann::json unreachable_of(const std::vector<RepoAnswer>& answers) {
  auto list = nlohmann::json::array();
  for (const auto& answer : answers) {
    if (!answer.reachable) {
      list.push_back({{"repo", answer.repo->name}, {"root", answer.repo->root.generic_string()}, {"error", answer.error}});
      continue;
    }
    if (!answer.envelope.value("ok", false)) {
      list.push_back({{"repo", answer.repo->name},
                      {"root", answer.repo->root.generic_string()},
                      {"error", answer.envelope.value("error", std::string{"request failed"})}});
    }
  }
  return list;
}

// Repos that answered from a graph they are still building. A cold daemon
// replies immediately with an empty graph rather than blocking, so without this
// a federated answer taken seconds after `workspace init` would silently be
// missing a whole repository's witnesses.
[[nodiscard]] nlohmann::json building_of(const std::vector<RepoAnswer>& answers) {
  auto list = nlohmann::json::array();
  for (const auto& answer : answers) {
    const auto* result = answer.reachable ? result_of(answer.envelope) : nullptr;
    if (result != nullptr && result->value("graph_state", std::string{}) == "building") {
      list.push_back({{"repo", answer.repo->name}, {"root", answer.repo->root.generic_string()}});
    }
  }
  return list;
}

void attach_repo_health(nlohmann::json& result, const std::vector<RepoAnswer>& answers) {
  if (auto list = unreachable_of(answers); !list.empty()) {
    result["unreachable"] = std::move(list);
  }
  if (auto list = building_of(answers); !list.empty()) {
    result["building"] = std::move(list);
    result["note"] = "a repo's graph is still building; its witnesses are missing - retry shortly or poll `workspace status`";
  }
}

[[nodiscard]] double centrality_of(const nlohmann::json& node) {
  const auto found = node.find("centrality");
  return found != node.end() && found->is_number() ? found->get<double>() : 0.0;
}

// ---- status ----------------------------------------------------------------

[[nodiscard]] nlohmann::json federate_status(const Workspace& workspace, const nlohmann::json& params, const RepoAsk& ask) {
  const auto answers = ask_all(workspace, "status", params, ask);
  auto repos = nlohmann::json::array();
  std::size_t nodes = 0;
  std::size_t edges = 0;
  std::size_t reachable = 0;
  for (const auto& answer : answers) {
    nlohmann::json entry{{"repo", answer.repo->name}, {"root", answer.repo->root.generic_string()}};
    if (const auto* result = answer.reachable ? result_of(answer.envelope) : nullptr) {
      ++reachable;
      nodes += result->value("node_count", std::size_t{0});
      edges += result->value("edge_count", std::size_t{0});
      for (const auto* key : {"node_count", "edge_count", "build_state", "uptime_seconds"}) {
        if (const auto value = result->find(key); value != result->end()) {
          entry[key] = *value;
        }
      }
    } else {
      entry["reachable"] = false;
      entry["error"] = answer.reachable ? answer.envelope.value("error", std::string{"request failed"}) : answer.error;
    }
    repos.push_back(std::move(entry));
  }
  nlohmann::json result{
      {"workspace", workspace.name},
      {"root", workspace.root.generic_string()},
      {"repos", std::move(repos)},
      {"totals", {{"repos", workspace.repos.size()}, {"reachable", reachable}, {"nodes", nodes}, {"edges", edges}}},
  };
  attach_repo_health(result, answers);
  return ok_response(std::move(result));
}

// ---- query -----------------------------------------------------------------

[[nodiscard]] nlohmann::json federate_query(const Workspace& workspace, const nlohmann::json& params, const RepoAsk& ask) {
  const auto answers = ask_all(workspace, "query", params, ask);
  std::vector<nlohmann::json> hits;
  std::size_t total = 0;
  std::set<std::string> routes;
  for (const auto& answer : answers) {
    const auto* result = answer.reachable ? result_of(answer.envelope) : nullptr;
    if (result == nullptr) {
      continue;
    }
    total += result->value("total", std::size_t{0});
    if (const auto route = result->find("route"); route != result->end() && route->is_string()) {
      routes.insert(route->get<std::string>());
    }
    if (const auto nodes = result->find("nodes"); nodes != result->end() && nodes->is_array()) {
      for (const auto& node : *nodes) {
        auto tagged = node;
        tagged["repo"] = answer.repo->name;
        hits.push_back(std::move(tagged));
      }
    }
  }
  // Each repo already ranked its own hits; across repos the only comparable
  // signal is centrality, so merge on that and break ties by name for a stable
  // order.
  std::ranges::stable_sort(hits, [](const nlohmann::json& lhs, const nlohmann::json& rhs) {
    const auto lc = centrality_of(lhs);
    const auto rc = centrality_of(rhs);
    if (lc != rc) {
      return lc > rc;
    }
    return lhs.value("label", std::string{}) < rhs.value("label", std::string{});
  });
  const auto limit = params.value("limit", std::size_t{0});
  const bool truncated = limit > 0 && hits.size() > limit;
  if (truncated) {
    hits.resize(limit);
  }
  nlohmann::json result{
      {"route", routes.size() == 1 ? *routes.begin() : std::string{"workspace"}},
      {"nodes", hits},
      {"total", total},
      {"returned", hits.size()},
  };
  if (truncated) {
    result["truncated"] = true;
  }
  attach_repo_health(result, answers);
  return ok_response(std::move(result));
}

// ---- explain ---------------------------------------------------------------

[[nodiscard]] nlohmann::json federate_explain(const Workspace& workspace, const nlohmann::json& params, const RepoAsk& ask) {
  const auto answers = ask_all(workspace, "explain", params, ask);
  auto searched = nlohmann::json::array();
  for (const auto& answer : answers) {
    searched.push_back(answer.repo->name);
    const auto* result = answer.reachable ? result_of(answer.envelope) : nullptr;
    if (result == nullptr || !found_in(*result)) {
      continue;
    }
    auto merged = *result;
    merged["repo"] = answer.repo->name;
    // An endpoint exists in every repo that serves or calls it; name the others
    // so the reader knows the contract is shared rather than local.
    auto also = nlohmann::json::array();
    for (const auto& other : answers) {
      if (other.repo == answer.repo) {
        continue;
      }
      const auto* other_result = other.reachable ? result_of(other.envelope) : nullptr;
      if (other_result != nullptr && found_in(*other_result)) {
        also.push_back(other.repo->name);
      }
    }
    if (!also.empty()) {
      merged["also_in"] = std::move(also);
    }
    attach_repo_health(merged, answers);
    return ok_response(std::move(merged));
  }
  nlohmann::json miss{{"id", params.value("id", std::string{})}, {"found", false}, {"repos_searched", std::move(searched)}};
  attach_repo_health(miss, answers);
  return ok_response(std::move(miss));
}

// ---- impact ----------------------------------------------------------------

struct Witness {
  nlohmann::json node;
  int depth = 0;
  std::string repo;
};

// Merges one repo's impact nodes into `witnesses`, keeping the shallowest depth
// for each (repo, id) pair.
void absorb(std::vector<Witness>& witnesses, std::unordered_map<std::string, std::size_t>& index,
            const nlohmann::json& nodes, const std::string& repo, int depth_offset, const std::string& via) {
  if (!nodes.is_array()) {
    return;
  }
  for (const auto& node : nodes) {
    const auto id = node.value("id", std::string{});
    if (id.empty()) {
      continue;
    }
    const auto depth = node.value("depth", 0) + depth_offset;
    const auto key = repo + '\n' + id;
    if (const auto found = index.find(key); found != index.end()) {
      if (depth < witnesses[found->second].depth) {
        witnesses[found->second].depth = depth;
      }
      continue;
    }
    auto tagged = node;
    tagged["repo"] = repo;
    if (depth_offset > 0 && !via.empty()) {
      tagged["bridged_through"] = via;
    }
    index.emplace(key, witnesses.size());
    witnesses.push_back(Witness{.node = std::move(tagged), .depth = depth, .repo = repo});
  }
}

[[nodiscard]] nlohmann::json federate_impact(const Workspace& workspace, const nlohmann::json& params, const RepoAsk& ask) {
  const auto seed = params.value("id", std::string{});
  const auto max_depth = std::max(0, params.value("max_depth", kDefaultImpactDepth));
  const auto limit = params.value("limit", kDefaultImpactLimit);
  const auto answers = ask_all(workspace, "impact", params, ask);

  std::vector<Witness> witnesses;
  std::unordered_map<std::string, std::size_t> index;
  auto owners = nlohmann::json::array();
  // endpoint id -> the shallowest depth any repo reached it at.
  std::map<std::string, int> contracts;
  if (is_endpoint_id(seed)) {
    contracts.emplace(seed, 0);
  }
  for (const auto& answer : answers) {
    const auto* result = answer.reachable ? result_of(answer.envelope) : nullptr;
    if (result == nullptr || !found_in(*result)) {
      continue;
    }
    owners.push_back(answer.repo->name);
    const auto nodes = result->find("nodes");
    if (nodes == result->end()) {
      continue;
    }
    absorb(witnesses, index, *nodes, answer.repo->name, 0, {});
    for (const auto& node : *nodes) {
      const auto id = node.value("id", std::string{});
      if (!is_endpoint_id(id)) {
        continue;
      }
      const auto depth = node.value("depth", 0);
      if (const auto slot = contracts.find(id); slot == contracts.end() || depth < slot->second) {
        contracts[id] = depth;
      }
    }
  }
  if (owners.empty()) {
    nlohmann::json miss{{"id", seed}, {"found", false}, {"direction", params.value("direction", std::string{"dependents"})},
                        {"max_depth", max_depth}, {"total", 0}, {"returned", 0}, {"nodes", nlohmann::json::array()}};
    attach_repo_health(miss, answers);
    return ok_response(std::move(miss));
  }

  // One hop per contract: every repo continues the traversal from each endpoint
  // the first pass reached, with the depth that remains.
  auto bridged = nlohmann::json::array();
  for (const auto& [endpoint, depth] : contracts) {
    const auto remaining = max_depth - depth;
    if (remaining <= 0) {
      continue;
    }
    auto forwarded = params;
    forwarded["id"] = endpoint;
    forwarded["max_depth"] = remaining;
    bool crossed = false;
    for (const auto& repo : workspace.repos) {
      std::string error;
      const auto envelope = ask(repo, "impact", forwarded, error);
      if (!envelope) {
        continue;  // already reported through the first pass's `unreachable`
      }
      const auto* result = result_of(*envelope);
      if (result == nullptr || !found_in(*result)) {
        continue;
      }
      const auto nodes = result->find("nodes");
      if (nodes == result->end()) {
        continue;
      }
      const auto before = witnesses.size();
      absorb(witnesses, index, *nodes, repo.name, depth, endpoint);
      crossed = crossed || witnesses.size() > before;
    }
    if (crossed) {
      bridged.push_back({{"endpoint", endpoint}, {"depth", depth}});
    }
  }

  // The seed is never a witness of itself, in any repo.
  std::erase_if(witnesses, [&](const Witness& witness) { return witness.node.value("id", std::string{}) == seed; });
  std::ranges::stable_sort(witnesses, [](const Witness& lhs, const Witness& rhs) {
    if (lhs.depth != rhs.depth) {
      return lhs.depth < rhs.depth;
    }
    const auto lc = centrality_of(lhs.node);
    const auto rc = centrality_of(rhs.node);
    if (lc != rc) {
      return lc > rc;
    }
    if (lhs.repo != rhs.repo) {
      return lhs.repo < rhs.repo;
    }
    return lhs.node.value("label", std::string{}) < rhs.node.value("label", std::string{});
  });
  const auto total = witnesses.size();
  const bool truncated = limit > 0 && witnesses.size() > limit;
  if (truncated) {
    witnesses.resize(limit);
  }
  auto nodes = nlohmann::json::array();
  for (auto& witness : witnesses) {
    witness.node["depth"] = witness.depth;
    nodes.push_back(std::move(witness.node));
  }
  nlohmann::json result{
      {"id", seed},
      {"direction", params.value("direction", std::string{"dependents"})},
      {"max_depth", max_depth},
      {"total", total},
      {"returned", nodes.size()},
      {"nodes", std::move(nodes)},
      {"repos", std::move(owners)},
  };
  if (!bridged.empty()) {
    result["bridged"] = std::move(bridged);
  }
  if (truncated) {
    result["truncated"] = true;
  }
  attach_repo_health(result, answers);
  return ok_response(std::move(result));
}

// ---- path ------------------------------------------------------------------

[[nodiscard]] bool has_path(const nlohmann::json& result) {
  const auto path = result.find("path");
  return path != result.end() && path->is_array() && !path->empty();
}

// The endpoints reachable from `id` in `repo`, nearest first, from one impact
// call. These are the contracts a cross-repo path can cross at.
[[nodiscard]] std::vector<std::string> reachable_contracts(
    const WorkspaceRepo& repo, const std::string& id, const std::string& direction, int max_depth, const RepoAsk& ask) {
  nlohmann::json params{{"id", id}, {"direction", direction}, {"max_depth", max_depth}, {"limit", 0}};
  std::string error;
  const auto envelope = ask(repo, "impact", params, error);
  if (!envelope) {
    return {};
  }
  const auto* result = result_of(*envelope);
  if (result == nullptr || !found_in(*result)) {
    return {};
  }
  std::vector<std::pair<int, std::string>> found;
  if (const auto nodes = result->find("nodes"); nodes != result->end() && nodes->is_array()) {
    for (const auto& node : *nodes) {
      const auto node_id = node.value("id", std::string{});
      if (is_endpoint_id(node_id)) {
        found.emplace_back(node.value("depth", 0), node_id);
      }
    }
  }
  std::ranges::sort(found);
  std::vector<std::string> ids;
  for (auto& [depth, endpoint] : found) {
    ids.push_back(std::move(endpoint));
    if (ids.size() >= kMaxBridgeCandidates) {
      break;
    }
  }
  return ids;
}

[[nodiscard]] nlohmann::json federate_path(const Workspace& workspace, const nlohmann::json& params, const RepoAsk& ask) {
  const auto source = params.value("source", std::string{});
  const auto target = params.value("target", std::string{});
  const auto answers = ask_all(workspace, "path", params, ask);

  // Both ends in one repo: that repo's answer is the answer.
  for (const auto& answer : answers) {
    const auto* result = answer.reachable ? result_of(answer.envelope) : nullptr;
    if (result != nullptr && has_path(*result)) {
      auto merged = *result;
      merged["repos"] = nlohmann::json::array({answer.repo->name});
      attach_repo_health(merged, answers);
      return ok_response(std::move(merged));
    }
  }

  // Otherwise bridge: a contract the source reaches in its repo and that reaches
  // the target in another. The depth bound is the caller's, applied per side.
  // `path` is undirected -- an endpoint points at its handler and is pointed at
  // by its callers, and a route between them exists either way -- so the
  // contracts a source can cross at are the ones it reaches in either direction.
  const auto max_depth = std::max(1, params.value("max_depth", kDefaultImpactDepth));
  for (const auto& from : workspace.repos) {
    const auto contracts = reachable_contracts(from, source, "both", max_depth, ask);
    if (contracts.empty()) {
      continue;
    }
    for (const auto& endpoint : contracts) {
      nlohmann::json head{{"source", source}, {"target", endpoint}};
      std::string error;
      const auto head_envelope = ask(from, "path", head, error);
      const auto* head_result = head_envelope ? result_of(*head_envelope) : nullptr;
      if (head_result == nullptr || !has_path(*head_result)) {
        continue;
      }
      for (const auto& to : workspace.repos) {
        if (to.name == from.name) {
          continue;
        }
        nlohmann::json tail{{"source", endpoint}, {"target", target}};
        const auto tail_envelope = ask(to, "path", tail, error);
        const auto* tail_result = tail_envelope ? result_of(*tail_envelope) : nullptr;
        if (tail_result == nullptr || !has_path(*tail_result)) {
          continue;
        }
        // Concatenate at the contract, which both sides name identically.
        auto path = (*head_result)["path"];
        auto path_nodes = (*head_result)["path_nodes"];
        for (auto& node : path_nodes) {
          node["repo"] = from.name;
        }
        const auto& tail_path = (*tail_result)["path"];
        const auto& tail_nodes = (*tail_result)["path_nodes"];
        for (std::size_t i = 1; i < tail_path.size(); ++i) {
          path.push_back(tail_path[i]);
        }
        for (std::size_t i = 1; i < tail_nodes.size(); ++i) {
          auto node = tail_nodes[i];
          node["repo"] = to.name;
          path_nodes.push_back(std::move(node));
        }
        nlohmann::json result{{"path", std::move(path)},
                              {"path_nodes", std::move(path_nodes)},
                              {"repos", nlohmann::json::array({from.name, to.name})},
                              {"bridged_through", endpoint}};
        attach_repo_health(result, answers);
        return ok_response(std::move(result));
      }
    }
  }

  nlohmann::json result{{"path", nlohmann::json::array()}, {"path_nodes", nlohmann::json::array()},
                        {"source", source}, {"target", target}};
  attach_repo_health(result, answers);
  return ok_response(std::move(result));
}

// ---- update ----------------------------------------------------------------

[[nodiscard]] nlohmann::json federate_update(const Workspace& workspace, const nlohmann::json& params, const RepoAsk& ask) {
  const auto answers = ask_all(workspace, "update", params, ask);
  auto repos = nlohmann::json::array();
  for (const auto& answer : answers) {
    nlohmann::json entry{{"repo", answer.repo->name}};
    if (const auto* result = answer.reachable ? result_of(answer.envelope) : nullptr) {
      entry["result"] = *result;
    } else {
      entry["error"] = answer.reachable ? answer.envelope.value("error", std::string{"request failed"}) : answer.error;
    }
    repos.push_back(std::move(entry));
  }
  nlohmann::json result{{"workspace", workspace.name}, {"repos", std::move(repos)}};
  attach_repo_health(result, answers);
  return ok_response(std::move(result));
}

}  // namespace

bool is_workspace_root(const std::filesystem::path& root) {
  std::error_code error;
  return std::filesystem::is_regular_file(root / std::filesystem::path(std::string(kWorkspaceFile)), error);
}

Workspace load_workspace(const std::filesystem::path& root) {
  Workspace workspace;
  std::error_code error;
  workspace.root = std::filesystem::weakly_canonical(root, error);
  if (error) {
    workspace.root = root;
  }
  const auto manifest_path = workspace.root / std::filesystem::path(std::string(kWorkspaceFile));
  std::ifstream input(manifest_path);
  if (!input) {
    workspace.errors.push_back("no workspace manifest at " + manifest_path.generic_string());
    return workspace;
  }
  nlohmann::json manifest;
  try {
    input >> manifest;
  } catch (const nlohmann::json::exception& exception) {
    workspace.errors.push_back("workspace manifest is malformed JSON: " + std::string(exception.what()));
    return workspace;
  }
  if (!manifest.is_object()) {
    workspace.errors.push_back("workspace manifest must be a JSON object");
    return workspace;
  }
  workspace.name = manifest.value("name", workspace.root.filename().generic_string());
  const auto repos = manifest.find("repos");
  if (repos == manifest.end() || !repos->is_array() || repos->empty()) {
    workspace.errors.push_back("workspace manifest needs a non-empty `repos` array");
    return workspace;
  }
  std::unordered_set<std::string> names;
  for (const auto& entry : *repos) {
    if (!entry.is_object()) {
      workspace.errors.push_back("each `repos` entry must be an object with `name` and `root`");
      continue;
    }
    WorkspaceRepo repo;
    repo.name = entry.value("name", std::string{});
    const auto declared = entry.value("root", std::string{});
    if (repo.name.empty() || declared.empty()) {
      workspace.errors.push_back("each `repos` entry needs a non-empty `name` and `root`");
      continue;
    }
    if (!names.insert(repo.name).second) {
      workspace.errors.push_back("duplicate repo name in the workspace manifest: " + repo.name);
      continue;
    }
    std::filesystem::path declared_path(declared);
    auto resolved = declared_path.is_absolute() ? declared_path : workspace.root / declared_path;
    resolved = std::filesystem::weakly_canonical(resolved, error);
    if (error) {
      resolved = (declared_path.is_absolute() ? declared_path : workspace.root / declared_path).lexically_normal();
    }
    if (!std::filesystem::is_directory(resolved, error)) {
      workspace.errors.push_back("repo '" + repo.name + "' root does not exist: " + resolved.generic_string());
      continue;
    }
    repo.root = std::move(resolved);
    workspace.repos.push_back(std::move(repo));
  }
  if (workspace.repos.empty() && workspace.errors.empty()) {
    workspace.errors.push_back("workspace manifest lists no usable repos");
  }
  if (!workspace.errors.empty()) {
    workspace.repos.clear();
  }
  return workspace;
}

nlohmann::json workspace_manifest_json(const Workspace& workspace) {
  auto repos = nlohmann::json::array();
  for (const auto& repo : workspace.repos) {
    // Relative when the repo sits under the workspace, so moving the checkout
    // does not invalidate the manifest.
    const auto relative = repo.root.lexically_relative(workspace.root);
    const auto stored = !relative.empty() && *relative.begin() != ".."
                            ? "./" + relative.generic_string()
                            : repo.root.generic_string();
    repos.push_back({{"name", repo.name}, {"root", stored}});
  }
  return nlohmann::json{{"name", workspace.name}, {"repos", std::move(repos)}};
}

std::vector<WorkspaceRepo> discover_workspace_repos(const std::filesystem::path& root) {
  std::vector<WorkspaceRepo> repos;
  std::error_code error;
  std::filesystem::directory_iterator entries(root, error);
  if (error) {
    return repos;
  }
  for (const auto& entry : entries) {
    if (!entry.is_directory(error) || error) {
      continue;
    }
    const auto name = entry.path().filename().generic_string();
    if (name.empty() || name.front() == '.') {
      continue;
    }
    if (!std::filesystem::exists(entry.path() / ".git", error)) {
      continue;
    }
    auto canonical = std::filesystem::weakly_canonical(entry.path(), error);
    repos.push_back(WorkspaceRepo{.name = name, .root = error ? entry.path() : canonical});
  }
  std::ranges::sort(repos, [](const WorkspaceRepo& lhs, const WorkspaceRepo& rhs) { return lhs.name < rhs.name; });
  return repos;
}

nlohmann::json federate_workspace_request(
    const Workspace& workspace, const std::string& op, const nlohmann::json& params, const RepoAsk& ask) {
  if (!workspace.ok()) {
    return error_response(workspace.errors.front(), "workspace_invalid");
  }
  if (!ask) {
    return error_response("workspace federation needs a repo transport", "workspace_no_transport");
  }
  if (op == "status") {
    return federate_status(workspace, params, ask);
  }
  if (op == "query") {
    return federate_query(workspace, params, ask);
  }
  if (op == "explain") {
    return federate_explain(workspace, params, ask);
  }
  if (op == "impact") {
    return federate_impact(workspace, params, ask);
  }
  if (op == "path") {
    return federate_path(workspace, params, ask);
  }
  if (op == "update") {
    return federate_update(workspace, params, ask);
  }
  // Everything else is answered about one project: a report's layers, a
  // context's token budget and a memory checkpoint all belong to a repo, and
  // merging them would invent a thing the caller did not ask for.
  std::string roots;
  for (const auto& repo : workspace.repos) {
    roots += (roots.empty() ? "" : ", ") + repo.name + "=" + repo.root.generic_string();
  }
  return error_response("op '" + op + "' is answered per repository, not across a workspace; run it with --root on one of: " + roots,
                        "workspace_op_unsupported");
}

}  // namespace cgraph
