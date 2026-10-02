#pragma once

#include "cgraph/daemon_ops.hpp"
#include "cgraph/workspace.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <unordered_map>

namespace cgraph {

// The other services of the workspace the target root belongs to, and a way to
// ask their daemons. With it, change_context adds a `cross_service` section:
// the consumers (in other repos) of every endpoint the change serves, and the
// providers of every endpoint it consumes.
struct CrossServiceAsk {
  const EnclosingWorkspace* enclosing = nullptr;
  RepoAsk ask;
};

// An endpoint the home repo's code touches, with the roles it plays for it
// ("serves", "removed", "added": ask for consumers; "consumes": ask for the
// provider) and a rank (0 first) that orders asking and trimming.
struct CrossServiceContract {
  std::set<std::string> roles;
  int rank = 4;
  bool outside_diff = false;  // only from root differences a diff does not supply
};
using CrossServiceContracts = std::map<std::string, CrossServiceContract>;

// The contracts (contracts.hpp: endpoints, tables, graph labels, headers,
// claims, env names) one snapshot's changed code touches, added to `touched`.
// `reached` is trace_impact from the changed symbols with `dependents`. A
// contract the change serves: one it changed (rank 0), one whose provider it
// reached (the last step is handled_by) or one a changed file contains (rank
// 1). A contract it uses: CONSUMES from changed code (rank 1), or from a
// function calling a changed helper directly (rank 2).
void touch_contracts(const GraphSnapshot& graph, const std::unordered_map<std::string, ImpactReach>& reached,
                     CrossServiceContracts& touched);

// Asks every other repo of the workspace who consumes what the home repo serves
// and who provides what it calls: the `cross_service` section change context
// returns, also used for a single file before it is edited. A repo-local table
// or label is asked only of the members declared in the home repo's database,
// under their own `table:local:` spelling; one with no declared database is
// listed with `local: true` and asked of nobody. An env variable the home repo
// uses is answered by the member declared to provide it, as one row of kind
// `service`.
[[nodiscard]] nlohmann::json cross_service_section(const CrossServiceAsk& scope, const CrossServiceContracts& contracts,
                                                   std::size_t max_contracts = 24);

// One `cross_service` row as a sentence about `file`: an endpoint is served and
// called (`src/routes.ts serves GET /api/v1/stats, called from web
// src/stats.ts:3 (loadStats)`), any other contract provided and used
// (`src/db.ts provides table:turing:users, used by ml ...`); a declared env
// provider row names the service.
[[nodiscard]] std::string cross_service_summary(const std::string& file, const nlohmann::json& row);

// Builds two isolated in-memory source snapshots and validates a supplied unified
// diff. Never edits either root or publishes into a resident daemon. Exceptions
// reject the whole operation; callers must not emit a partial success response.
[[nodiscard]] nlohmann::json change_context(const nlohmann::json& parameters,
                                            const CrossServiceAsk* cross_service = nullptr);

}  // namespace cgraph
