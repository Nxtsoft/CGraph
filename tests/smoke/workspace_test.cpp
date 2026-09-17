#include "cgraph/workspace.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

int fail(std::string_view message) {
  std::cerr << "workspace_test: " << message << '\n';
  return 1;
}

void write_file(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << contents;
}

// A transport over canned per-repo answers: `answers[repo][op]` is the envelope
// that repo returns, and a repo listed in `down` is unreachable.
struct FakeRepos {
  std::map<std::string, std::map<std::string, json>> answers;
  std::vector<std::string> down;
  // Every (repo, op, id) asked, in order, so a test can assert what was forwarded.
  mutable std::vector<std::string> calls;

  [[nodiscard]] cgraph::RepoAsk ask() const {
    return [this](const cgraph::WorkspaceRepo& repo, const std::string& op, const json& params,
                  std::string& error) -> std::optional<json> {
      // A path is keyed by both ends, since federation asks the same repo for the
      // direct path and for the leg up to a contract.
      const auto id = op == "path"
                          ? params.value("source", std::string{}) + "->" + params.value("target", std::string{})
                          : params.value("id", std::string{});
      calls.push_back(repo.name + ":" + op + (id.empty() ? "" : ":" + id));
      if (std::find(down.begin(), down.end(), repo.name) != down.end()) {
        error = "daemon unreachable";
        return std::nullopt;
      }
      const auto by_repo = answers.find(repo.name);
      if (by_repo == answers.end()) {
        return std::optional<json>{json{{"ok", true}, {"result", {{"found", false}}}}};
      }
      // An op key may be qualified by the id it was asked about, so one repo can
      // answer differently for the seed and for the bridged endpoint.
      if (!id.empty()) {
        if (const auto exact = by_repo->second.find(op + ":" + id); exact != by_repo->second.end()) {
          return std::optional<json>{exact->second};
        }
      }
      const auto by_op = by_repo->second.find(op);
      return by_op == by_repo->second.end() ? std::optional<json>{json{{"ok", true}, {"result", {{"found", false}}}}}
                                            : std::optional<json>{by_op->second};
    };
  }
};

json impact_ok(const std::vector<json>& nodes) {
  return json{{"ok", true}, {"result", {{"found", true}, {"nodes", nodes}, {"total", nodes.size()}}}};
}

json node(const std::string& id, int depth, const std::string& label = {}) {
  return json{{"id", id}, {"label", label.empty() ? id : label}, {"kind", "function"}, {"depth", depth}};
}

cgraph::Workspace workspace_of(const fs::path& root, const std::vector<std::pair<std::string, fs::path>>& repos) {
  cgraph::Workspace workspace;
  workspace.root = root;
  workspace.name = "test";
  for (const auto& [name, repo_root] : repos) {
    workspace.repos.push_back(cgraph::WorkspaceRepo{.name = name, .root = repo_root});
  }
  return workspace;
}

int test_manifest(const fs::path& root) {
  const auto ws = root / "ws";
  fs::create_directories(ws / "api");
  fs::create_directories(ws / "web");
  if (cgraph::is_workspace_root(ws)) {
    return fail("a directory without the manifest is not a workspace");
  }
  write_file(ws / std::string(cgraph::kWorkspaceFile),
             R"({"name": "turing", "repos": [{"name": "api", "root": "./api"}, {"name": "web", "root": "./web"}]})");
  if (!cgraph::is_workspace_root(ws)) {
    return fail("a directory with the manifest is a workspace");
  }
  const auto loaded = cgraph::load_workspace(ws);
  if (!loaded.ok() || loaded.name != "turing" || loaded.repos.size() != 2 || loaded.repos[0].name != "api" ||
      loaded.repos[1].root.filename() != "web") {
    return fail("a valid manifest loads its repos in order, roots resolved against the workspace");
  }
  // Round trip: relative roots stay relative so the checkout can move.
  const auto manifest = cgraph::workspace_manifest_json(loaded);
  if (manifest["repos"][0]["root"] != "./api" || manifest["name"] != "turing") {
    std::cerr << manifest.dump() << '\n';
    return fail("repo roots under the workspace are stored relative");
  }
  // Each failure mode is a loud error and no repos.
  const auto missing = cgraph::load_workspace(root / "nope");
  write_file(ws / "bad" / std::string(cgraph::kWorkspaceFile), "{not json");
  const auto malformed = cgraph::load_workspace(ws / "bad");
  write_file(ws / "empty" / std::string(cgraph::kWorkspaceFile), R"({"repos": []})");
  const auto empty = cgraph::load_workspace(ws / "empty");
  write_file(ws / "dup" / std::string(cgraph::kWorkspaceFile),
             R"({"repos": [{"name": "a", "root": "../api"}, {"name": "a", "root": "../web"}]})");
  const auto duplicate = cgraph::load_workspace(ws / "dup");
  write_file(ws / "gone" / std::string(cgraph::kWorkspaceFile), R"({"repos": [{"name": "a", "root": "./nowhere"}]})");
  const auto absent = cgraph::load_workspace(ws / "gone");
  for (const auto* invalid : {&missing, &malformed, &empty, &duplicate, &absent}) {
    if (invalid->ok() || !invalid->repos.empty() || invalid->errors.empty()) {
      return fail("a missing, malformed, empty, duplicate-named or dangling manifest is an error with no repos");
    }
  }
  // An invalid workspace refuses every op rather than answering for some repos.
  FakeRepos repos;
  const auto refused = cgraph::federate_workspace_request(malformed, "query", json::object(), repos.ask());
  if (refused.value("ok", true) || refused.value("code", std::string{}) != "workspace_invalid") {
    return fail("an invalid workspace is a typed error");
  }
  // Discovery finds git repos one level down, sorted, and skips dotted dirs.
  fs::create_directories(ws / "api" / ".git");
  fs::create_directories(ws / "web" / ".git");
  fs::create_directories(ws / ".hidden" / ".git");
  fs::create_directories(ws / "notarepo");
  const auto found = cgraph::discover_workspace_repos(ws);
  if (found.size() != 2 || found[0].name != "api" || found[1].name != "web") {
    return fail("discovery finds the two git repos, skipping dotted and non-repo directories");
  }
  return 0;
}

