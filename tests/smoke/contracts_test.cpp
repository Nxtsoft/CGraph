#include "cgraph/contracts.hpp"

#include "cgraph/configured_extractors.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/non_grammar_extractors.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/operation_stats.hpp"
#include "cgraph/python_extractor.hpp"

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
    const cgraph::ExtractionContext context{.source_file = path, .relative_path = path, .source = source};
    const auto result = path.ends_with(".sql")    ? *cgraph::extract_non_grammar_language(cgraph::DetectedLanguage::Sql, context)
                        : path.ends_with(".kt")   ? *cgraph::extract_configured_language(cgraph::DetectedLanguage::Kotlin, context)
                        : path.ends_with(".java") ? *cgraph::extract_configured_language(cgraph::DetectedLanguage::Java, context)
                        : path.ends_with(".py")   ? cgraph::extract_python(context)
                        : path.ends_with(".js")   ? cgraph::extract_javascript(context)
                                                  : cgraph::extract_typescript(context);
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

// A mount on a name that is no chain. In Python (`app = create_app()`) the
// child is served somewhere nobody can place, so it mints nothing unless
// another mount places it; a JavaScript child keeps its own path, as before.
int test_unplaced_mounts() {
  const auto built = build({
      {"/proj/u/items.js", R"js(
import { Router } from 'express';
export const router = Router();
router.get('/items', (req, res) => { res.json(list()); });
)js"},
      {"/proj/u/wire.js", R"js(
import { server } from './server';
import { router } from './items';
server.use('/api', router);
)js"},
      {"/proj/u/server.js", R"js(
export const server = makeServer();
)js"},
      {"/proj/u/main.py", R"py(
from fastapi import APIRouter, FastAPI
users = APIRouter(prefix="/users")
orders = APIRouter(prefix="/orders")

@users.get("/{user_id}")
def get_user(user_id: int):
    return user_id

@orders.get("/{order_id}")
def get_order(order_id: int):
    return order_id

app = create_app()
app.include_router(users, prefix="/api/v1")
app.include_router(orders, prefix="/api/v1")
real = FastAPI()
real.include_router(orders, prefix="/v2")
)py"},
  });
  const auto& graph = built.graph;
  if (endpoints(graph) != 2 || endpoint(graph, "GET /items") == nullptr ||
      endpoint(graph, "GET /v2/orders/{order_id}") == nullptr) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  minted: " << node.label << '\n';
    }
    return fail("unplaced mounts: JavaScript /items stays, Python users mints nothing, orders only under /v2");
  }
  if (built.stats.mounts != 4 || built.stats.mounts_unresolved != 3 || built.stats.routes_unresolved != 1) {
    std::cerr << "  mounts " << built.stats.mounts << " unresolved " << built.stats.mounts_unresolved
              << " routes_unresolved " << built.stats.routes_unresolved << '\n';
    return fail("unplaced mount tally");
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

// Spring MVC: a method's @GetMapping/@PostMapping/... (or @RequestMapping with a
// method) under the class-level @RequestMapping prefix is a file-routed endpoint
// handled by the method, in Kotlin and in Java. A path that is not a literal, or a
// method-level @RequestMapping with no method, mints nothing: a wrong endpoint is
// worse than none.
int test_spring_mappings() {
  const auto built = build({
      {"src/main/kotlin/UserController.kt", R"kt(
@RestController
@RequestMapping("/api/v1/users")
class UserController(private val users: UserService) {
    @GetMapping
    fun list(): List<User> = users.all()

    @GetMapping("/{id}", produces = [MediaType.APPLICATION_JSON_VALUE])
    fun get(@PathVariable id: String): User = users.find(id)

    @PostMapping("/{id}/roles", consumes = [MediaType.APPLICATION_JSON_VALUE])
    fun addRole(@PathVariable id: String) { users.addRole(id) }

    @RequestMapping(value = ["/sync", "/resync"], method = [RequestMethod.PUT])
    fun sync() { users.sync() }

    @GetMapping(ApiPaths.EXPORT)
    fun export() { users.export() }

    @RequestMapping("/any")
    fun any() { users.any() }

    fun helper() { users.helper() }
}
)kt"},
      {"src/main/kotlin/HealthController.kt", R"kt(
@RestController
class HealthController {
    @GetMapping("/health")
    fun health(): String = "ok"
}
)kt"},
      {"src/main/java/OrderController.java", R"java(
@RestController
@RequestMapping(path = "/api/orders")
public class OrderController {
    @GetMapping(value = {"", "/all"})
    public List<Order> list() { return null; }

    @DeleteMapping("/{orderId}")
    public void delete(@PathVariable String orderId) { }

    @RequestMapping(value = "/legacy", method = RequestMethod.POST)
    public void legacy() { }
}
)java"},
      {"web/users.ts", "export async function loadUsers() {\n  return fetch('/api/v1/users');\n}\n"},
  });
  const auto& graph = built.graph;
  const char* expected[] = {
      "GET /api/v1/users", "GET /api/v1/users/{id}", "POST /api/v1/users/{id}/roles",
      "PUT /api/v1/users/sync", "PUT /api/v1/users/resync", "GET /health",
      "GET /api/orders", "GET /api/orders/all", "DELETE /api/orders/{orderId}", "POST /api/orders/legacy",
  };
  for (const auto* label : expected) {
    if (endpoint(graph, label) == nullptr) {
      for (const auto& node : graph.nodes) {
        if (node.kind == "endpoint") std::cerr << "  endpoint: " << node.label << '\n';
      }
      return fail(std::string("missing Spring endpoint ") + label);
    }
  }
  if (endpoints(graph) != std::size(expected)) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  endpoint: " << node.label << '\n';
    }
    return fail("a constant path or a method-less @RequestMapping must mint no endpoint");
  }
  const auto* get = endpoint(graph, "GET /api/v1/users/{id}");
  if (get->id != "endpoint:GET /api/v1/users/{}") {
    return fail("a Spring endpoint id uses the canonical {} form: " + get->id);
  }
  const auto handler_of = [&](const cgraph::Node* ep, std::string_view label) {
    for (const auto& edge : graph.edges) {
      if (edge.source != ep->id || edge.relation != "handled_by") continue;
      for (const auto& node : graph.nodes) {
        if (node.id == edge.target && node.label.find(label) != std::string::npos) return true;
      }
    }
    return false;
  };
  if (!handler_of(get, "get") || !handler_of(endpoint(graph, "PUT /api/v1/users/resync"), "sync") ||
      !handler_of(endpoint(graph, "DELETE /api/orders/{orderId}"), "delete")) {
    return fail("each Spring endpoint is handled_by its annotated method");
  }
  // The TypeScript client's fetch links to the Kotlin endpoint.
  bool consumed = false;
  const auto* list = endpoint(graph, "GET /api/v1/users");
  for (const auto& edge : graph.edges) {
    consumed = consumed || (edge.relation == "CONSUMES" && edge.target == list->id);
  }
  if (!consumed) {
    return fail("a client fetch of the path consumes the Spring endpoint");
  }
  return 0;
}

