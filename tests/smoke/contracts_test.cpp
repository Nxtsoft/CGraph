#include "cgraph/contracts.hpp"

#include "cgraph/graph_builder.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/normalize.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::cerr << "contracts_test: " << message << '\n';
  return 1;
}

struct Built {
  cgraph::GraphSnapshot graph;
  cgraph::ContractResolution stats;
};

// Extracts every (path, source) with the real JS/TS extractors, merges, resolves
// imports, then resolves contracts: the same order run_one_shot and the
// incremental rebuild use.
Built build(const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<cgraph::Fragment> fragments;
  std::vector<cgraph::RawRelation> relations;
  for (const auto& [path, source] : files) {
    const auto result = path.ends_with(".js")
                            ? cgraph::extract_javascript({.source_file = path, .relative_path = path, .source = source})
                            : cgraph::extract_typescript({.source_file = path, .relative_path = path, .source = source});
    fragments.push_back(result.fragment);
    relations.insert(relations.end(), result.raw_relations.begin(), result.raw_relations.end());
  }
  Built built;
  built.graph = cgraph::merge_fragments(fragments);
  cgraph::resolve_imports(built.graph);
  cgraph::resolve_contracts(built.graph, relations, &built.stats);
  return built;
}

const cgraph::Node* endpoint(const cgraph::GraphSnapshot& graph, std::string_view label) {
  for (const auto& node : graph.nodes) {
    if (node.kind == "endpoint" && node.label == label) {
      return &node;
    }
  }
  return nullptr;
}

std::size_t endpoints(const cgraph::GraphSnapshot& graph) {
  std::size_t count = 0;
  for (const auto& node : graph.nodes) {
    count += node.kind == "endpoint" ? 1 : 0;
  }
  return count;
}

bool has_edge(const cgraph::GraphSnapshot& graph, std::string_view source, std::string_view target, std::string_view relation) {
  for (const auto& edge : graph.edges) {
    if (edge.source == source && edge.target == target && edge.relation == relation) {
      return true;
    }
  }
  return false;
}

std::string edge_property(const cgraph::GraphSnapshot& graph, std::string_view source, std::string_view target,
                          std::string_view relation, const std::string& key) {
  for (const auto& edge : graph.edges) {
    if (edge.source == source && edge.target == target && edge.relation == relation) {
      const auto slot = edge.properties.find(key);
      return slot == edge.properties.end() ? std::string{} : slot->second;
    }
  }
  return {};
}

int test_join_route_path() {
  const std::vector<std::pair<std::pair<std::string, std::string>, std::string>> cases = {
      {{"/notebooks", "/"}, "/notebooks"},          // Elysia's `.get('/')` under a prefix
      {{"", "/health"}, "/health"},                 // no prefix
      {{"/api/v1", ""}, "/api/v1"},                 // empty route
      {{"", ""}, "/"},                              // the root
      {{"/api/v1/", "/notebooks/"}, "/api/v1/notebooks"},  // stray slashes collapse
      {{"/api", "items"}, "/api/items"},            // a path with no leading slash
      {{"/", "/"}, "/"},
  };
  for (const auto& [input, expected] : cases) {
    if (const auto actual = cgraph::join_route_path(input.first, input.second); actual != expected) {
      return fail("join_route_path('" + input.first + "', '" + input.second + "') = '" + actual + "', want '" + expected + "'");
    }
  }
  return 0;
}

int test_next_route_path() {
  const std::vector<std::pair<std::string, std::string>> routed = {
      {"/r/app/api/admin/connectors/[id]/disable/route.ts", "/api/admin/connectors/:id/disable"},
      {"/r/src/app/(marketing)/pricing/route.ts", "/pricing"},       // route group dropped
      {"/r/app/@modal/login/route.ts", "/login"},                    // parallel-route slot dropped
      {"/r/app/docs/[...slug]/route.ts", "/docs/*"},                 // catch-all
      {"/r/app/docs/[[...slug]]/route.js", "/docs/*"},               // optional catch-all
      {"/r/app/route.ts", "/"},                                      // the app root
      {"/Users/x/app/proj/app/api/route.ts", "/api"},                // the LAST app directory wins
  };
  for (const auto& [file, expected] : routed) {
    const auto actual = cgraph::next_route_path(file);
    if (!actual || *actual != expected) {
      return fail("next_route_path(" + file + ") = '" + actual.value_or("<none>") + "', want '" + expected + "'");
    }
  }
  for (const auto* file : {"/r/lib/route.ts", "/r/app/api/x/page.tsx", "/r/app/api/x/route.test.ts", "/r/app/api/x/router.ts"}) {
    if (cgraph::next_route_path(file)) {
      return fail(std::string("next_route_path should reject ") + file);
    }
  }
  return 0;
}

