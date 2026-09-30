// HTTP consumer facts (http_consumers.cpp), read through the TypeScript
// extractor's walk: http_call / http_wrapper / http_call_args / url_const.
#include "cgraph/http_consumers.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/normalize.hpp"

#include <iostream>
#include <set>
#include <string>
#include <string_view>

int main() {
  // HTTP consumer facts (CGR-13 slice 2): a wrapper records the prefix its own
  // client call appends its first parameter to; calls record the client, the
  // method when literal, and the path with `{}` for interpolated segments; a
  // URL in a variable records an empty path; a handler argument is a route.
  {
    const auto calls = cgraph::extract_typescript({.source_file = "lib/api.ts", .relative_path = "lib/api.ts", .source = R"ts(
const API_URL = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080';
const base = `${API_URL}/api/v1`;
async function apiFetch<T>(path: string, options?: RequestInit): Promise<T> {
  const res = await fetch(`${base}${path}`, { ...options });
  return res.json();
}
export async function del(path: string) { return fetch(`${API_URL}${path}`, { method: 'DELETE' }); }
export async function put(path: string) { return fetch(`${IMPORTED_BASE}${path}`, { method: 'PUT' }); }
export const notebooksApi = { get: (id: string) => apiFetch<Notebook>(`/notebooks/${id}?expand=1`) };
export async function publish(projectId: string) {
  await fetch(`${API_URL}/api/v1/projects/${projectId}/publish`, { method: 'POST' });
  const { data } = await api.GET('/api/v1/projects/{id}', { params: { path: { id: projectId } } });
  const url = build();
  await fetch(url);
  await axios.post(API_URL + '/api/v1/events', {});
  http.get('/mocked', () => new Response());
  cache.get('/api/v1/key');
  return data;
}
)ts"});
    std::set<std::string> facts;
    for (const auto& relation : calls.raw_relations) {
      if (relation.relation == "http_call" || relation.relation == "http_wrapper" || relation.relation == "url_const") {
        facts.insert(relation.relation + "|" + relation.source_id + "|" + relation.target_label + "|" + relation.context);
      }
    }
    const auto fn = [](std::string_view name) { return cgraph::make_id(std::string("lib/api.ts:") + std::string(name)); };
    const auto file = cgraph::make_id("lib/api.ts");
    const std::set<std::string> expected{
        // URL constants: the env host holds no path; `base` holds `/api/v1`.
        "url_const|" + file + "|API_URL|",
        "url_const|" + file + "|base|/api/v1",
        "http_wrapper|" + fn("apiFetch") + "|fetch| /api/v1",
        "http_wrapper|" + fn("del") + "|fetch|DELETE ",
        // An imported base stays a placeholder for project-wide resolution.
        "http_wrapper|" + fn("put") + "|fetch|PUT ${IMPORTED_BASE}",
        "http_call|" + fn("notebooksApi") + "|apiFetch| /notebooks/{}",
        "http_call|" + fn("publish") + "|fetch|POST /api/v1/projects/{}/publish",
        "http_call|" + fn("publish") + "|api.GET| /api/v1/projects/{id}",
        "http_call|" + fn("publish") + "|fetch| ",
        "http_call|" + fn("publish") + "|axios.post| /api/v1/events",
    };
    if (facts != expected) {
      for (const auto& fact : facts) std::cerr << "consumer fact: " << fact << '\n';
      return 1;
    }
  }

  // A typed awaited request is still a request: tree-sitter-typescript parses
  // `await axios.post<T>(url)` as `(await axios.post)<T>(url)`. A call at the
  // front of a URL that takes a runtime value is a builder: one this file
  // defines is read (its parameters fill segments), one it imports may hold a
  // path this file cannot see, so the request is left unresolved instead of
  // minting a truncated route (`/oracles`); a member or a host getter there is
  // the host.
  {
    const auto calls = cgraph::extract_typescript({.source_file = "lib/extra.ts", .relative_path = "lib/extra.ts", .source = R"ts(
const BACKEND_URL = process.env.BACKEND_URL || 'http://localhost:8080';
const base = (projectId: string) => `${BACKEND_URL}/api/v1/projects/${projectId}`;
export async function typedAwait() {
  let r;
  r = await axios.post<{ ok: boolean }>(`${BACKEND_URL}/api/v1/assigned-generic`, {});
  const s = await axios.post<LoginResponse>(`${BACKEND_URL}/api/v1/login`, {});
  return [r, s];
}
export async function viaHelper(projectId: string) {
  return fetch(`${base(projectId)}/oracles`, { method: 'POST' });
}
export async function viaImportedHelper(projectId: string) {
  return fetch(`${importedBase(projectId)}/runs`, { method: 'POST' });
}
export async function viaMember() {
  return fetch(`${config.baseUrl}/items`);
}
export async function viaGetter() {
  await fetch(`${getAgentsApiUrl()}/runs/wait`, { method: 'POST' });
  await fetch(`${process.env.NEXT_PUBLIC_API_URL?.replace(/\/$/, '')}/api/v1/users`);
  await fetch(`${config.get(`apiUrl`)}/api/v1/items`);
  return fetch(`${config.get('apiUrl')}/api/v1/orders`);
}
)ts"});
    std::set<std::string> facts;
    for (const auto& relation : calls.raw_relations) {
      if (relation.relation == "http_call" || relation.relation == "http_wrapper" || relation.relation == "url_const") {
        facts.insert(relation.relation + "|" + relation.source_id + "|" + relation.target_label + "|" + relation.context);
      }
    }
    const auto fn = [](std::string_view name) { return cgraph::make_id(std::string("lib/extra.ts:") + std::string(name)); };
    const auto file = cgraph::make_id("lib/extra.ts");
    const std::set<std::string> expected{
        "url_const|" + file + "|BACKEND_URL|",
        "http_call|" + fn("typedAwait") + "|axios.post| /api/v1/assigned-generic",
        "http_call|" + fn("typedAwait") + "|axios.post| /api/v1/login",
        "http_call|" + fn("viaHelper") + "|fetch|POST /api/v1/projects/{}/oracles",
        "http_call|" + fn("viaImportedHelper") + "|fetch|POST ",
        "http_call|" + fn("viaMember") + "|fetch| /items",
        // A host getter (no arguments, only literals, or off process.env) is a host.
        "http_call|" + fn("viaGetter") + "|fetch|POST /runs/wait",
        "http_call|" + fn("viaGetter") + "|fetch| /api/v1/users",
        "http_call|" + fn("viaGetter") + "|fetch| /api/v1/orders",
        "http_call|" + fn("viaGetter") + "|fetch| /api/v1/items",
    };
    if (facts != expected) {
      for (const auto& fact : facts) std::cerr << "typed/opaque consumer fact: " << fact << '\n';
      return 1;
    }
  }

  // URL shapes read through this file (p2-ts-url-resolution): a URL held in a
  // local (`const`, `new URL(...)` + `.toString()`, a `let` assigned per
  // branch), a same-file query helper, a class field base, an axios instance's
  // baseURL, a class-method wrapper called as `this.patch(...)`, a module
  // wrapper that strips the path's leading slash, and a wrapper chain whose
  // path and method are later parameters with a base supplied by the outer one.
  {
    const auto calls = cgraph::extract_typescript({.source_file = "lib/shapes.ts", .relative_path = "lib/shapes.ts", .source = R"ts(
const BACKEND_URL = process.env.BACKEND_URL || 'http://localhost:8080';
const API_BASE = '/api/backend';
const mlBackendBaseUrl = config.ml.mlBackendUrl;
function buildQuery(params: Q): string {
  const parts: string[] = [];
  return parts.length > 0 ? `?${parts.join('&')}` : '';
}
export async function viaLocals(token: string, tenantId: string | null) {
  const backendUrl = `${BACKEND_URL}/api/v1/invitations/${token}/mfa/verify`;
  await fetch(backendUrl, { method: 'POST' });
  const catalog = new URL(`${BACKEND_URL}/api/v1/skills/catalog`);
  catalog.searchParams.set('a', 'b');
  await fetch(catalog.toString());
  let eventsUrl: URL;
  if (tenantId) {
    eventsUrl = new URL(`${BACKEND_URL}/api/v1/auth-events/recent/realm/${tenantId}`);
  } else {
    eventsUrl = new URL(`${BACKEND_URL}/api/v1/auth-events/recent`);
  }
  await fetch(eventsUrl.toString());
  const moved = new URL(`${BACKEND_URL}/api/v1/moved`);
  moved.pathname = '/elsewhere';
  await fetch(moved.href);
  const scoreQuery = buildQuery({ scenario_id: tenantId });
  return fetch(`${BACKEND_URL}/project/${token}/formulations/score${scoreQuery}`);
}
async function apiRequest<T>(endpoint: string, options: RequestInit = {}): Promise<T> {
  const url = `${API_BASE}/${endpoint.replace(/^\//, '')}`;
  const response = await fetch(url, { ...options, credentials: 'include' });
  return response.json();
}
export async function refreshMetadata(id: string) {
  return apiRequest(`v1/service-providers/${id}/refresh-metadata`, { method: 'POST' });
}
class ApiService {
  private api: AxiosInstance;
  private readonly PASSKEYS = '/api/backend';
  constructor() {
    this.api = axios.create({ baseURL: '/api/backend', timeout: 10000 });
  }
  private async patch<T>(url: string, data?: any): Promise<T> {
    const response = await this.api.patch(url, data);
    return response.data;
  }
  async enableUser(id: string): Promise<User> {
    return this.patch<User>(`v1/users/${id}/enable`);
  }
  async themeCss(tenantId: string) {
    return this.api.get(`v1/public/tenants/${tenantId}/theme/css`, { headers: {} });
  }
  async startRegistration(tenantId: string) {
    return await axios.post<any>(`${this.PASSKEYS}/v1/passkeys/register/start`, {}, {});
  }
}
async function mlRequest<T>(baseUrl: string, method: 'GET' | 'POST', path: string, projectId?: string): Promise<T> {
  const url = `${baseUrl}${path}`;
  const response = await fetch(url, { method, headers: {} });
  return response.json();
}
export function mlBackendRequest<T>(method: 'GET' | 'POST', path: string, projectId?: string): Promise<T> {
  return mlRequest<T>(mlBackendBaseUrl, method, path, projectId);
}
export async function setup(projectId: string) {
  return mlBackendRequest('POST', `/project/${projectId}/setup`, projectId);
}
)ts"});
    std::set<std::string> facts;
    for (const auto& relation : calls.raw_relations) {
      if (relation.relation.starts_with("http_")) {
        facts.insert(relation.relation + "|" + relation.source_id + "|" + relation.target_label + "|" + relation.context);
      }
    }
    const auto fn = [](std::string_view name) { return cgraph::make_id(std::string("lib/shapes.ts:") + std::string(name)); };
    const std::set<std::string> expected{
        "http_call|" + fn("viaLocals") + "|fetch|POST /api/v1/invitations/{}/mfa/verify",
        "http_call|" + fn("viaLocals") + "|fetch| /api/v1/skills/catalog",
        // One consumer per branch the `let` is assigned in.
        "http_call|" + fn("viaLocals") + "|fetch| /api/v1/auth-events/recent/realm/{}",
        "http_call|" + fn("viaLocals") + "|fetch| /api/v1/auth-events/recent",
        // A URL whose path is rewritten after it is built is not guessed.
        "http_call|" + fn("viaLocals") + "|fetch| ",
        // A same-file helper returning `''` or `?...` is the query string.
        "http_call|" + fn("viaLocals") + "|fetch| /project/{}/formulations/score",
        // The stripped leading slash makes the parameter the tail after `/`.
        "http_wrapper|" + fn("apiRequest") + "|fetch| /api/backend/",
        "http_call|" + fn("refreshMetadata") + "|apiRequest|POST v1/service-providers/{}/refresh-metadata",
        // Its arguments too, in case `apiRequest` takes the path elsewhere.
        "http_call_args|" + fn("refreshMetadata") + "|apiRequest|Pv1/service-providers/{}/refresh-metadata\tOPOST",
        // The axios instance's baseURL prefixes its requests; the verb is the wrapper's method.
        "http_wrapper|" + fn("patch") + "|api.patch|PATCH /api/backend/",
        "http_call|" + fn("enableUser") + "|this.patch|PATCH /api/backend/v1/users/{}/enable",
        "http_call|" + fn("themeCss") + "|api.get| /api/backend/v1/public/tenants/{}/theme/css",
        "http_call|" + fn("startRegistration") + "|axios.post| /api/backend/v1/passkeys/register/start",
        // mlRequest's base is a parameter, filled only by mlBackendRequest's
        // forwarding call: mlBackendRequest takes the method at 0, the path at 1.
        "http_wrapper|" + fn("mlBackendRequest") + "|mlRequest|@0  #1",
        "http_call_args|" + fn("setup") + "|mlBackendRequest|VPOST\tP/project/{}/setup\t",
    };
    if (facts != expected) {
      for (const auto& fact : facts) std::cerr << "url shape fact: " << fact << '\n';
      return 1;
    }
  }

  // Locals that are not a readable path keep their old reading: a literal
  // origin or a call standing at the host is the host, a local inside the path
  // is a value filling a segment even when a callback assigns it a literal, and
  // a destructured parameter shadows an outer local of the same name. A method
  // parameter's default verb is recorded for calls that leave it out.
  {
    const auto calls = cgraph::extract_typescript({.source_file = "lib/locals.ts", .relative_path = "lib/locals.ts", .source = R"ts(
const API_BASE_URL = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080';
describe('users', () => {
  const backendUrl = 'http://localhost:8080';
  let sessionId: string;
  beforeAll(async () => {
    sessionId = 'test-session-id';
  });
  test('lists', async ({ request }) => {
    await request.get(`${backendUrl}/api/v1/users?tenantId=master`);
    await axios.patch(`${API_BASE_URL}/api/v1/sessions/${sessionId}/extend`, {});
  });
});
export async function health() {
  const apiUrl = (process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080').replace(/\/$/, '');
  return fetch(`${apiUrl}/api/v1/admin/health`);
}
export function seed(opts: Opts) {
  const apiUrl = resolveApiUrl(opts.apiUrl);
  return run(async ({ id, apiUrl }) => {
    await fetch(`${apiUrl}/api/v1/ic-sessions/${id}`, { method: 'PATCH' });
  });
}
async function presenceFetch(path: string, method = 'GET') {
  const res = await fetch(`${API_BASE_URL}/api/v1/notes${path}`, { method });
  return res.json();
}
export async function poll(noteId: string) {
  return presenceFetch(`/${noteId}/presence`);
}
function getAgentsApiUrl(): string {
  const raw = process.env.AGENTS_API_URL;
  return (raw || 'http://localhost:8000').replace(/\/$/, '');
}
export async function classify() {
  return fetch(`${getAgentsApiUrl()}/runs/wait`, { method: 'POST' });
}
export async function toggle(on: boolean) {
  const url = `${API_BASE_URL}/api/v1/me/library/favorites`;
  return fetch(url, { method: on ? 'POST' : 'DELETE' });
}
class RolesApi {
  private baseUrl = API_ENDPOINTS.roles || '/api/roles';
  async hierarchy(tenantId?: string) {
    const queryParams = tenantId ? `?tenantId=${tenantId}` : '';
    return fetch(`${this.baseUrl}/hierarchy${queryParams}`, { method: 'GET' });
  }
}
class AnalyticsApi {
  private baseUrl = '/api/analytics';
  async exportData(period: string) {
    return fetch(`${this.baseUrl}/export?period=${period}`, { method: 'POST' });
  }
}
)ts"});
    std::set<std::string> facts;
    for (const auto& relation : calls.raw_relations) {
      if (relation.relation.starts_with("http_")) {
        facts.insert(relation.relation + "|" + relation.source_id + "|" + relation.target_label + "|" + relation.context);
      }
    }
    const auto fn = [](std::string_view name) { return cgraph::make_id(std::string("lib/locals.ts:") + std::string(name)); };
    const auto file = cgraph::make_id("lib/locals.ts");
    const std::set<std::string> expected{
        "http_call|" + file + "|request.get| /api/v1/users",
        "http_call|" + file + "|axios.patch| /api/v1/sessions/{}/extend",
        "http_call|" + fn("health") + "|fetch| /api/v1/admin/health",
        "http_call|" + file + "|fetch|PATCH ${apiUrl}/api/v1/ic-sessions/{}",
        "http_wrapper|" + fn("presenceFetch") + "|fetch|@1=GET /api/v1/notes",
        "http_call|" + fn("poll") + "|presenceFetch| /{}/presence",
        "http_call_args|" + fn("poll") + "|presenceFetch|P/{}/presence",
        // A host getter this file defines is still the host.
        "http_call|" + fn("classify") + "|fetch|POST /runs/wait",
        // A method chosen between two verbs sends each.
        "http_call|" + fn("toggle") + "|fetch|POST /api/v1/me/library/favorites",
        "http_call|" + fn("toggle") + "|fetch|DELETE /api/v1/me/library/favorites",
        // A field that is some path we cannot read is not a host to drop
        // (no truncated `/hierarchy`); a literal field is the prefix.
        "http_call|" + fn("hierarchy") + "|fetch|GET ",
        "http_call|" + fn("exportData") + "|fetch|POST /api/analytics/export",
    };
    if (facts != expected) {
      for (const auto& fact : facts) std::cerr << "locals fact: " << fact << '\n';
      return 1;
    }
  }

  return 0;
}