// Only methods of a concrete controller class are handlers. An interface (an
// openapi-generator API, a Feign client that CALLS the route), an object, a
// companion object, an abstract or sealed base class and a top-level function
// mint nothing, and neither does a
// path that is not one plain literal. Kotlin's several positional paths, a
// non-HTTP *Mapping placed first, arrayOf(...), a qualified annotation name and
// a Java record all still mint.
int test_spring_non_handlers() {
  const auto built = build({
      {"src/main/kotlin/Clients.kt", R"kt(
@FeignClient(name = "users")
interface UserClient {
    @GetMapping("/internal/users/{id}")
    fun fetchUser(@PathVariable id: String): User
}

interface ProductApi {
    @GetMapping("/products")
    fun products(): List<Product>
}

@GetMapping("/toplevel")
fun topLevel() = "x"

@RestController
@RequestMapping("/v")
class ShapesController {
    @GetMapping("/one", "/two")
    fun many() = "m"

    @MessageMapping("/ws")
    @GetMapping("/after-message")
    fun afterMessage() = "a"

    @GetMapping(value = arrayOf("/arr"))
    fun arr() = "r"

    @org.springframework.web.bind.annotation.PostMapping("/fq")
    fun fq() = "f"

    @GetMapping("/a" + "/b")
    fun concat() = "c"

    @GetMapping("""/raw""")
    fun raw() = "w"

    companion object {
        @GetMapping("/companion")
        fun companionRoute() = "c"
    }

    object Nested {
        @GetMapping("/nested")
        fun nestedRoute() = "n"
    }
}
)kt"},
      {"src/main/java/OrdersApi.java", R"java(
@RequestMapping("/api/v2")
public interface OrdersApi {
    @GetMapping("/orders")
    String orders();
}
)java"},
      {"src/main/kotlin/Bases.kt", R"kt(
abstract class CrudController<T> {
    @GetMapping("/{id}")
    fun get(@PathVariable id: String): T? = null
}

@RequestMapping("/sealed")
sealed class SealedController {
    @GetMapping("/s")
    fun s() = "s"
}
)kt"},
      {"src/main/java/AbstractCtl.java", R"java(
public abstract class AbstractCtl {
    @GetMapping("/{id}")
    public String get(String id) { return id; }
}
)java"},
      {"src/main/java/StatusController.java", R"java(
@RestController
@RequestMapping("/status")
public record StatusController(String name) {
    @GetMapping("/ping")
    public String ping() { return "pong"; }
}
)java"},
  });
  const auto& graph = built.graph;
  const char* expected[] = {"GET /v/one", "GET /v/two", "GET /v/after-message", "GET /v/arr", "POST /v/fq", "GET /status/ping"};
  for (const auto* label : expected) {
    if (endpoint(graph, label) == nullptr) {
      for (const auto& node : graph.nodes) {
        if (node.kind == "endpoint") std::cerr << "  endpoint: " << node.label << '\n';
      }
      return fail(std::string("missing Spring endpoint ") + label);
    }
  }
  if (endpoints(graph) != std::size(expected)) {
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint") std::cerr << "  endpoint: " << node.label << '\n';
    }
    return fail("interfaces, Feign clients, objects, top-level functions and non-literal paths mint no endpoint");
  }
  return 0;
}

