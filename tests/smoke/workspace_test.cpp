#include "cgraph/workspace.hpp"

#include <algorithm>
#include <cstdlib>
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
  if (result["bridged"].size() != 1 || result["bridged"][0]["endpoint"] != endpoint || result["bridged"][0]["depth"] != 1 ||
      result["bridged"][0].value("contract", std::string{}) != endpoint) {
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

// A project root finds the workspace that lists it (or a member that contains
// it, as a nested worktree does), and the home repo is answered from that root.
// A directory the manifest does not list, or a manifest above $HOME, is not used.
int test_enclosing_workspace(const fs::path& root) {
  int failures = 0;
  const auto ws = root / "enclosing";
  write_file(ws / "api" / "src" / "a.ts", "export const a = 1;\n");
  write_file(ws / "web" / "src" / "w.ts", "export const w = 1;\n");
  write_file(ws / "stray" / "src" / "s.ts", "export const s = 1;\n");
  write_file(ws / std::string(cgraph::kWorkspaceFile),
             R"({"name": "shop", "repos": [{"name": "api", "root": "./api"}, {"name": "web", "root": "./web"}]})");
  const auto canonical = [](const fs::path& path) { return fs::weakly_canonical(path); };
  const auto root_of = [](const cgraph::EnclosingWorkspace& found, const std::string& name) {
    for (const auto& repo : found.workspace.repos) {
      if (repo.name == name) return repo.root;
    }
    return fs::path{};
  };

  const auto member = cgraph::find_enclosing_workspace(ws / "api");
  if (!member || member->home != "api" || member->workspace.name != "shop" ||
      root_of(*member, "api") != canonical(ws / "api") || root_of(*member, "web") != canonical(ws / "web")) {
    std::cerr << "enclosing: a member root did not find its workspace\n";
    ++failures;
  }
  const auto nested = ws / "api" / ".agents" / "worktrees" / "feature";
  fs::create_directories(nested);
  const auto worktree = cgraph::find_enclosing_workspace(nested);
  if (!worktree || worktree->home != "api" || root_of(*worktree, "api") != canonical(nested)) {
    std::cerr << "enclosing: a worktree nested in a member was not answered from its own root\n";
    ++failures;
  }
  if (cgraph::find_enclosing_workspace(ws / "stray")) {
    std::cerr << "enclosing: a directory the manifest does not list joined the workspace\n";
    ++failures;
  }
  // A member reached through a symlink still finds the workspace it sits in.
  fs::create_directories(root / "code");
  write_file(root / "code" / "shared" / "src" / "x.ts", "export const x = 1;\n");
  const auto linked_ws = root / "linked";
  fs::create_directories(linked_ws);
  fs::create_directory_symlink(root / "code" / "shared", linked_ws / "shared");
  write_file(linked_ws / std::string(cgraph::kWorkspaceFile),
             R"({"name": "linked", "repos": [{"name": "shared", "root": "./shared"}]})");
  const auto linked = cgraph::find_enclosing_workspace(linked_ws / "shared");
  if (!linked || linked->home != "shared" || linked->workspace.name != "linked") {
    std::cerr << "enclosing: a symlinked member did not find its workspace\n";
    ++failures;
  }
  // When one member's root holds another's, the more specific member is home.
  const auto nested_ws = root / "platform";
  write_file(nested_ws / "services" / "api" / "src" / "a.ts", "export const a = 1;\n");
  write_file(nested_ws / std::string(cgraph::kWorkspaceFile),
             R"({"name": "platform", "repos": [{"name": "all", "root": "."}, {"name": "api", "root": "./services/api"}]})");
  const auto specific = cgraph::find_enclosing_workspace(nested_ws / "services" / "api");
  if (!specific || specific->home != "api") {
    std::cerr << "enclosing: the broader member won over the specific one\n";
    ++failures;
  }
  // A manifest that lists the root but names a member not present here is
  // returned with its errors, never skipped as if the root were alone.
  const auto partial_ws = root / "partial";
  write_file(partial_ws / "api" / "src" / "a.ts", "export const a = 1;\n");
  write_file(partial_ws / std::string(cgraph::kWorkspaceFile),
             R"({"name": "partial", "repos": [{"name": "api", "root": "./api"}, {"name": "billing", "root": "./billing"}]})");
  const auto partial = cgraph::find_enclosing_workspace(partial_ws / "api");
  if (!partial || partial->workspace.ok() || partial->home != "api" || partial->workspace.name != "partial") {
    std::cerr << "enclosing: an unusable manifest that lists the root was skipped\n";
    ++failures;
  }
  // A manifest above $HOME is outside the search.
  const char* saved = std::getenv("HOME");
  const std::string saved_home = saved == nullptr ? "" : saved;
  ::setenv("HOME", (ws / "api").c_str(), 1);
  if (cgraph::find_enclosing_workspace(nested)) {
    std::cerr << "enclosing: the search climbed above $HOME\n";
    ++failures;
  }
  // $HOME spelled through a symlink (macOS's /var -> /private/var) still stops
  // the walk along the path as given: the manifest above it is not used.
  fs::create_directories(root / "code" / "shared" / "sub");
  ::setenv("HOME", (linked_ws / "shared").c_str(), 1);
  if (cgraph::find_enclosing_workspace(linked_ws / "shared" / "sub")) {
    std::cerr << "enclosing: a symlinked $HOME did not stop the search\n";
    ++failures;
  }
  if (saved == nullptr) {
    ::unsetenv("HOME");
  } else {
    ::setenv("HOME", saved_home.c_str(), 1);
  }
  return failures;
}

}  // namespace