// turing-api's shape: a module's chain carries its own prefix, a suite mounts
// it with a TS2589-dodging cast, the api chain mounts the suite under /api/v1,
// and app mounts the api chain. Every hop is an import.
int test_elysia_mount_chain() {
  const auto built = build({
      {"/proj/src/modules/notebooks.ts", R"ts(
import { Elysia } from 'elysia';
import { authWithDbUser } from '../middleware/auth';
export const notebookRoutes = new Elysia({ prefix: '/notebooks', tags: ['Notebooks'] })
  .use(authWithDbUser)
  .get('/starred-notes', async ({ dbUser }) => {
    return starred(dbUser);
  }, { detail: { summary: 'Starred' } })
  .post('/:id/notes', async ({ body }) => {
    return createNote(body);
  });
)ts"},
      {"/proj/src/modules/module.ts", R"ts(
import { Elysia } from 'elysia';
import { notebookRoutes } from './notebooks';
export const notebooksModule: Elysia = new Elysia()
  .use(notebookRoutes) as unknown as Elysia;
)ts"},
      {"/proj/src/app.ts", R"ts(
import { Elysia } from 'elysia';
import { cors } from '@elysiajs/cors';
import { notebooksModule } from './modules/module';
const apiRoutes = new Elysia({ prefix: '/api/v1' })
  .use(notebooksModule as any);
const app = new Elysia()
  .use(cors())
  .use(apiRoutes as any);
app.get('/health', () => 'ok');
export default app;
)ts"},
  });
  const auto& graph = built.graph;
  const auto* starred = endpoint(graph, "GET /api/v1/notebooks/starred-notes");
  const auto* create = endpoint(graph, "POST /api/v1/notebooks/:id/notes");
  const auto* health = endpoint(graph, "GET /health");
  if (starred == nullptr || create == nullptr || health == nullptr) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  minted: " << node.label << '\n';
    }
    return fail("expected three endpoints with fully composed paths");
  }
  if (starred->id != "endpoint:GET /api/v1/notebooks/starred-notes") {
    return fail("an endpoint id is `endpoint:<METHOD> <path>` with no repo in it: " + starred->id);
  }
  if (starred->properties.at("method") != "GET" || starred->properties.at("path") != "/api/v1/notebooks/starred-notes") {
    return fail("method/path properties");
  }
  // The endpoint is anchored on its handler: same file and span, contained by
  // the file, and handled_by the handler so impact on the handler reaches it.
  const auto handler = cgraph::make_id("/proj/src/modules/notebooks.ts:notebookRoutes.get /starred-notes");
  if (starred->source_file != "/proj/src/modules/notebooks.ts" || !starred->source_location ||
      starred->source_location->start_line != 6) {
    return fail("endpoint source anchor is the handler's");
  }
  if (!has_edge(graph, starred->id, handler, "handled_by")) {
    return fail("endpoint -> handler handled_by edge");
  }
  if (!has_edge(graph, cgraph::make_id("/proj/src/modules/notebooks.ts"), starred->id, "contains")) {
    return fail("file contains endpoint");
  }
  // Mounts became edges between the chain variables, resolved through imports.
  const auto api_routes = cgraph::make_id("/proj/src/app.ts:apiRoutes");
  const auto app = cgraph::make_id("/proj/src/app.ts:app");
  const auto suite = cgraph::make_id("/proj/src/modules/module.ts:notebooksModule");
  const auto routes = cgraph::make_id("/proj/src/modules/notebooks.ts:notebookRoutes");
  if (!has_edge(graph, api_routes, suite, "mounts") || !has_edge(graph, suite, routes, "mounts") ||
      !has_edge(graph, app, api_routes, "mounts")) {
    return fail("mounts edges through casts and imports");
  }
  // `.use(authWithDbUser)` names a middleware import no file declares:
  // unresolved. `.use(cors())` is a call, not a mount at all.
  if (built.stats.routes != 3 || built.stats.routes_unresolved != 0 || built.stats.endpoints != 3 ||
      built.stats.mounts != 4 || built.stats.mounts_unresolved != 1) {
    std::cerr << "  routes " << built.stats.routes << " unresolved " << built.stats.routes_unresolved
              << " endpoints " << built.stats.endpoints << " mounts " << built.stats.mounts << " unresolved "
              << built.stats.mounts_unresolved << '\n';
    return fail("route_resolution tally");
  }
  if (endpoints(graph) != 3) {
    return fail("exactly three endpoint nodes");
  }
  return 0;
}