// A Drizzle model maps the table its migration creates, so impact from the
// table reaches the model. Only a string-literal name on pgTable / mysqlTable /
// sqliteTable counts, and a name no migration creates links nothing.
int test_orm_table_links() {
  const auto built = build({
      {"src/db/migrations/0001_init.sql", R"sql(
CREATE TABLE "competitors" ("id" uuid PRIMARY KEY);
CREATE TABLE IF NOT EXISTS "User_Library_Favorites" ("id" uuid PRIMARY KEY);
CREATE TABLE "sessions" ("id" uuid PRIMARY KEY);
)sql"},
      {"src/db/schema/library.ts", R"ts(
import { pgTable, mysqlTable, uuid } from 'drizzle-orm/pg-core';
export const competitors = pgTable('competitors', { id: uuid('id') });
export const favorites = pgTable("User_Library_Favorites", { id: uuid('id') }) as unknown as Table;
export const sessionsMy = mysqlTable('sessions', { id: uuid('id') });
export const orphan = pgTable('not_migrated', { id: uuid('id') });
const sessions = 'competitors';
export const dynamic = pgTable(sessions, { id: uuid('id') });
export const other = makeTable('competitors', {});
)ts"},
  });
  const auto& graph = built.graph;
  const auto var = [](std::string_view name) { return cgraph::make_id(std::string("src/db/schema/library.ts:") + std::string(name)); };
  const std::pair<std::string_view, std::string_view> linked[] = {
      {"competitors", "competitors"}, {"favorites", "User_Library_Favorites"}, {"sessionsMy", "sessions"}};
  for (const auto& [model, table] : linked) {
    if (!has_edge(graph, var(model), cgraph::make_id(std::string("sql_table:") + std::string(table)), "maps_table")) {
      return fail(std::string("model ") + std::string(model) + " does not map table " + std::string(table));
    }
  }
  std::size_t maps = 0;
  for (const auto& edge : graph.edges) {
    maps += edge.relation == "maps_table" ? 1 : 0;
  }
  if (maps != std::size(linked)) {
    return fail("an unmigrated name, a non-literal name or a non-Drizzle call mapped a table");
  }
  return 0;
}

// A wrapper whose path is a later parameter and whose method is a parameter
// (`mlBackendRequest(method, path)` forwarding into `mlRequest(base, method,
// path)`), called from another file, consumes `<method> <path>` from the
// call's own arguments, the parameter's default verb when a call leaves the
// method out; a relative path joins only a prefix ending in a slash.
int test_positional_and_relative_wrappers() {
  const auto built = build({
      {"/proj/p/src/ml/service.ts", R"ts(
const mlBackendBaseUrl = config.ml.mlBackendUrl;
async function mlRequest<T>(baseUrl: string, method: 'GET' | 'POST', path: string): Promise<T> {
  const url = `${baseUrl}${path}`;
  const response = await fetch(url, { method });
  return response.json();
}
export function mlBackendRequest<T>(method: 'GET' | 'POST', path: string, projectId?: string): Promise<T> {
  return mlRequest<T>(mlBackendBaseUrl, method, path);
}
const API_BASE = '/api/backend';
export async function apiRequest<T>(endpoint: string, options: RequestInit = {}): Promise<T> {
  const url = `${API_BASE}/${endpoint.replace(/^\//, '')}`;
  return (await fetch(url, { ...options })).json();
}
export async function apiFetch<T>(path: string): Promise<T> {
  return (await fetch(`${API_BASE}${path}`)).json();
}
export async function presenceFetch(path: string, method = 'GET') {
  return (await fetch(`/api/v1/notes${path}`, { method })).json();
}
)ts"},
      {"/proj/p/src/ml/callers.ts", R"ts(
import { mlBackendRequest, apiRequest, apiFetch, presenceFetch } from './service';
export async function setup(projectId: string) {
  return mlBackendRequest('POST', `/project/${projectId}/setup`, projectId);
}
export async function cluster(projectId: string) {
  return mlBackendRequest('GET', '/simulate/cluster', projectId);
}
export async function unknownMethod(projectId: string, verb: string) {
  return mlBackendRequest(verb, '/simulate/runs', projectId);
}
export async function refresh(id: string) {
  return apiRequest(`v1/service-providers/${id}/refresh-metadata`, { method: 'POST' });
}
export async function glued(id: string) {
  return apiFetch(`v1/users/${id}`);
}
export async function poll(id: string) {
  return presenceFetch(`/${id}/presence`);
}
export async function beat(id: string) {
  return presenceFetch(`/${id}/presence`, 'POST');
}
)ts"},
  });
  const auto& graph = built.graph;
  const auto consumes = [&](std::string_view caller, std::string_view endpoint_id) {
    return has_edge(graph, cgraph::make_id(std::string("/proj/p/src/ml/callers.ts:") + std::string(caller)),
                    endpoint_id, "CONSUMES");
  };
  if (!consumes("setup", "endpoint:POST /project/{}/setup") || !consumes("cluster", "endpoint:GET /simulate/cluster") ||
      !consumes("refresh", "endpoint:POST /api/backend/v1/service-providers/{}/refresh-metadata") ||
      // A call leaving the method out sends the parameter's default.
      !consumes("poll", "endpoint:GET /api/v1/notes/{}/presence") ||
      !consumes("beat", "endpoint:POST /api/v1/notes/{}/presence")) {
    for (const auto& edge : graph.edges) {
      if (edge.relation == "CONSUMES") std::cerr << "  consumes: " << edge.source << " -> " << edge.target << '\n';
    }
    return fail("positional and slash-joined wrapper consumers");
  }
  // A method the call does not spell out, and a relative path glued onto a
  // prefix with no slash (`/api/backendv1/...`), are counted, never guessed.
  for (const auto& node : graph.nodes) {
    if (node.kind == "endpoint" && (node.label.find("/simulate/runs") != std::string::npos ||
                                    node.label.find("users") != std::string::npos)) {
      return fail("an unknown method or an unjoinable relative path minted an endpoint: " + node.label);
    }
  }
  if (built.stats.calls != 7 || built.stats.calls_unresolved != 2 || built.stats.consumes != 5) {
    std::cerr << "  calls " << built.stats.calls << " unresolved " << built.stats.calls_unresolved << " consumes "
              << built.stats.consumes << '\n';
    return fail("positional wrapper tally");
  }
  return 0;
}