// A manifest's `prefixes` load, round-trip, and are validated against the
// member names.
int test_manifest_prefixes(const fs::path& root) {
  const auto ws = root / "ws-prefixes";
  fs::create_directories(ws / "idp");
  fs::create_directories(ws / "web");
  write_file(ws / std::string(cgraph::kWorkspaceFile),
             R"({"name": "modsquad", "repos": [{"name": "idp", "root": "./idp"}, {"name": "web", "root": "./web"}],
                 "prefixes": [{"repo": "web", "from": "/api/backend/", "to": "/api"}]})");
  const auto loaded = cgraph::load_workspace(ws);
  if (!loaded.ok() || loaded.prefixes.size() != 1 || loaded.prefixes[0].repo != "web" ||
      loaded.prefixes[0].from != "/api/backend" || loaded.prefixes[0].to != "/api") {
    return fail("a manifest's prefixes load, normalized");
  }
  if (cgraph::workspace_manifest_json(loaded)["prefixes"] !=
      json::array({{{"repo", "web"}, {"from", "/api/backend"}, {"to", "/api"}}})) {
    return fail("prefixes round-trip through the manifest JSON");
  }
  write_file(ws / "stranger" / std::string(cgraph::kWorkspaceFile),
             R"({"repos": [{"name": "idp", "root": "../idp"}], "prefixes": [{"repo": "web", "from": "/api/backend", "to": "/api"}]})");
  write_file(ws / "relative" / std::string(cgraph::kWorkspaceFile),
             R"({"repos": [{"name": "idp", "root": "../idp"}], "prefixes": [{"repo": "idp", "from": "api/backend", "to": "/api"}]})");
  // Non-string members are manifest errors, never a JSON type exception.
  write_file(ws / "typed-prefix" / std::string(cgraph::kWorkspaceFile),
             R"({"repos": [{"name": "idp", "root": "../idp"}], "prefixes": [{"repo": 7, "from": ["/api"], "to": "/api"}]})");
  write_file(ws / "typed-repo" / std::string(cgraph::kWorkspaceFile), R"({"repos": [{"name": 5, "root": ["../idp"]}]})");
  write_file(ws / "typed-name" / std::string(cgraph::kWorkspaceFile), R"({"name": 3, "repos": [{"name": "idp", "root": "../idp"}]})");
  for (const auto* name : {"stranger", "relative", "typed-prefix", "typed-repo", "typed-name"}) {
    try {
      const auto invalid = cgraph::load_workspace(ws / name);
      if (invalid.ok() || !invalid.repos.empty() || !invalid.prefixes.empty()) {
        return fail(std::string("a prefix naming no member, a relative path, or a non-string member is a manifest error: ") + name);
      }
    } catch (const std::exception& error) {
      return fail(std::string("a malformed manifest threw instead of reporting an error: ") + name + ": " + error.what());
    }
  }
  return 0;
}

json endpoint_brief(const std::string& id, int depth, bool served) {
  json brief{{"id", id}, {"label", id.substr(9)}, {"kind", "endpoint"}, {"depth", depth}};
  if (served) {
    brief["source_file"] = "/src/handler.ts";
  }
  return brief;
}

json explain_of(const std::string& id, bool served) {
  json result{{"id", id}, {"label", id.substr(9)}, {"kind", "endpoint"}, {"neighbors", json::array()}};
  if (served) {
    result["source_file"] = "/src/route.ts";
  }
  return json{{"ok", true}, {"result", result}};
}