// Express: the mount carries the path; a router mounted twice serves its routes
// twice; a router nobody mounts is served at its own paths.
int test_express_mount_prefixes() {
  const auto built = build({
      {"/proj/srv/items.js", R"js(
import { Router } from 'express';
export const router = Router();
router.get('/items', (req, res) => { res.json(list()); });
)js"},
      {"/proj/srv/admin.js", R"js(
import { Router } from 'express';
export const admin = Router();
admin.get('/stats', (req, res) => { res.json(stats()); });
)js"},
      {"/proj/srv/server.js", R"js(
import express from 'express';
import cors from 'cors';
import { router } from './items';
const app = express();
app.use(cors());
app.use('/api', router);
app.use('/legacy', router);
app.listen(3000);
)js"},
  });
  const auto& graph = built.graph;
  const auto* api = endpoint(graph, "GET /api/items");
  const auto* legacy = endpoint(graph, "GET /legacy/items");
  const auto* stats = endpoint(graph, "GET /stats");
  if (api == nullptr || legacy == nullptr || stats == nullptr || endpoints(graph) != 3) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  minted: " << node.label << '\n';
    }
    return fail("express endpoints: /api/items, /legacy/items, unmounted /stats");
  }
  const auto handler = cgraph::make_id("/proj/srv/items.js:router.get /items");
  if (!has_edge(graph, api->id, handler, "handled_by") || !has_edge(graph, legacy->id, handler, "handled_by")) {
    return fail("both mount paths are handled by the one handler");
  }
  const auto app = cgraph::make_id("/proj/srv/server.js:app");
  const auto router = cgraph::make_id("/proj/srv/items.js:router");
  if (edge_property(graph, app, router, "mounts", "prefix") != "/api") {
    return fail("the mount edge records its path (the second mount of the same pair is one edge)");
  }
  if (built.stats.mounts != 2 || built.stats.mounts_unresolved != 0 || built.stats.routes != 2 ||
      built.stats.endpoints != 3) {
    return fail("express tally");
  }
  return 0;
}

// A handler on a chain whose mount is unknowable from its file -- a router
// passed in as a plain function parameter, a chain built inside a test
// callback -- is refused and counted, never minted at a guessed path. A
// module-level `app` in the same file must not capture the local one.
int test_unresolved_chain_is_refused() {
  const auto built = build({
      {"/proj/g/plugin.ts", R"ts(
import { Elysia } from 'elysia';
export const app = new Elysia({ prefix: '/real' }).get('/y', () => { return two(); });
export function register(app) {
  app.get('/x', () => { return one(); });
}
describe('routes', () => {
  const app = new Elysia();
  app.post('/probe', () => { return three(); });
});
)ts"},
  });
  if (endpoints(built.graph) != 1 || endpoint(built.graph, "GET /real/y") == nullptr) {
    for (const auto& node : built.graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  minted: " << node.label << '\n';
    }
    return fail("only the route on the module-level chain is minted");
  }
  // Neither the parameter `app` nor the test-local `const app` is the
  // module-level `app`: the extractor left both chains empty.
  if (endpoint(built.graph, "POST /real/probe") != nullptr || endpoint(built.graph, "GET /real/x") != nullptr) {
    return fail("a local or parameter chain must not bind to the module-level chain of the same name");
  }
  if (built.stats.routes != 3 || built.stats.routes_unresolved != 2 || built.stats.endpoints != 1) {
    std::cerr << "  routes " << built.stats.routes << " unresolved " << built.stats.routes_unresolved
              << " endpoints " << built.stats.endpoints << '\n';
    return fail("unresolved route tally");
  }
  return 0;
}

