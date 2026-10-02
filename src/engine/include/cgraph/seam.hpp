#pragma once

#include "cgraph/contract_declarations.hpp"
#include "cgraph/endpoint_prefixes.hpp"
#include "cgraph/types.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cgraph {

// Marker file `seam fuse` writes into its output dir; its presence tells graphd to
// serve that dir as a static read-only seam graph rather than build-and-watch it.
inline constexpr std::string_view kSeamMarkerFile = ".cgraph-seam";

// True iff `root` is a fused-seam output directory (carries the seam marker).
[[nodiscard]] bool is_seam_directory(const std::filesystem::path& root);

// Result of generating a cross-service seam fragment. On success `ok` is true and
// `fragment` holds the contract graph; on any hard error (malformed spec, unknown
// endpoint/schema reference, missing consumer graph, or an anchor that resolves to
// no node) `ok` is false, `fragment` is empty, and `errors` explains why -- a
// partial or dangling seam is never produced. `resolution_log` records every
// resolved anchor (for stderr auditability) on success.
struct SeamResult {
  bool ok = false;
  Fragment fragment;
  std::vector<std::string> errors;
  std::vector<std::string> resolution_log;
};

// Generate a cross-service contract fragment from a host-authored seam spec and the
// named consumer code graphs (name -> path to that service's graph.json). Anchors
// in the spec are resolved against the real graphs; this is deterministic and
// fail-loud. See docs/host-skill-contract.md and the cross-service-seam capability.
[[nodiscard]] SeamResult generate_seam(
    const nlohmann::json& spec,
    const std::unordered_map<std::string, std::filesystem::path>& graph_paths);

// Generate a cross-service contract fragment from the contracts each graph already
// carries (contracts.hpp): `endpoint` nodes with `handled_by` edges are what a
// service serves, `CONSUMES` edges are what it calls. No spec: endpoints join by
// their repo-free canonical id. The fragment has a `service` node per graph,
// every served or consumed endpoint, `SERVED_BY` (endpoint -> service),
// `HANDLED_BY` (endpoint -> handler code-ref), `CONSUMES` (service -> endpoint)
// and `CONSUMED_AT` (endpoint -> caller code-ref). `resolution_log` reports per
// service what it serves and consumes, how many endpoints matched across
// services, and how many are consumed with no provider among the given graphs.
// Fails loud only when a graph cannot be read.
//
// `prefixes` (endpoint_prefixes.hpp) join a consumer that reaches its provider
// through its own proxy route: an endpoint graph `repo` consumes but neither
// serves nor documents, under `from`, joins as `to` + the rest. Its CONSUMED_AT
// edge carries `via` = the consumer's own path, and the log counts the mapped
// endpoints per prefix.
//
// Every other contract that crosses repositories (crossing_id,
// contract_declarations.hpp: tables and graph labels in a named database,
// non-standard headers, claims, declared env names) joins the same way, with
// the same four edges and its own kind. `databases` spell a declared member's
// `table:local:<name>` / `label:local:<name>` as `table:<database>:<name>` so
// members of one database meet; an undeclared repo-local table or label, an
// undeclared env name and a standard header are left out of the seam. `env`
// makes the declared service the provider of `env:<NAME>` (`SERVED_BY`). The
// endpoint log lines are unchanged; other contracts get their own lines, only
// when there are any.
[[nodiscard]] SeamResult discover_seam(
    const std::vector<std::pair<std::string, std::filesystem::path>>& graphs,
    std::span<const EndpointPrefix> prefixes = {}, std::span<const ContractDatabase> databases = {},
    std::span<const EnvProvider> env = {});

// Result of fusing a seam fragment with its service graphs into one view graph.
struct SeamFuseResult {
  bool ok = false;
  GraphSnapshot graph;
  std::vector<std::string> errors;
};

// Merge a seam fragment with the named consumer code graphs into a single
// community-clustered view graph: every service node is tagged with its service
// name, seam contract nodes with their community, shadow code-refs are dropped
// (the real service node already carries that id), and edges are deduplicated.
// View-only -- the result is a static render artifact, not a daemon. Fails loud
// (ok=false) if any edge endpoint is missing from the fused node set.
//
// With `prefixes`, a service's CONSUMES edge into an endpoint it does not serve
// (`served: false`) under a `from` of its own is redirected to the `to` spelling,
// the one the seam discovered with the same prefixes, and the placeholder node it
// leaves unused is not rendered.
//
// Contract ids crossing_id gives (contract_declarations.hpp) are shared across
// services; every other id, a `table:local:` or undeclared `env:` one
// included, is scoped to its service. With `databases`, a declared member's
// `table:local:<name>` becomes the seam's `table:<database>:<name>` (its
// `database` property too); with `env`, a declared `env:<NAME>` is shared.
[[nodiscard]] SeamFuseResult fuse_seam(
    const Fragment& seam,
    const std::vector<std::pair<std::string, GraphSnapshot>>& services,
    std::span<const EndpointPrefix> prefixes = {}, std::span<const ContractDatabase> databases = {},
    std::span<const EnvProvider> env = {});

}  // namespace cgraph