// A positional wrapper that fixes no method takes it from the options right
// after the path: left out, or an object with no `method`, is GET; a literal
// method (inline or in a local) is that verb; options the caller's file cannot
// read are counted unresolved, never guessed GET.
int test_positional_wrapper_options() {
  const auto built = build({
      {"/proj/p/src/api/request.ts", R"ts(
const API = '/api/v1';
export async function authed(headers: Record<string, string>, path: string, init?: RequestInit) {
  return fetch(`${API}${path}`, init);
}
)ts"},
      {"/proj/p/src/api/callers.ts", R"ts(
import { authed } from './request';
export async function absent(h: H) { return authed(h, '/absent'); }
export async function noMethod(h: H) { return authed(h, '/no-method', { headers: h }); }
export async function literal(h: H) { return authed(h, '/literal', { method: 'POST' }); }
export async function viaLocal(h: H) { const opts = { method: 'PATCH' }; return authed(h, '/via-local', opts); }
export async function unreadable(h: H) { return authed(h, '/unreadable', buildOptions()); }
export async function unreadableMethod(h: H) { return authed(h, '/unreadable-method', { method: pick() }); }
)ts"},
  });
  const auto& graph = built.graph;
  const auto consumes = [&](std::string_view caller, std::string_view endpoint_id) {
    return has_edge(graph, cgraph::make_id(std::string("/proj/p/src/api/callers.ts:") + std::string(caller)),
                    endpoint_id, "CONSUMES");
  };
  if (!consumes("absent", "endpoint:GET /api/v1/absent") || !consumes("noMethod", "endpoint:GET /api/v1/no-method") ||
      !consumes("literal", "endpoint:POST /api/v1/literal") || !consumes("viaLocal", "endpoint:PATCH /api/v1/via-local")) {
    for (const auto& edge : graph.edges) {
      if (edge.relation == "CONSUMES") std::cerr << "  consumes: " << edge.source << " -> " << edge.target << '\n';
    }
    return fail("positional wrapper options");
  }
  for (const auto& node : graph.nodes) {
    if (node.kind == "endpoint" && node.label.find("unreadable") != std::string::npos) {
      return fail("unreadable options minted an endpoint: " + node.label);
    }
  }
  if (built.stats.calls != 6 || built.stats.calls_unresolved != 2 || built.stats.consumes != 4) {
    std::cerr << "  calls " << built.stats.calls << " unresolved " << built.stats.calls_unresolved << " consumes "
              << built.stats.consumes << '\n';
    return fail("positional wrapper options tally");
  }
  return 0;
}