// impact crosses a proxy prefix both ways, only through an endpoint the front
// end consumes without serving; path joins across it keeping both spellings.
int test_impact_and_path_cross_a_proxy_prefix(const fs::path& root) {
  auto workspace = workspace_of(root, {{"idp", root / "api"}, {"web", root / "web"}});
  workspace.prefixes.push_back(cgraph::EndpointPrefix{.repo = "web", .from = "/api/backend", .to = "/api"});
  const std::string provided = "endpoint:PATCH /api/v1/sessions/{}/extend";
  const std::string proxied = "endpoint:PATCH /api/backend/v1/sessions/{}/extend";
  const std::string backend_health = "endpoint:GET /api/healthz";
  const std::string local_health = "endpoint:GET /api/backend/healthz";
  FakeRepos repos;
  // idp: the controller method's dependents are both served endpoints.
  repos.answers["idp"]["impact:idp::extendSession"] =
      impact_ok({endpoint_brief(provided, 1, true), endpoint_brief(backend_health, 1, true)});
  // web: the hook consumes the proxied spelling; web serves /api/backend/healthz itself.
  repos.answers["web"]["explain:" + proxied] = explain_of(proxied, false);
  repos.answers["web"]["explain:" + local_health] = explain_of(local_health, true);
  repos.answers["web"]["impact:" + proxied] = impact_ok({node("web::useExtendSession", 1)});
  repos.answers["web"]["impact:" + local_health] = impact_ok({node("web::healthzRoute", 1)});
  const auto response = cgraph::federate_workspace_request(
      workspace, "impact", json{{"id", "idp::extendSession"}, {"direction", "dependents"}, {"max_depth", 3}}, repos.ask());
  std::map<std::string, json> reached;
  for (const auto& hit : response["result"]["nodes"]) {
    reached[hit["id"]] = hit;
  }
  if (reached.count("web::useExtendSession") == 0 || reached["web::useExtendSession"]["depth"] != 2 ||
      reached["web::useExtendSession"].value("bridged_through", std::string{}) != provided) {
    std::cerr << response.dump(2) << '\n';
    return fail("impact crosses from the provider to the consumer's proxied spelling");
  }
  if (reached.count("web::healthzRoute") != 0) {
    return fail("a route the front end serves itself is not reached through the proxy");
  }

  // The other way: the hook reaches the placeholder, which crosses to idp.
  repos.answers["web"]["impact:web::useExtendSession"] = impact_ok({endpoint_brief(proxied, 1, false)});
  repos.answers["idp"]["impact:" + provided] = impact_ok({node("idp::extendSession", 1)});
  // web also serves a route at the proxied path; the proxy never forwards to it.
  repos.answers["web"]["impact:" + provided] = impact_ok({node("web::ownRouteAtProxiedPath", 1)});
  const auto reverse = cgraph::federate_workspace_request(
      workspace, "impact", json{{"id", "web::useExtendSession"}, {"direction", "dependencies"}, {"max_depth", 3}}, repos.ask());
  bool crossed = false;
  for (const auto& hit : reverse["result"]["nodes"]) {
    crossed = crossed || (hit["id"] == "idp::extendSession" && hit.value("repo", std::string{}) == "idp" &&
                          hit.value("bridged_through", std::string{}) == provided);
  }
  if (!crossed) {
    std::cerr << reverse.dump(2) << '\n';
    return fail("impact crosses from the consumer's proxied spelling to the provider");
  }
  for (const auto& hit : reverse["result"]["nodes"]) {
    if (hit["id"] == "web::ownRouteAtProxiedPath") {
      return fail("a contract reached through a repo's proxy is not asked back of that repo");
    }
  }
  // Without the prefix the two spellings never meet.
  const auto plain = cgraph::federate_workspace_request(
      workspace_of(root, {{"idp", root / "api"}, {"web", root / "web"}}), "impact",
      json{{"id", "web::useExtendSession"}, {"direction", "dependencies"}, {"max_depth", 3}}, repos.ask());
  for (const auto& hit : plain["result"]["nodes"]) {
    if (hit.value("repo", std::string{}) == "idp") {
      return fail("no prefix, no crossing");
    }
  }

  // path: web hook -> proxied placeholder, then idp from the provided endpoint.
  repos.answers["web"]["path"] = json{{"ok", true}, {"result", {{"path", json::array()}, {"path_nodes", json::array()}}}};
  repos.answers["idp"]["path"] = json{{"ok", true}, {"result", {{"path", json::array()}, {"path_nodes", json::array()}}}};
  repos.answers["web"]["path:web::useExtendSession->" + proxied] =
      json{{"ok", true}, {"result", {{"path", {"web::useExtendSession", proxied}},
                                     {"path_nodes", {{{"id", "web::useExtendSession"}}, {{"id", proxied}}}}}}};
  repos.answers["idp"]["path:" + provided + "->idp::extendSession"] =
      json{{"ok", true}, {"result", {{"path", {provided, "idp::extendSession"}},
                                     {"path_nodes", {{{"id", provided}}, {{"id", "idp::extendSession"}}}}}}};
  const auto path = cgraph::federate_workspace_request(
      workspace, "path", json{{"source", "web::useExtendSession"}, {"target", "idp::extendSession"}}, repos.ask());
  if (path["result"]["path"] != json::array({"web::useExtendSession", proxied, provided, "idp::extendSession"}) ||
      path["result"]["bridged_through"] != provided || path["result"]["path_nodes"][2]["repo"] != "idp") {
    std::cerr << path.dump(2) << '\n';
    return fail("path joins across the proxy, keeping both spellings of the contract");
  }
  return 0;
}