// The three turing-api shapes the first field test lost: an aliased import
// (`config as configModule`), an anonymous chain passed straight to `.use()`,
// and a module re-exported as a cast alias; plus `.group()` / `.guard()`
// callbacks, whose parameter is the enclosing chain under the group's path.
int test_inline_grouped_and_aliased_chains() {
  const auto built = build({
      {"/proj/i/app.ts", R"ts(
import { Elysia } from 'elysia';
import { config as configModule } from './config';
import { deckModule } from './deck';
import { authMiddleware } from './auth';
export const apiRoutes = new Elysia({ prefix: '/api/v1' })
  .use(configModule)
  .use(deckModule)
  .use(new Elysia({ prefix: '/inline' }).use(authMiddleware).get('/protected', () => { return one(); }))
  .group('/v2', (app) => app.get('/x', () => { return two(); }))
  .guard({ beforeHandle: check }, (app) => app.post('/guarded', () => { return three(); }));
)ts"},
      {"/proj/i/config.ts", R"ts(
import { Elysia } from 'elysia';
export const config = new Elysia({ prefix: '/config' }).get('/luna', () => { return four(); });
)ts"},
      {"/proj/i/deck.ts", R"ts(
import { Elysia } from 'elysia';
const deckRoutes = new Elysia({ prefix: '/notebook-docs' }).post('/:id/deck', () => { return five(); });
export const deckModule: Elysia = deckRoutes as unknown as Elysia;
)ts"},
  });
  const auto& graph = built.graph;
  for (const auto* label : {"GET /api/v1/config/luna", "POST /api/v1/notebook-docs/:id/deck",
                            "GET /api/v1/inline/protected", "GET /api/v1/v2/x", "POST /api/v1/guarded"}) {
    if (endpoint(graph, label) == nullptr) {
      for (const auto& node : graph.nodes) {
        if (node.kind == "endpoint") std::cerr << "  minted: " << node.label << '\n';
      }
      return fail(std::string("missing endpoint ") + label);
    }
  }
  if (endpoints(graph) != 5 || built.stats.routes != 5 || built.stats.routes_unresolved != 0) {
    return fail("five routes, five endpoints, none refused");
  }
  // Handlers on inline and grouped chains are named by the enclosing chain and
  // the path beneath it, so the label reads as the route is served.
  const auto* protected_route = endpoint(graph, "GET /api/v1/inline/protected");
  const auto* grouped = endpoint(graph, "GET /api/v1/v2/x");
  if (!has_edge(graph, protected_route->id, cgraph::make_id("/proj/i/app.ts:apiRoutes.get /inline/protected"), "handled_by") ||
      !has_edge(graph, grouped->id, cgraph::make_id("/proj/i/app.ts:apiRoutes.get /v2/x"), "handled_by")) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "function" && node.label.find(" /") != std::string::npos) std::cerr << "  handler: " << node.label << '\n';
    }
    return fail("inline and grouped handlers are labelled by the enclosing chain");
  }
  // The alias is a mount for composition but not an edge in the graph.
  const auto deck_module = cgraph::make_id("/proj/i/deck.ts:deckModule");
  const auto deck_routes = cgraph::make_id("/proj/i/deck.ts:deckRoutes");
  if (has_edge(graph, deck_module, deck_routes, "mounts")) {
    return fail("a cast alias is not a mounts edge");
  }
  if (!has_edge(graph, cgraph::make_id("/proj/i/app.ts:apiRoutes"), cgraph::make_id("/proj/i/config.ts:config"), "mounts")) {
    return fail("an aliased import resolves to the imported chain");
  }
  return 0;
}