// A wrapper whose path is its first parameter reads its callers' method where
// it takes their options (#147): `apiFetch(path, authHeaders, init)` spreads
// the third argument, so the second (headers) says nothing. A wrapper that
// forwards no options sends its own method whatever a caller passes; one whose
// own options are unreadable (`decorate(init)`) leaves every caller unresolved,
// and one whose own method is unreadable but spreads the caller's options after
// it takes a method only a caller spells; a
// wrapper choosing between two verbs consumes both. A path held in a constant,
// a local or `new URL(...)` reaches such a wrapper as it reaches `fetch`. A
// same-class call into a wrapper that takes no options sends the wrapper's GET.
int test_first_parameter_wrapper_options() {
  const auto built = build({
      {"/proj/q/src/api.ts", R"ts(
const API = '/api/v1';
export async function apiFetch<T>(path: string, authHeaders: Record<string, string>, init?: RequestInit) {
  return fetch(`${API}${path}`, { ...init, headers: { ...authHeaders } });
}
export async function plain(path: string) { return fetch(`${API}${path}`); }
export async function built(path: string, init?: RequestInit) { return fetch(`${API}${path}`, decorate(init)); }
export async function mixed(path: string, init?: RequestInit) { return fetch(`${API}${path}`, { method: pick(), ...init }); }
export async function toggle(path: string, on: boolean) {
  return fetch(`${API}${path}`, { method: on ? 'POST' : 'DELETE' });
}
export async function jsonFetch(url: string, init?: RequestInit) { return fetch(url, { ...init }); }
export class Client {
  raw(path: string) { return fetch(`${API}${path}`); }
  save() { return this.raw('/saved', { method: 'POST' }); }
}
)ts"},
      {"/proj/q/src/callers.ts", R"ts(
import { apiFetch, plain, built, mixed, toggle, jsonFetch } from './api';
const authHeaders = { Authorization: 'Bearer x' };
const HOST = process.env.HOST;
const LINKS_URL = `${HOST}/api/v1/links/`;
export async function listInputs() { return apiFetch('/inputs', authHeaders); }
export async function createInput() { return apiFetch('/inputs', authHeaders, { method: 'POST', body: '{}' }); }
export async function topLevel(id: string) {
  return apiFetch(`/composites/${id}/top-level`, authHeaders, { method: 'PATCH' });
}
export async function unreadable() { return apiFetch('/unreadable', authHeaders, makeInit()); }
export async function ignored() { return plain('/plain', { method: 'POST' }); }
export async function guessed() { return built('/built'); }
export async function spelled() { return built('/spelled', { method: 'PUT' }); }
export async function overridden() { return mixed('/mixed', { method: 'PUT' }); }
export async function bare() { return mixed('/bare'); }
export async function flip() { return toggle('/flags', true); }
export async function viaConst() { return jsonFetch(LINKS_URL, { method: 'POST' }); }
export async function viaUrl() {
  const sessions = new URL(`${HOST}/api/v1/sessions`);
  return jsonFetch(sessions.toString(), {});
}
export async function viaLocal() { const url = `${HOST}/api/v1/config`; return jsonFetch(url); }
)ts"},
  });
  const auto& graph = built.graph;
  const auto consumes = [&](std::string_view caller, std::string_view endpoint_id) {
    return has_edge(graph, cgraph::make_id(std::string("/proj/q/src/callers.ts:") + std::string(caller)),
                    endpoint_id, "CONSUMES");
  };
  if (!consumes("listInputs", "endpoint:GET /api/v1/inputs") ||
      !consumes("createInput", "endpoint:POST /api/v1/inputs") ||
      !consumes("topLevel", "endpoint:PATCH /api/v1/composites/{}/top-level") ||
      !consumes("ignored", "endpoint:GET /api/v1/plain") || !consumes("overridden", "endpoint:PUT /api/v1/mixed") ||
      !consumes("flip", "endpoint:POST /api/v1/flags") || !consumes("flip", "endpoint:DELETE /api/v1/flags") ||
      !consumes("viaConst", "endpoint:POST /api/v1/links") || !consumes("viaUrl", "endpoint:GET /api/v1/sessions") ||
      !consumes("viaLocal", "endpoint:GET /api/v1/config") || endpoint(graph, "GET /api/v1/saved") == nullptr) {
    for (const auto& edge : graph.edges) {
      if (edge.relation == "CONSUMES") std::cerr << "  consumes: " << edge.source << " -> " << edge.target << '\n';
    }
    return fail("first-parameter wrapper options");
  }
  for (const auto& node : graph.nodes) {
    if (node.kind == "endpoint" &&
        (node.label.find("unreadable") != std::string::npos || node.label.find("/built") != std::string::npos ||
         node.label.find("/spelled") != std::string::npos || node.label.find("/bare") != std::string::npos ||
         node.label == "GET /api/v1/composites/{}/top-level" || node.label == "POST /api/v1/plain" ||
         node.label == "POST /api/v1/saved")) {
      return fail("a wrapper call's method was guessed: " + node.label);
    }
  }
  if (built.stats.calls != 14 || built.stats.calls_unresolved != 4 || built.stats.consumes != 11) {
    std::cerr << "  calls " << built.stats.calls << " unresolved " << built.stats.calls_unresolved << " consumes "
              << built.stats.consumes << '\n';
    return fail("first-parameter wrapper options tally");
  }
  return 0;
}