// The front end serves `GET /api/saml/metadata` itself and also calls
// `/api/backend/saml/metadata` through its proxy. The proxy forwards to the
// backend, never to the front end's own route, so impact from that route does
// not cross to the proxied caller -- unless another member serves the path too.
int test_impact_does_not_proxy_onto_a_members_own_route(const fs::path& root) {
  auto workspace = workspace_of(root, {{"idp", root / "api"}, {"web", root / "web"}});
  workspace.prefixes.push_back(cgraph::EndpointPrefix{.repo = "web", .from = "/api/backend", .to = "/api"});
  const std::string own = "endpoint:GET /api/saml/metadata";
  const std::string proxied = "endpoint:GET /api/backend/saml/metadata";
  FakeRepos repos;
  repos.answers["web"]["impact:web::samlMetadataRoute"] = impact_ok({endpoint_brief(own, 1, true)});
  repos.answers["web"]["explain:" + own] = explain_of(own, true);
  repos.answers["web"]["explain:" + proxied] = explain_of(proxied, false);
  repos.answers["web"]["impact:" + own] = impact_ok({node("web::samlMetadataRoute", 1)});
  repos.answers["web"]["impact:" + proxied] = impact_ok({node("web::getIdpMetadata", 1)});
  const json params{{"id", "web::samlMetadataRoute"}, {"direction", "dependents"}, {"max_depth", 3}};
  const auto response = cgraph::federate_workspace_request(workspace, "impact", params, repos.ask());
  for (const auto& hit : response["result"]["nodes"]) {
    if (hit["id"] == "web::getIdpMetadata") {
      std::cerr << response.dump(2) << '\n';
      return fail("a proxied call crossed at a route only its own repo serves");
    }
  }
  // idp serves the same path too (`/api/v1/users/profile` shape). A change to
  // web's own route still does not reach the proxied caller, which hits idp's
  // copy; a change to idp's handler does.
  repos.answers["idp"]["explain:" + own] = explain_of(own, true);
  repos.answers["idp"]["impact:idp::samlMetadata"] = impact_ok({endpoint_brief(own, 1, true)});
  const auto from_web = cgraph::federate_workspace_request(workspace, "impact", params, repos.ask());
  for (const auto& hit : from_web["result"]["nodes"]) {
    if (hit["id"] == "web::getIdpMetadata") {
      std::cerr << from_web.dump(2) << '\n';
      return fail("a change to the front end's own route reached a caller of the backend's copy");
    }
  }
  const auto from_idp = cgraph::federate_workspace_request(
      workspace, "impact", json{{"id", "idp::samlMetadata"}, {"direction", "dependents"}, {"max_depth", 3}}, repos.ask());
  bool reached = false;
  for (const auto& hit : from_idp["result"]["nodes"]) {
    reached = reached || (hit["id"] == "web::getIdpMetadata" && hit.value("repo", std::string{}) == "web");
  }
  if (!reached) {
    std::cerr << from_idp.dump(2) << '\n';
    return fail("a change to the backend's handler reaches the proxied caller when both repos serve the path");
  }
  return 0;
}

// Three members: web alone serves `GET /api/saml/metadata`, and also calls
// `/api/backend/saml/metadata` through its proxy (web:/api/backend=/api). mobile
// calls `GET /api/saml/metadata` directly. web's proxy forwards to the backend,
// never to web's own route, so the proxied call never meets mobile's call there:
// not from impact in either direction, not from a seed spelled the proxied way,
// and not on a path. Once idp serves the path too, the proxied call does cross.
int test_proxied_call_does_not_join_a_third_member_at_a_members_own_route(const fs::path& root) {
  auto workspace = workspace_of(root, {{"idp", root / "api"}, {"web", root / "web"}, {"mobile", root / "mobile"}});
  workspace.prefixes.push_back(cgraph::EndpointPrefix{.repo = "web", .from = "/api/backend", .to = "/api"});
  const std::string own = "endpoint:GET /api/saml/metadata";
  const std::string proxied = "endpoint:GET /api/backend/saml/metadata";
  FakeRepos repos;
  repos.answers["web"]["explain:" + own] = explain_of(own, true);
  repos.answers["web"]["explain:" + proxied] = explain_of(proxied, false);
  repos.answers["mobile"]["explain:" + own] = explain_of(own, false);
  repos.answers["web"]["impact:web::getIdpMetadata"] = impact_ok({endpoint_brief(proxied, 1, false)});
  repos.answers["web"]["impact:" + proxied] = impact_ok({node("web::getIdpMetadata", 1)});
  repos.answers["web"]["impact:" + own] = impact_ok({node("web::samlRoute", 1)});
  repos.answers["mobile"]["impact:" + own] = impact_ok({node("mobile::fetchMetadata", 1)});
  const auto empty_path = json{{"ok", true}, {"result", {{"path", json::array()}, {"path_nodes", json::array()}}}};
  repos.answers["web"]["path"] = empty_path;
  repos.answers["mobile"]["path"] = empty_path;
  repos.answers["web"]["path:web::getIdpMetadata->" + proxied] =
      json{{"ok", true},
           {"result", {{"path", {"web::getIdpMetadata", proxied}},
                       {"path_nodes", {{{"id", "web::getIdpMetadata"}}, {{"id", proxied}}}}}}};
  repos.answers["mobile"]["path:" + own + "->mobile::fetchMetadata"] =
      json{{"ok", true},
           {"result", {{"path", {own, "mobile::fetchMetadata"}},
                       {"path_nodes", {{{"id", own}}, {{"id", "mobile::fetchMetadata"}}}}}}};
  const json from_caller{{"id", "web::getIdpMetadata"}, {"direction", "both"}, {"max_depth", 3}};
  const json from_seed{{"id", proxied}, {"direction", "dependents"}, {"max_depth", 3}};
  const json path_params{{"source", "web::getIdpMetadata"}, {"target", "mobile::fetchMetadata"}};
  auto reaches_mobile = [](const json& response) {
    return std::ranges::any_of(response["result"]["nodes"],
                               [](const json& hit) { return hit["id"] == "mobile::fetchMetadata"; });
  };

  const auto both = cgraph::federate_workspace_request(workspace, "impact", from_caller, repos.ask());
  if (reaches_mobile(both)) {
    std::cerr << both.dump(2) << '\n';
    return fail("impact (both) bridged a proxied call to a third member at a route only the caller serves");
  }
  const auto seeded = cgraph::federate_workspace_request(workspace, "impact", from_seed, repos.ask());
  if (reaches_mobile(seeded)) {
    std::cerr << seeded.dump(2) << '\n';
    return fail("impact seeded with the proxied spelling crossed at a route only the caller serves");
  }
  const auto path = cgraph::federate_workspace_request(workspace, "path", path_params, repos.ask());
  if (!path["result"]["path"].empty()) {
    std::cerr << path.dump(2) << '\n';
    return fail("path bridged a proxied call to a third member at a route only the caller serves");
  }

  // idp serves the path too: now the proxy does forward there, and all three cross.
  repos.answers["idp"]["explain:" + own] = explain_of(own, true);
  const auto both_served = cgraph::federate_workspace_request(workspace, "impact", from_caller, repos.ask());
  const auto seeded_served = cgraph::federate_workspace_request(workspace, "impact", from_seed, repos.ask());
  const auto path_served = cgraph::federate_workspace_request(workspace, "path", path_params, repos.ask());
  if (!reaches_mobile(both_served) || !reaches_mobile(seeded_served) ||
      path_served["result"]["path"] !=
          json::array({"web::getIdpMetadata", proxied, own, "mobile::fetchMetadata"}) ||
      path_served["result"]["bridged_through"] != own) {
    std::cerr << both_served.dump(2) << '\n' << seeded_served.dump(2) << '\n' << path_served.dump(2) << '\n';
    return fail("a proxied call crosses at an endpoint another member serves too");
  }
  return 0;
}