// Next.js App Router: the file is the route, the exported capitalised verb is
// the method. A lowercase helper named `get` is not a route.
int test_next_route_file() {
  const auto built = build({
      {"/proj/web/app/api/admin/connectors/[id]/route.ts", R"ts(
export async function GET(req: Request) { return ok(); }
export const POST = async (req: Request) => { return ok(); };
export function get() { return helper(); }
function DELETE() { return no(); }
)ts"},
  });
  const auto* get = endpoint(built.graph, "GET /api/admin/connectors/:id");
  const auto* post = endpoint(built.graph, "POST /api/admin/connectors/:id");
  if (get == nullptr || post == nullptr) {
    for (const auto& node : built.graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  minted: " << node.label << '\n';
    }
    return fail("GET and POST exports of a route file are endpoints");
  }
  // The unexported DELETE is still module-level: Next.js would not serve it,
  // but the walker cannot see `export`; the lowercase `get` must not count.
  if (endpoints(built.graph) != 3 || endpoint(built.graph, "DELETE /api/admin/connectors/:id") == nullptr) {
    return fail("module-level verb functions are routes; lowercase helpers are not");
  }
  if (!has_edge(built.graph, get->id, cgraph::make_id("/proj/web/app/api/admin/connectors/[id]/route.ts:GET"), "handled_by")) {
    return fail("route file endpoint is handled by the exported function");
  }
  return 0;
}

// turing-api's app.ts: `const app = new Elysia()` beside `export type App =
// typeof app`. Name keys fold case, so the shared scope index calls `app`
// ambiguous; the chain lookup is exact and variable-only and must not be.
int test_type_named_like_chain() {
  const auto built = build({
      {"/proj/t/app.ts", R"ts(
import { Elysia } from 'elysia';
const app = new Elysia()
  .get('/health', () => { return ok(); });
export type App = typeof app;
export { app };
)ts"},
  });
  if (endpoints(built.graph) != 1 || endpoint(built.graph, "GET /health") == nullptr || built.stats.routes_unresolved != 0) {
    return fail("a type whose name folds to the chain's must not make the chain ambiguous");
  }
  return 0;
}

// Endpoint ids are canonical (`{}` for every parameter segment) so a consumer in
// another repo joins by construction; the label keeps the provider's spelling.
int test_canonical_ids() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"/api/v1/notebooks/:id/notes", "/api/v1/notebooks/{}/notes"},
      {"/api/v1/projects/{projectId}/publish", "/api/v1/projects/{}/publish"},
      {"/api/admin/[id]/disable", "/api/admin/{}/disable"},
      {"/docs/*", "/docs/*"},
      {"/notes/{}/star", "/notes/{}/star"},
      {"/api/v1/composites/", "/api/v1/composites"},  // an OpenAPI document's spelling of `.get('/')` under a prefix
      {"/", "/"},
  };
  for (const auto& [input, expected] : cases) {
    if (const auto actual = cgraph::canonical_route_path(input); actual != expected) {
      return fail("canonical_route_path(" + input + ") = " + actual + ", want " + expected);
    }
  }
  const auto built = build({
      {"/proj/k/app.ts", R"ts(
import { Elysia } from 'elysia';
export const app = new Elysia({ prefix: '/api/v1' }).get('/notebooks/:id/notes', () => { return one(); });
)ts"},
  });
  const auto* notes = endpoint(built.graph, "GET /api/v1/notebooks/:id/notes");
  if (notes == nullptr || notes->id != "endpoint:GET /api/v1/notebooks/{}/notes" ||
      notes->properties.at("path") != "/api/v1/notebooks/:id/notes") {
    return fail("a served endpoint keeps the provider's spelling in label and path, `{}` in the id");
  }
  return 0;
}

