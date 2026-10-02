#pragma once

#include "cgraph/contract_declarations.hpp"
#include "cgraph/endpoint_prefixes.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Workspace federation (CGR-14): one read surface over several repositories,
// each still served by its own resident daemon.
//
// A workspace is a directory holding `cgraph.workspace.json`:
//
//   { "name": "turing", "repos": [ { "name": "api",  "root": "./turing-api" },
//                                  { "name": "web",  "root": "./frontend"  } ] }
//
// Pointing the client or the MCP server at that directory federates every read:
// each member repo answers from its own graph, the answers are merged, and every
// node carries the `repo` it came from. No graph is ever copied into a workspace
// process -- each repo keeps its own daemon, its own watcher and its own
// incremental updates, so an edit on either side is live on the next query.
//
// Cross-repo reach comes from the contract nodes of CGR-13: an `endpoint:` id is
// canonical and repo-free, so the same node exists in the graph of the service
// that serves it and of every service that calls it. `impact` therefore runs in
// each repo, and where a traversal reaches a contract it is forwarded once, with
// the remaining depth, to the other repos -- one hop per contract, never a join
// over copied graphs. Every id is_bridged_contract accepts (contracts.hpp:
// endpoints, headers, claims, env names, tables and graph labels in a named
// database) crosses this way.
//
// A repo whose daemon cannot be reached is reported in `unreachable`, never
// silently dropped: a partial answer that looks total is worse than a loud gap.
//
// A repo that reaches another through its own proxy route (a Next.js
// `/api/backend/[...path]` forwarding to the backend's `/api/...`) names that in
// the manifest's optional `prefixes`:
//
//   "prefixes": [ { "repo": "web", "from": "/api/backend", "to": "/api" } ]
//
// `impact` and `path` then cross at `endpoint:GET /api/v1/users/{}` from web's
// `endpoint:GET /api/backend/v1/users/{}` (and back), but only through an endpoint
// web consumes and does not serve itself (endpoint_prefixes.hpp). Each repo's own
// graph keeps its own spelling.
//
// Repos that share a database, and the service an env variable addresses, are
// declared too (contract_declarations.hpp):
//
//   "databases": [ { "name": "turing", "repos": ["api", "ml"] } ],
//   "env": [ { "name": "ML_BACKEND_URL", "service": "ml" } ]
//
// `impact` and `path` then cross from api's `table:local:users` to ml's
// `table:local:users` at `table:turing:users`, and never to a repo outside the
// database. A reached `env:ML_BACKEND_URL` is reported in `bridged` with
// `provided_by: "ml"`.
namespace cgraph {

inline constexpr std::string_view kWorkspaceFile = "cgraph.workspace.json";

struct WorkspaceRepo {
  std::string name;
  std::filesystem::path root;  // canonical absolute path
};

struct Workspace {
  std::filesystem::path root;
  std::string name;
  std::vector<WorkspaceRepo> repos;
  std::vector<EndpointPrefix> prefixes;  // proxy prefixes between members, manifest order
  std::vector<ContractDatabase> databases;  // members sharing a database, manifest order
  std::vector<EnvProvider> env;             // env variables and the member each addresses
  std::vector<std::string> errors;  // non-empty when the manifest is unusable

  [[nodiscard]] bool ok() const { return errors.empty(); }
};

// True when `root` holds a workspace manifest.
[[nodiscard]] bool is_workspace_root(const std::filesystem::path& root);

// A project root's enclosing workspace: the nearest ancestor directory (up to
// and including $HOME) holding a manifest that lists a repo whose root is the
// project root or contains it (a worktree nested in a member checkout counts).
// `home` names that repo, and its `root` is replaced by the project root, so
// the home repo is answered from the tree the caller is actually in. A manifest
// that lists the root but cannot be loaded is returned with `workspace.errors`
// set and no repos, so the caller can say so.
struct EnclosingWorkspace {
  Workspace workspace;
  std::string home;
  std::filesystem::path home_root;  // the home member's root as the manifest names it
};
[[nodiscard]] std::optional<EnclosingWorkspace> find_enclosing_workspace(const std::filesystem::path& project_root);

// Reads and validates `root/cgraph.workspace.json`. A missing file, malformed
// JSON, an empty repo list, a duplicate name, a repo root that does not exist,
// or a `prefixes`, `databases` or `env` entry that is malformed or names no member
// repo (or a repo declared in two databases) is an error; the returned Workspace
// then carries `errors` and no repos.
[[nodiscard]] Workspace load_workspace(const std::filesystem::path& root);

// The manifest text for a workspace (what `workspace init` writes): repo roots
// are stored relative to the workspace root when they sit beneath it, so a
// checkout moves without editing the file.
[[nodiscard]] nlohmann::json workspace_manifest_json(const Workspace& workspace);

// Every immediate subdirectory of `root` that looks like a repository (holds a
// `.git` entry), named after the directory, sorted by name. Used by
// `cgraph workspace init` when no repos are given explicitly.
[[nodiscard]] std::vector<WorkspaceRepo> discover_workspace_repos(const std::filesystem::path& root);

// Asks one repo's daemon for one op. Returns the daemon's envelope
// (`{ok, result}` or `{ok:false, error}`), or nullopt when the daemon could not
// be reached, with `error` describing why.
using RepoAsk = std::function<std::optional<nlohmann::json>(
    const WorkspaceRepo& repo, const std::string& op, const nlohmann::json& params, std::string& error)>;

// Answers `op` across the workspace, returning the same `{ok, result}` envelope
// a single daemon would. `status`, `query`, `explain`, `impact`, `path` and
// `update` federate; every other op is refused with a typed error naming the
// repo roots, because its unit is one project (a `report` or a `context` budget
// spanning repositories would be a different thing, not a merged one).
[[nodiscard]] nlohmann::json federate_workspace_request(
    const Workspace& workspace, const std::string& op, const nlohmann::json& params, const RepoAsk& ask);

}  // namespace cgraph