// A manifest's `databases` and `env` load, round-trip, and every malformed or
// conflicting declaration is a manifest error, never an exception.
int test_manifest_databases_and_env(const fs::path& root) {
  const auto ws = root / "ws-databases";
  for (const auto* repo : {"api", "ml", "web"}) {
    fs::create_directories(ws / repo);
  }
  const std::string repos = R"("repos": [{"name": "api", "root": "../api"}, {"name": "ml", "root": "../ml"}, {"name": "web", "root": "../web"}])";
  write_file(ws / "ok" / std::string(cgraph::kWorkspaceFile),
             "{" + repos + R"(, "databases": [{"name": "turing", "repos": ["api", "ml"]}],
                 "env": [{"name": "ML_BACKEND_URL", "service": "ml"}]})");
  const auto loaded = cgraph::load_workspace(ws / "ok");
  const auto manifest = cgraph::workspace_manifest_json(loaded);
  if (!loaded.ok() || manifest.value("databases", json{}) != json::parse(R"([{"name": "turing", "repos": ["api", "ml"]}])") ||
      manifest.value("env", json{}) != json::parse(R"([{"name": "ML_BACKEND_URL", "service": "ml"}])")) {
    std::cerr << manifest.dump() << '\n';
    return fail("a manifest's databases and env load and round-trip");
  }
  const std::map<std::string, std::string> invalid{
      {"stranger", R"(, "databases": [{"name": "turing", "repos": ["api", "billing"]}])"},
      {"two-databases", R"(, "databases": [{"name": "a", "repos": ["api"]}, {"name": "b", "repos": ["api", "ml"]}])"},
      {"twice", R"(, "databases": [{"name": "a", "repos": ["api"]}, {"name": "a", "repos": ["ml"]}])"},
      {"local", R"(, "databases": [{"name": "local", "repos": ["api"]}])"},
      {"colon", R"(, "databases": [{"name": "a:b", "repos": ["api"]}])"},
      {"typed-db", R"(, "databases": [{"name": 3, "repos": "api"}])"},
      {"not-array", R"(, "databases": {"name": "turing"})"},
      {"env-stranger", R"(, "env": [{"name": "X", "service": "billing"}])"},
      {"env-typed", R"(, "env": [{"name": "X", "service": ["ml"]}])"},
      {"env-twice", R"(, "env": [{"name": "X", "service": "ml"}, {"name": "X", "service": "api"}])"},
  };
  for (const auto& [name, extra] : invalid) {
    write_file(ws / name / std::string(cgraph::kWorkspaceFile), "{" + repos + extra + "}");
    try {
      const auto bad = cgraph::load_workspace(ws / name);
      if (bad.ok() || !bad.repos.empty() || bad.errors.empty()) {
        return fail("a malformed or conflicting declaration is a manifest error: " + name);
      }
    } catch (const std::exception& error) {
      return fail("a malformed declaration threw instead of reporting an error: " + name + ": " + error.what());
    }
  }
  return 0;
}

