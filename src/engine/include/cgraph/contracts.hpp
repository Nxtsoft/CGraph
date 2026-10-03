#pragma once

#include "cgraph/language_config.hpp"
#include "cgraph/operation_stats.hpp"
#include "cgraph/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

// Contract discovery (CGR-13): the wire contracts a repo provides and consumes,
// found in its own source rather than typed into a seam spec: HTTP
// endpoints, tables, graph labels, headers, JWT claims, env names and DynamoDB
// tables. Extraction records raw facts as RawRelation entries:
//
//   "route"        source_id = the inline handler's function node, target_label =
//                  the module-level identifier of the router chain it is
//                  registered on (`notebookRoutes` in `notebookRoutes.get('/x',
//                  handler)`; empty when the extractor could not root the chain),
//                  context = "<verb> <path>", the path beneath any enclosing
//                  `.group('/p')` / inline-constructor prefix.
//   "file_route"   the same for an exported GET/POST/... in a Next.js
//                  `app/**/route.ts`: no chain, the path is the file's.
//   "mounts"       source_id = the parent chain's variable node, target_label =
//                  the child chain's identifier (`apiRoutes.use(notebookRoutes)`,
//                  `app.use('/api', router)`, `app.route('/api', sub)`), context =
//                  the mount path when the framework takes one, else empty.
//   "aliases"      source_id = a variable whose value is another identifier
//                  (`export const deckModule = deckRoutes as unknown as Elysia`):
//                  the same chain under a second name.
//   "http_call"    source_id = the function (or module-level variable, or file)
//                  the call sits in, target_label = the client it goes through
//                  (`fetch`, `api.GET`, `axios.post`, or a wrapper function's
//                  name), context = "<METHOD or empty> <path template>" with
//                  `{}` for interpolated segments; an empty path means the URL
//                  was a local variable or an absolute external URL.
//   "http_wrapper" source_id = a function whose own client call appends its
//                  first parameter to a fixed prefix (`apiFetch(path)` calling
//                  fetch(`${base}${path}`)), context = "<fixed METHOD or empty>
//                  <prefix>". Calls to it are consumers of prefix + argument.
//                  The method may be choices (`POST|DELETE`), end in `?` when
//                  the wrapper's own options are unreadable, and end in
//                  `~<index>` when a caller's options at that argument index
//                  override it (`{ ...init }`); calls to such a wrapper are
//                  read from their `http_call_args`.
//                  A call on a LangGraph SDK `Client` (`@langchain/langgraph-sdk`)
//                  is an `http_call` too, labelled `client.runs.stream`, with the
//                  verb and path that SDK method sends (`POST /threads/{}/runs/stream`).
//   "langgraph_client" source_id = a module function whose every return is a
//                  `new Client(...)` of the SDK (`createLangGraphClient`).
//   "langgraph_call" source_id as for "http_call", target_label = the imported
//                  function the client came from, context = "<METHOD> <path>":
//                  a consumer once that name resolves to a `langgraph_client`.
//   "maps_table"   source_id = a module-level ORM model variable, target_label =
//                  the SQL table it declares (`pgTable('competitors', ...)`).
//                  Resolves to a `maps_table` edge from the variable to the
//                  migration's `sql_table:<name>` node when one exists.
//   "provides_contract" / "uses_contract"
//                  Contracts other than HTTP endpoints. source_id = the code
//                  that provides the contract (a migration creating a table, a
//                  handler reading a header, a token builder writing a claim)
//                  or uses it (a query, a client setting the header, a decoder
//                  reading the claim), context = "<kind>:<name>" with kind one
//                  of `table`, `label` (a graph database node label or
//                  relationship type), `header`, `claim`, `env`, `dynamo` (a
//                  DynamoDB table, dynamo_contracts.hpp), and name as the
//                  code spells it. For `table` and `label` target_label is the
//                  database the extractor knows the name lives in, empty when
//                  it knows none (the usual case: code rarely proves which
//                  database a connection reaches). For a `header` provider it
//                  is kBoundHeaderRead when the framework binds the read to a
//                  request itself, else empty. For `dynamo` it is the env
//                  variables whose default the name is (`X,Y`), kept as the
//                  node's `env` property. It is ignored otherwise.
//                  A header provider that is not bound provides only when
//                  something reaches its function: a CALLS, imports or
//                  references edge, or a route's `handled_by`, from code
//                  outside test sources (is_test_source_path). A read in a
//                  helper no code calls serves no request (route_resolution
//                  counts it as contract_reads_unreached). resolve_contracts mints one node per contract id
//                  (contract_id below): kind = the contract kind, label = the
//                  name as a provider spells it (a user's spelling when no
//                  provider is in this repo, with `served: false`), properties
//                  `name` and, for tables and labels, `database`; `handled_by`
//                  from the contract to each provider, `CONSUMES` from each
//                  user to the contract, `contains` from a provider's file.
//
// Contract ids are raw (never make_id'd) so that, like endpoints, the same
// contract in two repos' graphs is the same id:
//
//   table:<database>:<name>   label:<database>:<name>
//   header:<name lowercased>  claim:<name>  env:<name>  dynamo:<name>
//
// A table or label with no known database is `table:local:<name>`: local to
// its repo, never the same node as another repo's `table:local:<name>` (two
// services each with their own `users` table must not join). It joins another
// repo only where a workspace manifest or a seam command declares that both
// repos use one database (contract_declarations.hpp), which spells it
// `table:<declared database>:<name>` at the crossing. An env variable likewise
// crosses only where a declaration names the service it addresses, and a
// standard HTTP header (`authorization`, `content-type`) or a standard JWT
// claim (`exp`, `email`) never does: every service uses those for its own
// reasons. Where a declaration names the repos whose tokens one issuer mints,
// their claims cross between those repos only, at `claim:<issuer>:<name>`
// (every name but the RFC 7519 registered ones). Claim facts come only from
// provably-JWT code (claim_contracts.hpp).
// A DynamoDB table (`dynamo:<name>`, case-sensitive) is its own kind, never a
// `table:`, and crosses by name: its name is its whole address in an AWS
// account, with no database between to declare. Nothing in code says which
// account or region a service uses, so one name in two accounts (two services
// each with their own `sessions` table) joins too; an account declaration is a
// recorded follow-up (dynamo_contracts.hpp).
// is_bridged_contract says which ids cross repositories as they are.
//
// Extractors normalize names before emitting a fact: an unquoted SQL
// identifier is folded to lower case, as Postgres does (`Users` is `users`),
// and a schema qualifier is dropped (`public.users` is `users`); a quoted
// identifier keeps its spelling. An extractor never passes `local` as a
// database (contract_id refuses it).
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
// The id is canonical: every parameter segment (`:id`, `{id}`, `[id]`, a
// template `${id}`) is `{}` in it, and it carries no repo, so the same route
// consumed in another repo's graph is the same node and the two graphs join by
// construction. The label keeps the provider's spelling (`GET /notebooks/:id`).
// Consumer calls (`http_call`, through `http_wrapper`s) then attach a `CONSUMES`
// edge from the calling function to the endpoint, minting the node (property
// `served: false`, no source) when this repo does not serve it. Endpoint nodes
// are rebuilt from scratch on every resolve, which is what keeps the
// incremental path correct: rebuild_graph re-merges every cached fragment and
// re-runs resolution.
namespace cgraph {

// The HTTP verbs a router DSL exposes as methods (lowercase).
[[nodiscard]] bool is_http_verb(std::string_view verb);

// Joins a mount prefix and a route path the way routers do: one slash between
// segments, duplicate slashes collapsed, no trailing slash except for the root.
// `/notebooks` + `/` is `/notebooks`; `` + `/health` is `/health`.
[[nodiscard]] std::string join_route_path(std::string_view prefix, std::string_view path);

// The id form of a path: `:id`, `{id}` and `[id]` segments become `{}`; `*`
// and literal segments stay. `/notebooks/:id/notes` and `/notebooks/{id}/notes`
// and a consumer's `/notebooks/{}/notes` all canonicalize to the last.
[[nodiscard]] std::string canonical_route_path(std::string_view path);

// The URL path a Next.js App Router `route.ts|js` file serves, from its path
// under the last `app` directory: `app/api/admin/connectors/[id]/route.ts` is
// `/api/admin/connectors/:id`. Route groups `(marketing)` and parallel-route
// slots `@modal` are dropped, `[id]` becomes `:id`, `[...slug]` and
// `[[...slug]]` become `*`. Empty when the file is not a route file.
[[nodiscard]] std::optional<std::string> next_route_path(std::string_view source_file);

// The scope of a table or label whose database nobody declared.
inline constexpr std::string_view kLocalDatabase = "local";

// The `target_label` of a `provides_contract header:` fact whose read the
// framework binds to a request without any call in the code: a Spring
// `@RequestHeader` parameter, a FastAPI `Header()` parameter, Ktor's
// `call.request.header` on the route's ApplicationCall, gin's `c.GetHeader` on
// a `*gin.Context` parameter, or `r.Header.Get` in a Go
// `func(http.ResponseWriter, *http.Request)` handler.
inline constexpr std::string_view kBoundHeaderRead = "bound";

// The contract kinds a `provides_contract` / `uses_contract` fact may name.
[[nodiscard]] bool is_contract_kind(std::string_view kind);

// The id a contract of `kind` named `name` has (see the forms above). `database`
// is used for `table` and `label` only; empty means kLocalDatabase. nullopt for
// an unknown kind, an empty name, or a database spelled with a `:` or spelled
// `local`.
[[nodiscard]] std::optional<std::string> contract_id(std::string_view kind, std::string_view name,
                                                     std::string_view database = {});

// The kind of a contract id (`endpoint`, `table`, `label`, `header`, `claim`,
// `env`, `dynamo`), empty when `id` is no contract id.
[[nodiscard]] std::string_view contract_kind_of(std::string_view id);

// True for an id that is the same contract in every repository's graph by
// itself: every endpoint and DynamoDB table, a claim outside the standard set
// below, a header outside the standard set below, and a table or label in a
// named database. A `table:local:` / `label:local:` id, every `env:` id, a
// standard claim and a standard header are not: a table or env variable crosses only where a
// declaration says so (contract_declarations.hpp crossing_id, which every
// cross-repo matcher uses), a standard header never, and a claim of a repo
// in a declared issuer only at that issuer's `claim:<issuer>:<name>`.
[[nodiscard]] bool is_bridged_contract(std::string_view id);

// True for a lowercased header name every service uses for its own reasons
// (IANA permanent registrations, common tracing and proxy headers): it never
// bridges repositories.
[[nodiscard]] bool is_standard_http_header(std::string_view name);
// The sorted table behind is_standard_http_header (`x-forwarded-*` is matched
// by prefix and not listed).
[[nodiscard]] std::span<const std::string_view> standard_http_headers();

// True for a claim name any issuer writes with the same meaning (the IANA JWT
// Claims registry's RFC 7519, OpenID Connect, RFC 7800, RFC 8693 and RFC 9449
// names: `exp`, `sub`, `email`, `scope`, `client_id`, ...): two repos naming
// one are not evidence that they share a token, so a standard claim never
// bridges repositories. Application claims (`roles`, `tenant_id`,
// `session_id`) do. Case-sensitive, as claim names are.
[[nodiscard]] bool is_standard_jwt_claim(std::string_view name);
// The sorted table behind is_standard_jwt_claim.
[[nodiscard]] std::span<const std::string_view> standard_jwt_claims();

// True for the RFC 7519 section 4.1 registered claims (`iss`, `sub`, `aud`,
// `exp`, `nbf`, `iat`, `jti`): the token's own envelope, never a value one
// service hands another. Inside a declared issuer (contract_declarations.hpp)
// every other claim crosses between its members, OpenID Connect ones included.
[[nodiscard]] bool is_registered_jwt_claim(std::string_view name);

// True for a `table:local:` or `label:local:` id.
[[nodiscard]] bool is_database_local_contract(std::string_view id);

// Mints endpoint nodes and their edges from the contract facts in
// raw_relations. Runs after resolve_imports (mount targets, chains and wrapper
// names resolve through the file's imports, then its own declarations).
// `stats`, when given, receives the tally that stats.json reports as
// route_resolution.
void resolve_contracts(GraphSnapshot& graph, std::span<const RawRelation> raw_relations,
                       ContractResolution* stats = nullptr);

}  // namespace cgraph