int test_status_and_unreachable(const fs::path& root) {
  const auto workspace = workspace_of(root, {{"api", root / "api"}, {"web", root / "web"}});
  FakeRepos repos;
  repos.answers["api"]["status"] = json{{"ok", true}, {"result", {{"node_count", 12000}, {"edge_count", 23000}, {"build_state", "ready"}}}};
  repos.down.push_back("web");
  const auto response = cgraph::federate_workspace_request(workspace, "status", json::object(), repos.ask());
  if (!response.value("ok", false)) {
    return fail("status answers even when a repo is down");
  }
  const auto& result = response["result"];
  if (result["totals"]["repos"] != 2 || result["totals"]["reachable"] != 1 || result["totals"]["nodes"] != 12000) {
    return fail("status totals count only reachable repos");
  }
  if (!result.contains("unreachable") || result["unreachable"].size() != 1 ||
      result["unreachable"][0]["repo"] != "web" || result["repos"][1]["reachable"] != false) {
    std::cerr << result.dump(2) << '\n';
    return fail("a repo whose daemon is down is reported, never dropped");
  }

  // A cold daemon answers at once from an empty graph rather than blocking, so a
  // federated answer says which repos were still building.
  FakeRepos cold;
  cold.answers["api"]["query"] = json{{"ok", true}, {"result", {{"total", 1}, {"nodes", {{{"id", "a1"}}}}}}};
  cold.answers["web"]["query"] = json{
      {"ok", true},
      {"result", {{"total", 0}, {"nodes", json::array()}, {"graph_state", "building"}}}};
  const auto warming = cgraph::federate_workspace_request(workspace, "query", json::object(), cold.ask());
  const auto& partial = warming["result"];
  if (!partial.contains("building") || partial["building"].size() != 1 || partial["building"][0]["repo"] != "web" ||
      partial.value("note", std::string{}).find("still building") == std::string::npos) {
    std::cerr << partial.dump(2) << '\n';
    return fail("a repo still building its graph is named, so a partial answer cannot look total");
  }
  return 0;
}