// Consumers: the webapp's shapes. A `fetch` with the host interpolated then a
// literal path; a wrapper (`apiFetch(path)` over `\`${base}${path}\`` with `base`
// a module const) whose callers are the consumers; openapi-fetch `api.GET`
// with `{id}` params; an intra-repo call to this repo's own Next.js route; a
// URL in a local variable and an absolute external URL, both refused; and
// `map.get('/key')`, which is no request at all.
int test_consumers() {
  const auto built = build({
      {"/proj/w/app/api/auth/token/route.ts", R"ts(
export async function GET(req: Request) { return ok(); }
)ts"},
      {"/proj/w/lib/hooks/notebooks/api.ts", R"ts(
const API_URL = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080';
const base = `${API_URL}/api/v1`;
async function apiFetch<T>(path: string, options?: RequestInit): Promise<T> {
  const headers = await getAuthHeaders();
  const res = await fetch(`${base}${path}`, { ...options, headers });
  return res.json();
}
export const notebooksApi = {
  list: () => apiFetch<Notebook[]>('/notebooks'),
  get: (id: string) => apiFetch<Notebook>(`/notebooks/${id}`),
  create: (input: CreateNotebookInput) =>
    apiFetch<Notebook>('/notebooks', { method: 'POST', body: JSON.stringify(input) }),
};
export async function starred() {
  return apiFetch('/notebooks/starred-notes');
}
)ts"},
      {"/proj/w/lib/project-visibility-api.ts", R"ts(
const API_BASE_URL = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080';
export async function publishProject(projectId: string) {
  const res = await fetch(`${API_BASE_URL}/api/v1/projects/${projectId}/publish`, { method: 'POST' });
  return res.json();
}
export async function post(path: string, body: unknown) {
  return fetch(`${API_BASE_URL}${path}`, { method: 'POST', body: JSON.stringify(body) });
}
export function suppliers() { return post('/api/v1/suppliers', {}); }
export async function token() {
  const res = await fetch('/api/auth/token');
  const url = buildUrl();
  const other = await fetch(url);
  const gh = await fetch('https://api.github.com/repos/x/y');
  const cache = new Map<string, number>();
  cache.get('/api/v1/not-a-request');
  return res;
}
)ts"},
      {"/proj/w/lib/config.ts", R"ts(
const API_URL = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080';
export const API_BASE = `${API_URL}/api/v1`;
export const AMBIGUOUS = `${API_URL}/one`;
)ts"},
      {"/proj/w/lib/other-config.ts", R"ts(
export const AMBIGUOUS = '/two';
)ts"},
      {"/proj/w/lib/definition-api.ts", R"ts(
import { API_BASE, AMBIGUOUS } from './config';
import { apiFetch } from './hooks/notebooks/api';
export async function del(path: string) { return fetch(`${API_BASE}${path}`, { method: 'DELETE' }); }
export function removeDefinition(id: string) { return del(`/definitions/${id}`); }
export function generic(entity: string, id: string) { return apiFetch(`/${entity}/${id}`); }
export async function ambiguous() { return fetch(`${AMBIGUOUS}/things`); }
)ts"},
      {"/proj/w/lib/projects.ts", R"ts(
import { api } from './api-client';
export async function loadProject(id: string) {
  const { data } = await api.GET('/api/v1/projects/{id}', { params: { path: { id } } });
  await api.DELETE('/api/v1/projects/{id}/members/{userId}', { params: { path: { id, userId: 'u' } } });
  return data;
}
)ts"},
  });
  const auto& graph = built.graph;
  const auto consumers_of = [&](std::string_view label) {
    std::vector<std::string> callers;
    const auto* node = endpoint(graph, label);
    if (node == nullptr) {
      return callers;
    }
    for (const auto& edge : graph.edges) {
      if (edge.relation == "CONSUMES" && edge.target == node->id) {
        callers.push_back(edge.source);
      }
    }
    std::ranges::sort(callers);
    return callers;
  };
  const auto notebooks_api = cgraph::make_id("/proj/w/lib/hooks/notebooks/api.ts:notebooksApi");
  // Wrapper: prefix `/api/v1` inlined from `base`, method from the call's options
  // or GET; the object-literal arrows attribute to the module-level variable.
  if (consumers_of("GET /api/v1/notebooks") != std::vector<std::string>{notebooks_api} ||
      consumers_of("GET /api/v1/notebooks/{}") != std::vector<std::string>{notebooks_api} ||
      consumers_of("POST /api/v1/notebooks") != std::vector<std::string>{notebooks_api} ||
      consumers_of("GET /api/v1/notebooks/starred-notes") !=
          std::vector<std::string>{cgraph::make_id("/proj/w/lib/hooks/notebooks/api.ts:starred")}) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  endpoint: " << node.label << '\n';
    }
    for (const auto& edge : graph.edges) {
      if (edge.relation == "CONSUMES") std::cerr << "  consumes: " << edge.source << " -> " << edge.target << '\n';
    }
    return fail("apiFetch wrapper consumers");
  }
  const auto* starred = endpoint(graph, "GET /api/v1/notebooks/starred-notes");
  if (starred->properties.at("served") != "false" || !starred->source_file.empty() ||
      starred->id != "endpoint:GET /api/v1/notebooks/starred-notes") {
    return fail("a consumed endpoint this repo does not serve is minted as served:false with no source");
  }
  // Direct fetch with the host interpolated; a segment parameter; a fixed-method wrapper.
  if (consumers_of("POST /api/v1/projects/{}/publish") !=
          std::vector<std::string>{cgraph::make_id("/proj/w/lib/project-visibility-api.ts:publishProject")} ||
      consumers_of("POST /api/v1/suppliers") !=
          std::vector<std::string>{cgraph::make_id("/proj/w/lib/project-visibility-api.ts:suppliers")}) {
    return fail("direct fetch and fixed-method wrapper consumers");
  }
  // openapi-fetch: verb from the property, `{id}` canonicalised.
  const auto load_project = cgraph::make_id("/proj/w/lib/projects.ts:loadProject");
  if (consumers_of("GET /api/v1/projects/{}") != std::vector<std::string>{load_project} ||
      consumers_of("DELETE /api/v1/projects/{}/members/{}") != std::vector<std::string>{load_project}) {
    return fail("openapi-fetch consumers");
  }
  // Intra-repo: the Next.js route file serves what `token()` fetches, so the
  // one endpoint node has both a handler and a consumer.
  const auto* own = endpoint(graph, "GET /api/auth/token");
  if (own == nullptr || own->properties.contains("served") ||
      consumers_of("GET /api/auth/token") !=
          std::vector<std::string>{cgraph::make_id("/proj/w/lib/project-visibility-api.ts:token")} ||
      !has_edge(graph, own->id, cgraph::make_id("/proj/w/app/api/auth/token/route.ts:GET"), "handled_by")) {
    return fail("a consumer of this repo's own route joins the served endpoint");
  }
  // A wrapper over an imported base constant resolves the constant project-wide
  // when one file defines it: `del(\`/definitions/${id}\`)` is `DELETE
  // /api/v1/definitions/{}`, not `DELETE /definitions/{}`. A name two files
  // define differently is refused; so is a call whose own path is all parameters.
  if (consumers_of("DELETE /api/v1/definitions/{}") !=
      std::vector<std::string>{cgraph::make_id("/proj/w/lib/definition-api.ts:removeDefinition")}) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  endpoint: " << node.label << '\n';
    }
    return fail("a wrapper over an imported base constant inlines the constant");
  }
  if (endpoint(graph, "GET /repos/x/y") != nullptr || endpoint(graph, "GET /api/v1/not-a-request") != nullptr ||
      endpoint(graph, "DELETE /definitions/{}") != nullptr || endpoint(graph, "GET /api/v1/{}/{}") != nullptr ||
      endpoint(graph, "GET /one/things") != nullptr || endpoint(graph, "GET /things") != nullptr) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  endpoint: " << node.label << '\n';
    }
    return fail("an external URL, a Map lookup, an ambiguous base and an all-parameter path are not endpoints");
  }
  std::size_t external = 0;
  for (const auto& node : graph.nodes) {
    external += node.kind == "endpoint" && node.properties.contains("served") ? 1 : 0;
  }
  // 10 resolved client calls: 4 apiFetch, publish, post('/api/v1/suppliers'), 2 api.*,
  // fetch('/api/auth/token'), del('/definitions/...'); 4 unresolved: fetch(url),
  // fetch('https://...'), the all-parameter apiFetch, the ambiguous base -> 14 counted.
  // 9 endpoints this repo does not serve (the 10 consumers minus the own route).
  if (built.stats.calls != 14 || built.stats.calls_unresolved != 4 || built.stats.consumes != 10 ||
      built.stats.endpoints_external != 9 || external != 9 || built.stats.endpoints != 1) {
    std::cerr << "  calls " << built.stats.calls << " unresolved " << built.stats.calls_unresolved << " consumes "
              << built.stats.consumes << " external " << built.stats.endpoints_external << '/' << external
              << " endpoints " << built.stats.endpoints << '\n';
    return fail("consumer tally");
  }
  return 0;
}