// A LangGraph SDK client made by a factory in another file (turing-webapp's
// `createLangGraphClient`) consumes the Agent Server routes its methods send:
// the factory's `langgraph_client` fact is what makes the importer's
// `langgraph_call` a request. An imported function that returns anything else,
// or one this project does not define, makes no request.
int test_langgraph_sdk_clients() {
  const auto built = build({
      {"/proj/w/lib/langgraph-client.ts", R"ts(
import { Client } from '@langchain/langgraph-sdk';
export function createLangGraphClient(accessToken: string, service: 'luna' | 'ic' = 'luna'): Client {
  const apiUrl = service === 'ic' ? process.env.NEXT_PUBLIC_IC_AGENTS_API_URL || 'https://a' : 'https://b';
  return new Client({ apiUrl, defaultHeaders: { Authorization: `Bearer ${accessToken}` } });
}
export function createOther(): Other { return new Other(); }
)ts"},
      {"/proj/w/lib/use-stream.ts", R"ts(
import { createLangGraphClient, createOther } from './langgraph-client';
import { makeClient } from 'some-package';
export async function send(token: string) {
  const client = createLangGraphClient(token, 'ic');
  const thread = await client.threads.create();
  for await (const chunk of client.runs.stream(thread.thread_id, 'luna', { input: {} })) {}
  return client.runs.wait(null, 'recap', {});
}
export async function notSdk(token: string, t: string) {
  const other = createOther();
  await other.threads.create();
  const x = makeClient(token);
  await x.runs.stream(t, 'luna', {});
}
)ts"},
      {"/proj/w/lib/module-clients.ts", R"ts(
import { Client } from '@langchain/langgraph-sdk';
let shared = new Client();
export function reset(other: Client) { shared = other; }
export function useShared() { return shared.threads.get('x'); }
const fixed = new Client();
export function subgraphs(id: string, ns?: string) {
  fixed.assistants.getSubgraphs(id, { namespace: 'child' });
  fixed.assistants.getSubgraphs(id, { namespace: '' });
  return fixed.assistants.getSubgraphs(id, { namespace: ns });
}
)ts"},
  });
  const auto& graph = built.graph;
  const auto send = cgraph::make_id("/proj/w/lib/use-stream.ts:send");
  if (!has_edge(graph, send, "endpoint:POST /threads", "CONSUMES") ||
      !has_edge(graph, send, "endpoint:POST /threads/{}/runs/stream", "CONSUMES") ||
      !has_edge(graph, send, "endpoint:POST /runs/wait", "CONSUMES")) {
    for (const auto& edge : graph.edges) {
      if (edge.relation == "CONSUMES") std::cerr << "  consumes: " << edge.source << " -> " << edge.target << '\n';
    }
    return fail("langgraph sdk client calls");
  }
  for (const auto& edge : graph.edges) {
    if (edge.relation == "CONSUMES" && edge.source == cgraph::make_id("/proj/w/lib/use-stream.ts:notSdk")) {
      return fail("a client no SDK factory made consumed " + edge.target);
    }
  }
  // A module `let` may be reassigned (`reset`): not read as a client. The SDK
  // tests `namespace` for truth: a non-empty literal is the namespaced route, an
  // empty one the plain route, a value it cannot read records nothing.
  const auto subgraphs = cgraph::make_id("/proj/w/lib/module-clients.ts:subgraphs");
  if (!has_edge(graph, subgraphs, "endpoint:GET /assistants/{}/subgraphs/{}", "CONSUMES") ||
      !has_edge(graph, subgraphs, "endpoint:GET /assistants/{}/subgraphs", "CONSUMES")) {
    return fail("langgraph getSubgraphs namespace");
  }
  for (const auto& edge : graph.edges) {
    if (edge.source == cgraph::make_id("/proj/w/lib/module-clients.ts:useShared") && edge.relation == "CONSUMES") {
      return fail("a reassigned module let was read as a client: " + edge.target);
    }
    // Contract facts are resolve_contracts' input, never code-graph edges.
    if (edge.relation == "langgraph_call" || edge.relation == "langgraph_client" || edge.relation == "http_call_args") {
      return fail("a contract fact leaked into the graph: " + edge.relation + " " + edge.source + " -> " + edge.target);
    }
  }
  if (built.stats.calls != 5 || built.stats.calls_unresolved != 0 || built.stats.consumes != 5) {
    std::cerr << "  calls " << built.stats.calls << " unresolved " << built.stats.calls_unresolved << " consumes "
              << built.stats.consumes << '\n';
    return fail("langgraph sdk client tally");
  }
  return 0;
}

const cgraph::Node* node_of(const cgraph::GraphSnapshot& graph, std::string_view id) {
  for (const auto& node : graph.nodes) {
    if (node.id == id) {
      return &node;
    }
  }
  return nullptr;
}

std::string property_of(const cgraph::Node& node, const std::string& key) {
  const auto value = node.properties.find(key);
  return value == node.properties.end() ? std::string{"<none>"} : value->second;
}