json contract_brief(const std::string& id, int depth, bool served) {
  json brief{{"id", id}, {"label", id}, {"kind", id.substr(0, id.find(':'))}, {"depth", depth}};
  if (served) {
    brief["source_file"] = "/src/provider.ts";
  }
  return brief;
}

// A header is the same id in every repo: impact from the server code that
// reads it reaches the client that sends it, and back, and path joins there.
// No declaration is needed. A reached env variable names its declared
// provider in `bridged`.
int test_impact_and_path_cross_a_header(const fs::path& root) {
  const auto workspace = workspace_of(root, {{"api", root / "api"}, {"web", root / "web"}});
  const std::string header = "header:x-tenant-id";
  FakeRepos repos;
  repos.answers["api"]["impact:api::readTenant"] = impact_ok(
      {contract_brief(header, 1, true), contract_brief("header:authorization", 1, true), contract_brief("env:NODE_ENV", 1, false)});
  repos.answers["web"]["impact:" + header] = impact_ok({node("web::sendTenant", 1)});
  // web also sends Authorization and reads NODE_ENV, as every service does.
  repos.answers["web"]["impact:header:authorization"] = impact_ok({node("web::fetchWithToken", 1)});
  repos.answers["web"]["impact:env:NODE_ENV"] = impact_ok({node("web::isProduction", 1)});
  const auto response = cgraph::federate_workspace_request(
      workspace, "impact", json{{"id", "api::readTenant"}, {"direction", "dependents"}, {"max_depth", 3}}, repos.ask());
  bool crossed = false;
  for (const auto& hit : response["result"]["nodes"]) {
    crossed = crossed || (hit["id"] == "web::sendTenant" && hit.value("repo", std::string{}) == "web" &&
                          hit.value("bridged_through", std::string{}) == header && hit["depth"] == 2);
  }
  const auto bridged = response["result"].value("bridged", json::array());
  if (!crossed || bridged.size() != 1 || bridged[0].value("contract", std::string{}) != header || bridged[0].contains("endpoint")) {
    std::cerr << response.dump(2) << '\n';
    return fail("impact crosses from the server reading a header to the client sending it");
  }
  for (const auto& hit : response["result"]["nodes"]) {
    if (hit["id"] == "web::fetchWithToken" || hit["id"] == "web::isProduction") {
      std::cerr << response.dump(2) << '\n';
      return fail("impact crossed at a standard header or an undeclared env variable");
    }
  }
  repos.answers["web"]["impact:web::sendTenant"] = impact_ok({contract_brief(header, 1, false)});
  repos.answers["api"]["impact:" + header] = impact_ok({node("api::readTenant", 1)});
  const auto reverse = cgraph::federate_workspace_request(
      workspace, "impact", json{{"id", "web::sendTenant"}, {"direction", "dependencies"}, {"max_depth", 3}}, repos.ask());
  bool back = false;
  for (const auto& hit : reverse["result"]["nodes"]) {
    back = back || (hit["id"] == "api::readTenant" && hit.value("repo", std::string{}) == "api");
  }
  if (!back) {
    std::cerr << reverse.dump(2) << '\n';
    return fail("impact crosses from the client sending a header to the server reading it");
  }
  const auto empty_path = json{{"ok", true}, {"result", {{"path", json::array()}, {"path_nodes", json::array()}}}};
  repos.answers["web"]["path"] = empty_path;
  repos.answers["api"]["path"] = empty_path;
  repos.answers["web"]["path:web::sendTenant->" + header] =
      json{{"ok", true}, {"result", {{"path", {"web::sendTenant", header}},
                                     {"path_nodes", {{{"id", "web::sendTenant"}}, {{"id", header}}}}}}};
  repos.answers["api"]["path:" + header + "->api::readTenant"] =
      json{{"ok", true}, {"result", {{"path", {header, "api::readTenant"}},
                                     {"path_nodes", {{{"id", header}}, {{"id", "api::readTenant"}}}}}}};
  const auto path = cgraph::federate_workspace_request(
      workspace, "path", json{{"source", "web::sendTenant"}, {"target", "api::readTenant"}}, repos.ask());
  if (path["result"]["path"] != json::array({"web::sendTenant", header, "api::readTenant"}) ||
      path["result"]["bridged_through"] != header) {
    std::cerr << path.dump(2) << '\n';
    return fail("path joins two repos at a header");
  }
  // Two unrelated repos that both read NODE_ENV or send Authorization share no path.
  repos.answers["web"]["impact:web::isProduction"] =
      impact_ok({contract_brief("env:NODE_ENV", 1, false), contract_brief("header:authorization", 1, false)});
  repos.answers["api"]["impact:env:NODE_ENV"] = impact_ok({node("api::readTenant", 1)});
  repos.answers["web"]["path:web::isProduction->env:NODE_ENV"] =
      json{{"ok", true}, {"result", {{"path", {"web::isProduction", "env:NODE_ENV"}},
                                     {"path_nodes", {{{"id", "web::isProduction"}}, {{"id", "env:NODE_ENV"}}}}}}};
  repos.answers["web"]["path:web::isProduction->header:authorization"] =
      json{{"ok", true}, {"result", {{"path", {"web::isProduction", "header:authorization"}},
                                     {"path_nodes", {{{"id", "web::isProduction"}}, {{"id", "header:authorization"}}}}}}};
  repos.answers["api"]["path:env:NODE_ENV->api::readTenant"] =
      json{{"ok", true}, {"result", {{"path", {"env:NODE_ENV", "api::readTenant"}},
                                     {"path_nodes", {{{"id", "env:NODE_ENV"}}, {{"id", "api::readTenant"}}}}}}};
  repos.answers["api"]["path:header:authorization->api::readTenant"] =
      json{{"ok", true}, {"result", {{"path", {"header:authorization", "api::readTenant"}},
                                     {"path_nodes", {{{"id", "header:authorization"}}, {{"id", "api::readTenant"}}}}}}};
  const auto unrelated = cgraph::federate_workspace_request(
      workspace, "path", json{{"source", "web::isProduction"}, {"target", "api::readTenant"}}, repos.ask());
  if (!unrelated["result"]["path"].empty()) {
    std::cerr << unrelated.dump(2) << '\n';
    return fail("path bridged two repos through NODE_ENV or Authorization");
  }
  return 0;
}