// A documented endpoint (an openapi-typescript `paths` member here) is the node a
// route serves and a client consumes: resolution attaches to it instead of
// minting, keeps its document anchor, and counts it.
int test_documented_endpoint_joins() {
  const auto built = build({
      {"/proj/d/lib/generated/api-types.d.ts", R"ts(
export interface paths {
    "/api/auth/token": { get: operations["getToken"]; post?: never; };
    "/api/v1/notebooks": { get: operations["listNotebooks"]; };
}
export interface components { schemas: never; }
export interface operations { getToken: { responses: { 200: { content: { "application/json": { token: string } } } } }; listNotebooks: { responses: { 200: { content: { "application/json": unknown } } } }; }
)ts"},
      {"/proj/d/app/api/auth/token/route.ts", R"ts(
export async function GET() { return ok(); }
)ts"},
      {"/proj/d/lib/client.ts", R"ts(
export async function listNotebooks() { return fetch('/api/v1/notebooks'); }
)ts"},
  });
  const auto& graph = built.graph;
  const auto* token = endpoint(graph, "GET /api/auth/token");
  const auto* notebooks = endpoint(graph, "GET /api/v1/notebooks");
  if (token == nullptr || notebooks == nullptr || endpoints(graph) != 2) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  endpoint: " << node.label << " " << node.source_file << '\n';
    }
    return fail("documented endpoints are single nodes shared with routes and consumers");
  }
  if (token->properties.at("documented") != "true" || !token->source_file.ends_with("api-types.d.ts") ||
      !has_edge(graph, token->id, cgraph::make_id("/proj/d/app/api/auth/token/route.ts:GET"), "handled_by") ||
      token->properties.contains("served")) {
    return fail("a served documented endpoint keeps its document anchor and gains handled_by");
  }
  if (!has_edge(graph, cgraph::make_id("/proj/d/lib/client.ts:listNotebooks"), notebooks->id, "CONSUMES") ||
      notebooks->properties.contains("served")) {
    return fail("a consumed documented endpoint gains CONSUMES and is not marked unserved");
  }
  if (built.stats.endpoints_documented != 2 || built.stats.endpoints != 0 || built.stats.endpoints_external != 0 ||
      built.stats.consumes != 1 || built.stats.routes != 1) {
    std::cerr << "  documented " << built.stats.endpoints_documented << " endpoints " << built.stats.endpoints << " external "
              << built.stats.endpoints_external << " consumes " << built.stats.consumes << '\n';
    return fail("documented tally");
  }
  return 0;
}

