#pragma once

#include "cgraph/language_config.hpp"
#include "cgraph/operation_stats.hpp"
#include "cgraph/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

// Contract discovery (CGR-13): the wire contracts a repo provides, found in its
// own source rather than typed into a seam spec. This slice covers HTTP
// endpoints. Extraction records four kinds of raw fact as RawRelation entries:
//
//   "route"       source_id = the inline handler's function node, target_label =
//                 the module-level identifier of the router chain it is
//                 registered on (`notebookRoutes` in `notebookRoutes.get('/x',
//                 handler)`; empty when the extractor could not root the chain),
//                 context = "<verb> <path>", the path beneath any enclosing
//                 `.group('/p')` / inline-constructor prefix.
//   "file_route"  the same for an exported GET/POST/... in a Next.js
//                 `app/**/route.ts`: no chain, the path is the file's.
//   "mounts"      source_id = the parent chain's variable node, target_label =
//                 the child chain's identifier (`apiRoutes.use(notebookRoutes)`,
//                 `app.use('/api', router)`, `app.route('/api', sub)`), context =
//                 the mount path when the framework takes one, else empty.
//   "aliases"     source_id = a variable whose value is another identifier
//                 (`export const deckModule = deckRoutes as unknown as Elysia`):
//                 the same chain under a second name.
//
// A chain's own prefix (`new Elysia({ prefix: '/notebooks' })`,
// `new Hono().basePath('/v1')`) is the `route_prefix` property on its variable
// node. resolve_contracts composes the full path of every route by walking
// mounts and aliases up to each top-level chain, so `/api/v1` + `` +
// `/notebooks` + `/starred-notes` becomes one `endpoint` node
//
//   id `endpoint:GET /api/v1/notebooks/starred-notes`, label `GET /api/v1/...`,
//   kind `endpoint`, properties method/path, source_file and source_location of
//   the handler, `contains` from the handler's file, and `handled_by` to the
//   handler so `graph_impact` on the handler reaches its endpoint.
//
// The id carries no repo, so the same route found in a consumer (a later slice)
// joins by construction. Endpoint nodes are rebuilt from scratch on every
// resolve, which is what keeps the incremental path correct: rebuild_graph
// re-merges every cached fragment and re-runs resolution.
namespace cgraph {

// The HTTP verbs a router DSL exposes as methods (lowercase).
[[nodiscard]] bool is_http_verb(std::string_view verb);

// Joins a mount prefix and a route path the way routers do: one slash between
// segments, duplicate slashes collapsed, no trailing slash except for the root.
// `/notebooks` + `/` is `/notebooks`; `` + `/health` is `/health`.
[[nodiscard]] std::string join_route_path(std::string_view prefix, std::string_view path);

// The URL path a Next.js App Router `route.ts|js` file serves, from its path
// under the last `app` directory: `app/api/admin/connectors/[id]/route.ts` is
// `/api/admin/connectors/:id`. Route groups `(marketing)` and parallel-route
// slots `@modal` are dropped, `[id]` becomes `:id`, `[...slug]` and
// `[[...slug]]` become `*`. Empty when the file is not a route file.
[[nodiscard]] std::optional<std::string> next_route_path(std::string_view source_file);

// Mints endpoint nodes and their edges from the "route" / "mounts" facts in
// raw_relations. Runs after resolve_imports (mount targets and chains resolve
// through the file's imports, then its own declarations). `stats`, when given,
// receives the tally that stats.json reports as route_resolution.
void resolve_contracts(GraphSnapshot& graph, std::span<const RawRelation> raw_relations,
                       ContractResolution* stats = nullptr);

}  // namespace cgraph