int test_query_and_explain(const fs::path& root) {
  const auto workspace = workspace_of(root, {{"api", root / "api"}, {"web", root / "web"}});
  FakeRepos repos;
  repos.answers["api"]["query"] = json{
      {"ok", true},
      {"result", {{"route", "search"}, {"total", 2}, {"nodes", {{{"id", "a1"}, {"label", "listNotebooks"}, {"centrality", 0.4}}, {{"id", "a2"}, {"label", "helper"}, {"centrality", 0.1}}}}}}};
  repos.answers["web"]["query"] = json{
      {"ok", true},
      {"result", {{"route", "search"}, {"total", 1}, {"nodes", {{{"id", "w1"}, {"label", "useNotebooks"}, {"centrality", 0.9}}}}}}};
  const auto merged = cgraph::federate_workspace_request(workspace, "query", json{{"limit", 2}}, repos.ask());
  const auto& result = merged["result"];
  if (result["total"] != 3 || result["returned"] != 2 || result["truncated"] != true) {
    return fail("query sums totals across repos and truncates the merge to the caller's limit");
  }
  if (result["nodes"][0]["repo"] != "web" || result["nodes"][0]["id"] != "w1" || result["nodes"][1]["repo"] != "api") {
    std::cerr << result.dump(2) << '\n';
    return fail("merged hits are ranked by centrality and tagged with their repo");
  }

  // explain: the repo that has the node answers; an endpoint that exists in both
  // names the other. A node no repo has is a loud miss listing the repos tried.
  repos.answers["api"]["explain"] = json{{"ok", true}, {"result", {{"found", true}, {"node", {{"id", "endpoint:GET /x"}}}}}};
  repos.answers["web"]["explain"] = json{{"ok", true}, {"result", {{"found", true}, {"node", {{"id", "endpoint:GET /x"}}}}}};
  const auto shared = cgraph::federate_workspace_request(workspace, "explain", json{{"id", "endpoint:GET /x"}}, repos.ask());
  if (shared["result"]["repo"] != "api" || shared["result"]["also_in"] != json::array({"web"})) {
    return fail("a contract node reports the first repo that has it and the others that share it");
  }
  FakeRepos none;
  const auto miss = cgraph::federate_workspace_request(workspace, "explain", json{{"id", "ghost"}}, none.ask());
  if (miss["result"]["found"] != false || miss["result"]["repos_searched"] != json::array({"api", "web"})) {
    return fail("a node no repo has is a miss naming every repo searched");
  }
  return 0;
}

// The CGR-14 acceptance: impact on a handler in one repo reaches the consumer in
// another, through the endpoint node both graphs share.
int test_impact_bridges_the_contract(const fs::path& root) {
  const auto workspace = workspace_of(root, {{"api", root / "api"}, {"web", root / "web"}});
  const std::string handler = "api::starredHandler";
  const std::string endpoint = "endpoint:GET /api/v1/notebooks/starred-notes";
  FakeRepos repos;
  // The api graph: the handler's dependents are its file and the endpoint.
  repos.answers["api"]["impact:" + handler] = impact_ok({node("api::file", 1), node(endpoint, 1)});
  // Asked about the endpoint, the api has nothing new (it reaches the handler,
  // which the seed already is).
  repos.answers["api"]["impact:" + endpoint] = impact_ok({node(handler, 1)});
  // The web graph does not have the handler at all, but does have the endpoint:
  // its dependents are the API object and the hooks that import it.
  repos.answers["web"]["impact:" + handler] = json{{"ok", true}, {"result", {{"found", false}}}};
  repos.answers["web"]["impact:" + endpoint] = impact_ok({node("web::notebooksApi", 1), node("web::useStarred", 2)});

  const auto response = cgraph::federate_workspace_request(
      workspace, "impact", json{{"id", handler}, {"direction", "dependents"}, {"max_depth", 3}}, repos.ask());
  if (!response.value("ok", false)) {
    return fail("impact answers");
  }
  const auto& result = response["result"];
  if (result["repos"] != json::array({"api"})) {
    return fail("the repo that owns the seed is named");
  }
  std::map<std::string, std::pair<int, std::string>> reached;  // id -> (depth, repo)
  for (const auto& hit : result["nodes"]) {
    reached[hit["id"]] = {hit["depth"], hit.value("repo", std::string{})};
  }
  if (reached.count("web::notebooksApi") == 0 || reached["web::notebooksApi"] != std::pair<int, std::string>{2, "web"} ||
      reached.count("web::useStarred") == 0 || reached["web::useStarred"] != std::pair<int, std::string>{3, "web"}) {
    std::cerr << result.dump(2) << '\n';
    return fail("the consumer in the other repo is reached through the endpoint, at the endpoint's depth plus its own");
  }
  if (reached[endpoint] != std::pair<int, std::string>{1, "api"} ||
      reached.count("api::file") == 0) {
    return fail("the endpoint and the owning repo's own witnesses are kept");
  }
  // The bridge is reported, and the crossing witnesses name the contract.
  if (result["bridged"].size() != 1 || result["bridged"][0]["endpoint"] != endpoint || result["bridged"][0]["depth"] != 1) {
    return fail("the contract the traversal crossed is named");
  }
  for (const auto& hit : result["nodes"]) {
    if (hit["id"] == "web::useStarred" && hit.value("bridged_through", std::string{}) != endpoint) {
      return fail("a witness found across the contract records which contract it came through");
    }
  }
  // The seed is not a witness of itself, in any repo.
  if (reached.count(handler) != 0) {
    return fail("the seed is not its own witness");
  }
  // Depth is respected: with only one hop of budget the bridge is not taken.
  const auto shallow = cgraph::federate_workspace_request(
      workspace, "impact", json{{"id", handler}, {"direction", "dependents"}, {"max_depth", 1}}, repos.ask());
  for (const auto& hit : shallow["result"]["nodes"]) {
    if (hit.value("repo", std::string{}) == "web") {
      return fail("the endpoint at depth 1 with max_depth 1 leaves no budget to cross");
    }
  }
  // A seed no repo has is a miss, not an empty success.
  const auto ghost = cgraph::federate_workspace_request(workspace, "impact", json{{"id", "ghost"}}, repos.ask());
  if (ghost["result"]["found"] != false || ghost["result"]["total"] != 0) {
    return fail("a seed no repo has is a miss");
  }
  return 0;
}