// Contracts other than endpoints, from raw `provides_contract` /
// `uses_contract` facts (no extractor emits them yet): one raw-id node per
// contract, `handled_by` to each provider, `CONSUMES` from each user,
// `contains` from a provider's file, `served: false` when only used, header
// names case-folded with the provider's spelling as label, a table with no
// database `local`, and every malformed fact counted, never minted.
int test_generic_contract_facts() {
  auto built = build({
      {"/proj/c/api/migrate.ts", "export function createUsers() { return 1; }\n"},
      {"/proj/c/api/server.ts", "export function readTenant(req: any) { return req; }\n"},
      // Something reaches readTenant: a header read in code nothing reaches provides nothing.
      {"/proj/c/api/router.ts", "import { readTenant } from './server';\nexport const route = (req: any) => readTenant(req);\n"},
      {"/proj/c/web/client.ts",
       "export function listUsers() { return 1; }\nexport function sendTenant() { return 2; }\n"},
  });
  auto& graph = built.graph;
  const auto create_users = cgraph::make_id("/proj/c/api/migrate.ts:createUsers");
  const auto read_tenant = cgraph::make_id("/proj/c/api/server.ts:readTenant");
  const auto list_users = cgraph::make_id("/proj/c/web/client.ts:listUsers");
  const auto send_tenant = cgraph::make_id("/proj/c/web/client.ts:sendTenant");
  for (const auto& id : {create_users, read_tenant, list_users, send_tenant}) {
    if (node_of(graph, id) == nullptr) {
      return fail("fixture function missing: " + id);
    }
  }
  const auto fact = [](std::string relation, std::string source, std::string context, std::string database = {},
                       std::string file = "/proj/c/web/client.ts") {
    return cgraph::RawRelation{.source_id = std::move(source), .target_label = std::move(database),
                               .relation = std::move(relation), .context = std::move(context),
                               .source_file = std::move(file)};
  };
  // Users come first in the list: a provider's spelling still wins the label.
  const std::vector<cgraph::RawRelation> facts{
      fact("uses_contract", list_users, "table:users"),
      fact("uses_contract", send_tenant, "header:x-tenant-id"),
      fact("provides_contract", create_users, "table:users", "", "/proj/c/api/migrate.ts"),
      fact("provides_contract", read_tenant, "header:X-Tenant-Id", "", "/proj/c/api/server.ts"),
      fact("uses_contract", list_users, "table:orders", "turing"),
      fact("uses_contract", list_users, "label:HAS_ROLE"),
      fact("uses_contract", send_tenant, "claim:org_id"),
      fact("uses_contract", send_tenant, "env:ML_BACKEND_URL"),
      // Malformed: unknown kind, no kind, empty name, a database with `:`, a source no node names.
      fact("uses_contract", send_tenant, "queue:jobs"),
      fact("uses_contract", send_tenant, "users"),
      fact("uses_contract", send_tenant, "claim:"),
      fact("uses_contract", list_users, "table:users", "a:b"),
      fact("uses_contract", "nobody", "header:x-tenant-id"),
      // An extractor may not spell the reserved scope of undeclared tables.
      fact("uses_contract", list_users, "table:accounts", "local"),
  };
  cgraph::resolve_contracts(graph, facts, &built.stats);

  const auto* users = node_of(graph, "table:local:users");
  if (users == nullptr || users->kind != "table" || users->label != "users" || property_of(*users, "database") != "local" ||
      property_of(*users, "name") != "users" || users->properties.contains("served") ||
      users->source_file != "/proj/c/api/migrate.ts") {
    return fail("a table with no database is table:local:<name>, kind table, anchored at its provider");
  }
  if (!has_edge(graph, "table:local:users", create_users, "handled_by") ||
      !has_edge(graph, list_users, "table:local:users", "CONSUMES") ||
      !has_edge(graph, cgraph::make_id("/proj/c/api/migrate.ts"), "table:local:users", "contains")) {
    return fail("a table contract is handled_by its provider, CONSUMED by its user, contained by the provider's file");
  }
  const auto* tenant = node_of(graph, "header:x-tenant-id");
  if (tenant == nullptr || tenant->kind != "header" || tenant->label != "X-Tenant-Id" ||
      !has_edge(graph, "header:x-tenant-id", read_tenant, "handled_by") ||
      !has_edge(graph, send_tenant, "header:x-tenant-id", "CONSUMES") || node_of(graph, "header:X-Tenant-Id") != nullptr) {
    return fail("a header is case-folded in its id and labelled as its provider spells it");
  }
  const auto* orders = node_of(graph, "table:turing:orders");
  if (orders == nullptr || property_of(*orders, "database") != "turing" || property_of(*orders, "served") != "false") {
    return fail("a table in a named database carries it; one only used is served:false");
  }
  for (const auto* id : {"label:local:HAS_ROLE", "claim:org_id", "env:ML_BACKEND_URL"}) {
    const auto* used = node_of(graph, id);
    if (used == nullptr || property_of(*used, "served") != "false" || !used->source_file.empty()) {
      return fail(std::string("a contract only used here is minted served:false with no anchor: ") + id);
    }
  }
  if (node_of(graph, "label:local:HAS_ROLE")->kind != "label" || node_of(graph, "env:ML_BACKEND_URL")->label != "ML_BACKEND_URL") {
    return fail("graph labels have kind label; env keeps its spelling");
  }
  for (const auto& node : graph.nodes) {
    if (node.id.starts_with("queue:") || node.id == "claim:" || node.id.find("a:b") != std::string::npos) {
      return fail("a malformed contract fact minted " + node.id);
    }
  }
  for (const auto& edge : graph.edges) {
    if (edge.relation == "uses_contract" || edge.relation == "provides_contract") {
      return fail("a contract fact leaked into the graph");
    }
  }
  // Read through the stats JSON, as stats.json and `status` report it.
  const auto tally = cgraph::contract_resolution_json(built.stats);
  if (node_of(graph, "table:local:accounts") != nullptr) {
    return fail("an extractor-supplied database `local` minted a table");
  }
  if (tally.value("contract_facts", 0) != 14 || tally.value("contract_facts_unresolved", 0) != 6 ||
      tally.value("contracts_provided", 0) != 2 || tally.value("contracts_external", 0) != 4 ||
      tally.value("contract_consumes", 0) != 6) {
    std::cerr << "  " << tally.dump() << '\n';
    return fail("contract tallies");
  }
  // Two repos' graphs name a shared header with the same id, and two repos'
  // undeclared tables with the same repo-local id that is_bridged_contract
  // refuses: those join only once a database is declared.
  if (!cgraph::is_bridged_contract("header:x-tenant-id") || !cgraph::is_bridged_contract("table:turing:orders") ||
      cgraph::is_bridged_contract("table:local:users") || cgraph::is_bridged_contract("label:local:HAS_ROLE") ||
      !cgraph::is_bridged_contract("endpoint:GET /users") || cgraph::is_bridged_contract("src_db_client_ts") ||
      cgraph::is_bridged_contract("service:api")) {
    return fail("is_bridged_contract");
  }
  // Every service reads NODE_ENV and sends Authorization: an env name crosses
  // only when declared, a standard header never.
  if (cgraph::is_bridged_contract("env:NODE_ENV") || cgraph::is_bridged_contract("env:ML_BACKEND_URL") ||
      cgraph::is_bridged_contract("header:authorization") || cgraph::is_bridged_contract("header:content-type") ||
      cgraph::is_bridged_contract("header:x-forwarded-for") || cgraph::is_bridged_contract("header:x-request-id") ||
      cgraph::is_bridged_contract("header:traceparent") || !cgraph::is_bridged_contract("header:x-act-as-org") ||
      !cgraph::is_bridged_contract("claim:org_id")) {
    return fail("env ids and standard headers never bridge by themselves");
  }
  return 0;
}