// A mount cycle terminates and still mints the route once.
int test_mount_cycle_terminates() {
  const auto built = build({
      {"/proj/c/a.ts", R"ts(
import { Elysia } from 'elysia';
import { b } from './b';
export const a = new Elysia({ prefix: '/a' }).use(b).get('/x', () => { return one(); });
)ts"},
      {"/proj/c/b.ts", R"ts(
import { Elysia } from 'elysia';
import { a } from './a';
export const b = new Elysia({ prefix: '/b' }).use(a);
)ts"},
  });
  if (endpoints(built.graph) != 1 || built.stats.endpoints != 1 || built.stats.routes_unresolved != 0) {
    return fail("a mount cycle mints the route exactly once");
  }
  return 0;
}

// Resolution is a pure function of the merged fragments: running the pipeline
// twice over the same files yields the same endpoints, and a repo with no
// routes gains no nodes or edges at all (Graphify parity for every other repo).
int test_no_routes_no_change() {
  const auto built = build({
      {"/proj/lib/util.ts", R"ts(
export function add(a: number, b: number) { return a + b; }
const cache = new Map<string, number>();
cache.get('x');
)ts"},
  });
  if (endpoints(built.graph) != 0 || built.stats.routes != 0 || built.stats.mounts != 0) {
    return fail("a repo without routers gains nothing");
  }
  for (const auto& edge : built.graph.edges) {
    if (edge.relation == "mounts" || edge.relation == "handled_by") {
      return fail("no contract edges without routes");
    }
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += test_join_route_path();
  failures += test_next_route_path();
  failures += test_elysia_mount_chain();
  failures += test_express_mount_prefixes();
  failures += test_unresolved_chain_is_refused();
  failures += test_inline_grouped_and_aliased_chains();
  failures += test_next_route_file();
  failures += test_type_named_like_chain();
  failures += test_canonical_ids();
  failures += test_consumers();
  failures += test_documented_endpoint_joins();
  failures += test_mount_cycle_terminates();
  failures += test_no_routes_no_change();
  return failures == 0 ? 0 : 1;
}