int test_path_bridges_and_unsupported_ops(const fs::path& root) {
  const auto workspace = workspace_of(root, {{"api", root / "api"}, {"web", root / "web"}});
  const std::string endpoint = "endpoint:GET /api/v1/notebooks";
  FakeRepos repos;
  // Neither repo has both ends, so the direct path fails in both.
  repos.answers["api"]["path"] = json{{"ok", true}, {"result", {{"path", json::array()}, {"path_nodes", json::array()}}}};
  repos.answers["web"]["path"] = json{{"ok", true}, {"result", {{"path", json::array()}, {"path_nodes", json::array()}}}};
  // The api reaches the endpoint from the handler...
  repos.answers["api"]["impact:api::handler"] = impact_ok({node(endpoint, 1)});
  repos.answers["api"]["path:api::handler->" + endpoint] =
      json{{"ok", true},
           {"result", {{"path", {"api::handler", endpoint}}, {"path_nodes", {{{"id", "api::handler"}}, {{"id", endpoint}}}}}}};
  // ...and the web reaches the hook from the endpoint.
  repos.answers["web"]["path:" + endpoint + "->web::useNotebooks"] =
      json{{"ok", true},
           {"result", {{"path", {endpoint, "web::useNotebooks"}}, {"path_nodes", {{{"id", endpoint}}, {{"id", "web::useNotebooks"}}}}}}};
  const auto response = cgraph::federate_workspace_request(
      workspace, "path", json{{"source", "api::handler"}, {"target", "web::useNotebooks"}}, repos.ask());
  const auto& result = response["result"];
  if (result["path"] != json::array({"api::handler", endpoint, "web::useNotebooks"})) {
    std::cerr << result.dump(2) << '\n';
    return fail("a cross-repo path joins at the contract, naming it once");
  }
  if (result["bridged_through"] != endpoint || result["repos"] != json::array({"api", "web"})) {
    return fail("the crossing contract and both repos are named");
  }
  if (result["path_nodes"][0]["repo"] != "api" || result["path_nodes"][2]["repo"] != "web") {
    return fail("each path node says which repo it is in");
  }
  // No contract in common: an empty path, not a fabricated one.
  FakeRepos apart;
  apart.answers["api"]["path"] = json{{"ok", true}, {"result", {{"path", json::array()}, {"path_nodes", json::array()}}}};
  const auto none = cgraph::federate_workspace_request(
      workspace, "path", json{{"source", "a"}, {"target", "b"}}, apart.ask());
  if (!none["result"]["path"].empty()) {
    return fail("two repos with no shared contract have no path");
  }

  // update fans out; report/context/remember are per repo and say so.
  FakeRepos updates;
  updates.answers["api"]["update"] = json{{"ok", true}, {"result", {{"changed", 3}}}};
  updates.answers["web"]["update"] = json{{"ok", true}, {"result", {{"changed", 0}}}};
  const auto updated = cgraph::federate_workspace_request(workspace, "update", json::object(), updates.ask());
  if (updated["result"]["repos"].size() != 2 || updated["result"]["repos"][0]["result"]["changed"] != 3) {
    return fail("update reaches every repo and reports each");
  }
  for (const auto* op : {"report", "context", "remember", "recall", "shutdown"}) {
    const auto refused = cgraph::federate_workspace_request(workspace, op, json::object(), repos.ask());
    if (refused.value("ok", true) || refused.value("code", std::string{}) != "workspace_op_unsupported" ||
        refused.value("error", std::string{}).find("api=") == std::string::npos) {
      return fail(std::string("op '") + op + "' is refused at a workspace root with the repo roots to use instead");
    }
  }
  return 0;
}

}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / "cgraph-workspace-test";
  fs::remove_all(root);
  fs::create_directories(root / "api");
  fs::create_directories(root / "web");

  int failures = 0;
  failures += test_manifest(root);
  failures += test_status_and_unreachable(root);
  failures += test_query_and_explain(root);
  failures += test_impact_bridges_the_contract(root);
  failures += test_path_bridges_and_unsupported_ops(root);

  fs::remove_all(root);
  return failures == 0 ? 0 : 1;
}