// Tables cross only between members declaring one database: api's and ml's
// `table:local:users` meet at `table:turing:users`; billing's own `users`
// table is never reached; with no declaration nothing crosses. A reached env
// variable names the service the manifest says provides it.
int test_impact_crosses_a_declared_database(const fs::path& root) {
  const auto ws = root / "ws-db-impact";
  for (const auto* repo : {"api", "ml", "billing"}) {
    fs::create_directories(ws / repo);
  }
  write_file(ws / std::string(cgraph::kWorkspaceFile),
             R"({"repos": [{"name": "api", "root": "./api"}, {"name": "ml", "root": "./ml"}, {"name": "billing", "root": "./billing"}],
                 "databases": [{"name": "turing", "repos": ["api", "ml"]}],
                 "env": [{"name": "API_URL", "service": "api"}]})");
  const auto workspace = cgraph::load_workspace(ws);
  if (!workspace.ok()) {
    return fail("the database manifest loads: " + workspace.errors.front());
  }
  const std::string local = "table:local:users";
  FakeRepos repos;
  repos.answers["api"]["impact:api::createUsers"] = impact_ok({contract_brief(local, 1, true)});
  repos.answers["ml"]["impact:" + local] = impact_ok({node("ml::listUsers", 1)});
  repos.answers["billing"]["impact:" + local] = impact_ok({node("billing::chargeUsers", 1)});
  const json params{{"id", "api::createUsers"}, {"direction", "dependents"}, {"max_depth", 3}};
  const auto response = cgraph::federate_workspace_request(workspace, "impact", params, repos.ask());
  std::map<std::string, json> reached;
  for (const auto& hit : response["result"]["nodes"]) {
    reached[hit["id"]] = hit;
  }
  if (reached.count("ml::listUsers") == 0 || reached["ml::listUsers"].value("bridged_through", std::string{}) != "table:turing:users" ||
      reached.count("billing::chargeUsers") != 0) {
    std::cerr << response.dump(2) << '\n';
    return fail("a table crosses to a member of its declared database, and only to one");
  }
  // No declaration: the same answers never cross.
  const auto undeclared = cgraph::federate_workspace_request(
      workspace_of(ws, {{"api", ws / "api"}, {"ml", ws / "ml"}, {"billing", ws / "billing"}}), "impact", params, repos.ask());
  for (const auto& hit : undeclared["result"]["nodes"]) {
    if (hit.value("repo", std::string{}) != "api") {
      std::cerr << undeclared.dump(2) << '\n';
      return fail("an undeclared repo-local table crossed repositories");
    }
  }
  // An env variable ml reads names api, the service the manifest declares.
  repos.answers["ml"]["impact:ml::callApi"] = impact_ok({contract_brief("env:API_URL", 1, false)});
  const auto env = cgraph::federate_workspace_request(
      workspace, "impact", json{{"id", "ml::callApi"}, {"direction", "dependencies"}, {"max_depth", 3}}, repos.ask());
  const auto bridged = env["result"].value("bridged", json::array());
  if (bridged.size() != 1 || bridged[0].value("contract", std::string{}) != "env:API_URL" ||
      bridged[0].value("provided_by", std::string{}) != "api") {
    std::cerr << env.dump(2) << '\n';
    return fail("a reached env variable names its declared provider");
  }
  return 0;
}