// The standard header table is sorted and unique (binary search depends on
// it), lowercased, and holds the IANA permanent names and the de-facto ones.
// A header read provides only where something reaches the reading function
// (a CALLS, imports or references edge, or a route's handled_by), unless the
// framework binds the read itself (kBoundHeaderRead).
int test_unreached_header_reads() {
  auto built = build({
      {"/proj/h/api/admin-auth.ts", "export function applyActAsOverride(request: Request) { return request.headers.get('x-act-as-org'); }\n"},
      {"/proj/h/api/routes.ts", "import { applyActAsOverride } from './admin-auth';\nexport const handle = (r: Request) => applyActAsOverride(r);\n"},
      {"/proj/h/web/proxy.ts", "export function getUserContextFromHeaders(request: Request) { return request.headers.get('x-tenant-id'); }\n"},
      {"/proj/h/api/Token.kt", "class Token { fun token(@RequestHeader(\"X-Bound-Tag\") tag: String): String = tag }\n"},
  });
  auto& graph = built.graph;
  const auto apply = cgraph::make_id("/proj/h/api/admin-auth.ts:applyActAsOverride");
  const auto orphan = cgraph::make_id("/proj/h/web/proxy.ts:getUserContextFromHeaders");
  if (node_of(graph, apply) == nullptr || node_of(graph, orphan) == nullptr) {
    return fail("unreached-read fixture functions missing");
  }
  if (!has_edge(graph, "header:x-act-as-org", apply, "handled_by")) {
    return fail("a header read in an imported function provides the header");
  }
  if (node_of(graph, "header:x-tenant-id") != nullptr || has_edge(graph, "header:x-tenant-id", orphan, "handled_by")) {
    return fail("a header read in a function nothing calls, imports or routes to provided the header");
  }
  const auto* bound = node_of(graph, "header:x-bound-tag");
  if (bound == nullptr || bound->properties.contains("served")) {
    return fail("a framework-bound read (@RequestHeader) in a function nothing calls still provides the header");
  }
  if (built.stats.contract_reads_unreached != 1) {
    return fail("contract_reads_unreached counts the one unreached read: " +
                std::to_string(built.stats.contract_reads_unreached));
  }
  return 0;
}

int test_standard_http_headers() {
  const auto table = cgraph::standard_http_headers();
  for (std::size_t i = 1; i < table.size(); ++i) {
    if (!(table[i - 1] < table[i])) {
      return fail("standard header table is not sorted and unique at " + std::string(table[i]));
    }
  }
  for (const auto name : table) {
    for (const char ch : name) {
      if (ch >= 'A' && ch <= 'Z') {
        return fail("standard header table holds an uppercase name: " + std::string(name));
      }
    }
  }
  for (const auto* name : {"accept", "authorization", "content-type", "cookie", "host", "if-none-match", "user-agent",
                           "www-authenticate", "x-request-id", "x-real-ip", "x-correlation-id", "traceparent",
                           "tracestate", "baggage", "x-forwarded-proto", "x-forwarded-host", "x-requested-with",
                           "x-csrf-token", "x-xss-protection", "permissions-policy", "sentry-trace"}) {
    if (!cgraph::is_standard_http_header(name)) {
      return fail(std::string("a standard header is not in the table: ") + name);
    }
  }
  for (const auto* name : {"x-tenant-id", "x-act-as-org", "x-webapp-env", "x-api-key-id", "x-forwarded", "x-api-key"}) {
    if (cgraph::is_standard_http_header(name)) {
      return fail(std::string("an application header is in the standard table: ") + name);
    }
  }
  return 0;
}

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
  failures += test_unplaced_mounts();
  failures += test_no_routes_no_change();
  failures += test_spring_mappings();
  failures += test_spring_non_handlers();
  failures += test_orm_table_links();
  failures += test_positional_and_relative_wrappers();
  failures += test_positional_wrapper_options();
  failures += test_first_parameter_wrapper_options();
  failures += test_langgraph_sdk_clients();
  failures += test_generic_contract_facts();
  failures += test_unreached_header_reads();
  failures += test_standard_http_headers();
  return failures == 0 ? 0 : 1;
}