// Claims of the repos one declared issuer mints tokens for cross between those
// repos only, at `claim:<issuer>:<name>`: web is reached through idp's
// `session_id` and OIDC `email`, never through the RFC 7519 `sub`, and api,
// outside the issuer, is reached through none of them. Without the
// declaration `session_id` crosses to every repo and `email` to none, as
// before. Malformed or conflicting issuers are manifest errors.
int test_impact_crosses_within_a_declared_issuer(const fs::path& root) {
  const auto ws = root / "ws-issuer";
  for (const auto* repo : {"idp", "web", "api"}) {
    fs::create_directories(ws / repo);
  }
  const std::string members =
      R"("repos": [{"name": "idp", "root": "./idp"}, {"name": "web", "root": "./web"}, {"name": "api", "root": "./api"}])";
  write_file(ws / std::string(cgraph::kWorkspaceFile),
             "{" + members + R"(, "issuers": [{"name": "idp", "repos": ["idp", "web"]}]})");
  const auto workspace = cgraph::load_workspace(ws);
  if (!workspace.ok() ||
      cgraph::workspace_manifest_json(workspace).value("issuers", json{}) !=
          json::parse(R"([{"name": "idp", "repos": ["idp", "web"]}])")) {
    return fail("an issuer manifest loads and round-trips");
  }
  FakeRepos repos;
  repos.answers["idp"]["impact:idp::mintToken"] = impact_ok({contract_brief("claim:session_id", 1, true),
                                                              contract_brief("claim:email", 1, true),
                                                              contract_brief("claim:sub", 1, true)});
  for (const auto* repo : {"web", "api"}) {
    for (const auto* claim : {"session_id", "email", "sub"}) {
      repos.answers[repo]["impact:claim:" + std::string(claim)] =
          impact_ok({node(std::string(repo) + "::read_" + claim, 1)});
    }
  }
  const json params{{"id", "idp::mintToken"}, {"direction", "dependents"}, {"max_depth", 3}};
  json last;
  auto reached_by = [&](const cgraph::Workspace& scope) {
    std::map<std::string, std::string> reached;  // node -> the contract it came through
    last = cgraph::federate_workspace_request(scope, "impact", params, repos.ask());
    for (const auto& hit : last["result"]["nodes"]) {
      if (hit.value("repo", std::string{}) != "idp") {
        reached[hit["id"]] = hit.value("bridged_through", std::string{});
      }
    }
    return reached;
  };
  const auto declared = reached_by(workspace);
  const std::map<std::string, std::string> within{{"web::read_session_id", "claim:idp:session_id"},
                                                  {"web::read_email", "claim:idp:email"}};
  if (declared != within) {
    std::cerr << last.dump(2) << '\n';
    return fail("an issuer member's claims cross to its other members only, every name but the RFC 7519 ones");
  }
  const auto undeclared = reached_by(workspace_of(ws, {{"idp", ws / "idp"}, {"web", ws / "web"}, {"api", ws / "api"}}));
  const std::map<std::string, std::string> everywhere{{"web::read_session_id", "claim:session_id"},
                                                      {"api::read_session_id", "claim:session_id"}};
  if (undeclared != everywhere) {
    std::cerr << last.dump(2) << '\n';
    return fail("with no issuer declared an application claim crosses to every repo and a standard one to none");
  }
  const std::map<std::string, std::string> invalid{
      {"issuer-stranger", R"(, "issuers": [{"name": "idp", "repos": ["idp", "billing"]}])"},
      {"two-issuers", R"(, "issuers": [{"name": "a", "repos": ["web"]}, {"name": "b", "repos": ["idp", "web"]}])"},
      {"issuer-twice", R"(, "issuers": [{"name": "a", "repos": ["idp"]}, {"name": "a", "repos": ["web"]}])"},
      {"issuer-local", R"(, "issuers": [{"name": "local", "repos": ["idp"]}])"},
      {"issuer-colon", R"(, "issuers": [{"name": "a:b", "repos": ["idp"]}])"},
      {"issuer-typed", R"(, "issuers": [{"name": "idp", "repos": [3]}])"},
      {"issuer-empty", R"(, "issuers": [{"name": "idp", "repos": []}])"},
      {"issuer-not-array", R"(, "issuers": {"name": "idp"})"},
  };
  for (const auto& [name, extra] : invalid) {
    write_file(ws / name / std::string(cgraph::kWorkspaceFile),
               "{" + std::string(R"("repos": [{"name": "idp", "root": "../idp"}, {"name": "web", "root": "../web"}])") +
                   extra + "}");
    try {
      const auto bad = cgraph::load_workspace(ws / name);
      if (bad.ok() || !bad.repos.empty() || bad.errors.empty()) {
        return fail("a malformed or conflicting issuer is a manifest error: " + name);
      }
    } catch (const std::exception& error) {
      return fail("a malformed issuer threw instead of reporting an error: " + name + ": " + error.what());
    }
  }
  return 0;
}

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
  failures += test_enclosing_workspace(root);
  failures += test_manifest_prefixes(root);
  failures += test_impact_and_path_cross_a_proxy_prefix(root);
  failures += test_impact_does_not_proxy_onto_a_members_own_route(root);
  failures += test_proxied_call_does_not_join_a_third_member_at_a_members_own_route(root);
  failures += test_manifest_databases_and_env(root);
  failures += test_impact_and_path_cross_a_header(root);
  failures += test_impact_crosses_a_declared_database(root);
  failures += test_impact_crosses_within_a_declared_issuer(root);

  fs::remove_all(root);
  return failures == 0 ? 0 : 1;
}
