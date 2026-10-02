// Header contract facts (header_contracts.cpp), read through each language's
// extractor: a client sending a header records `uses_contract header:<name>`,
// a server handler reading one records `provides_contract header:<name>`.
// Driven only through the extractors' public entry points, so it builds
// against an engine without header contracts and fails there.
#include "cgraph/configured_extractors.hpp"
#include "cgraph/contracts.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/python_extractor.hpp"

#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

// "<provides|uses>|<source id>|<context>[|bound]" for every header fact; `bound`
// marks a read the framework binds to a request (contracts.hpp kBoundHeaderRead).
std::set<std::string> header_facts(const cgraph::ExtractionResult& result) {
  std::set<std::string> facts;
  for (const auto& relation : result.raw_relations) {
    if ((relation.relation == "provides_contract" || relation.relation == "uses_contract") &&
        relation.context.starts_with("header:")) {
      facts.insert(std::string(relation.relation == "provides_contract" ? "provides" : "uses") + "|" +
                   relation.source_id + "|" + relation.context +
                   (relation.target_label == cgraph::kBoundHeaderRead ? "|bound" : ""));
    }
  }
  return facts;
}

bool expect(std::string_view what, const std::set<std::string>& actual, const std::set<std::string>& expected) {
  if (actual == expected) {
    return true;
  }
  std::cerr << what << ": unexpected header facts\n";
  for (const auto& fact : actual) std::cerr << "  actual:   " << fact << '\n';
  for (const auto& fact : expected) std::cerr << "  expected: " << fact << '\n';
  return false;
}

std::string id(std::string_view file, std::string_view symbol) {
  return cgraph::make_id(std::string(file) + ":" + std::string(symbol));
}

}  // namespace

int main() {
  bool ok = true;

  // TypeScript senders: a `headers` option, an object held in or returned by a
  // `*header*` name, a subscript write, a constant key one hop away, a spread
  // conditional, an argument into a same-file function's `extraHeaders`
  // parameter. Readers: `request.headers.get`, `headers[...]`, `headers.get(CONST)`.
  // Not facts: standard headers, a dashless key, a response's headers, a key
  // of an ordinary object, an interpolated template.
  {
    const std::string file = "src/client.ts";
    const auto result = cgraph::extract_typescript({.source_file = file, .relative_path = file, .source = R"ts(
const BACKEND_HEADER = 'x-ml-backend';
async function mlRequest(base: string, path: string, extraHeaders?: Record<string, string>) {
  return fetch(`${base}${path}`, { headers: { 'Content-Type': 'application/json', ...extraHeaders } });
}
export function mlBackendRequest(path: string) {
  return mlRequest(base, path, { 'X-Webapp-Env': config.webappEnv });
}
export function backendAuthHeaders(c: { actAsOrg?: string }): Record<string, string> {
  const headers: Record<string, string> = { Authorization: `Bearer ${c.token}`, 'X-Request-ID': rid };
  if (c.actAsOrg) {
    headers["X-Act-As-Org"] = c.actAsOrg;
  }
  return headers;
}
export function getAuthHeaders() {
  return { 'X-ML-Backend': pick(), [BACKEND_HEADER]: 'v3', ...(key ? { 'x-api-key': key } : {}) };
}
export async function call(id: string) {
  await axios.post(url, body, { headers: { 'X-Collab-Key': k, Accept: 'x', [`x-${id}`]: 'y' } });
  const plain = { 'X-Not-A-Header': 1 };
  return new Response(body, { headers: { 'X-Served-By': 'api' } });
}
export function pickBackend(headers: Headers) {
  return headers.get(BACKEND_HEADER);
}
export async function applyActAsOverride(request: Request) {
  const org = request.headers.get('x-act-as-org');
  const ua = request.headers.get('user-agent');
  nextResponse.headers.set('x-tenant-id', org);
  return org;
}
export function resolveOrg(headers: Record<string, string>) {
  return headers['x-act-as-org'] ?? null;
}
)ts"});
    ok &= expect("typescript", header_facts(result),
                 {
                     "uses|" + id(file, "mlBackendRequest") + "|header:X-Webapp-Env",
                     "uses|" + id(file, "backendAuthHeaders") + "|header:X-Act-As-Org",
                     "uses|" + id(file, "getAuthHeaders") + "|header:X-ML-Backend",
                     "uses|" + id(file, "getAuthHeaders") + "|header:x-ml-backend",
                     "uses|" + id(file, "getAuthHeaders") + "|header:x-api-key",
                     "uses|" + id(file, "call") + "|header:X-Collab-Key",
                     "provides|" + id(file, "pickBackend") + "|header:x-ml-backend",
                     "provides|" + id(file, "applyActAsOverride") + "|header:x-act-as-org",
                     "provides|" + id(file, "resolveOrg") + "|header:x-act-as-org",
                 });
  }

  // A test file records nothing: its fake server does not provide the header,
  // and its requests go to its own service or to such a fake.
  {
    const std::string file = "src/client.test.ts";
    const auto result = cgraph::extract_typescript({.source_file = file, .relative_path = file, .source = R"ts(
function fakeServer(request: Request) { return request.headers.get('x-act-as-org'); }
function reqWith() { return fetch(u, { headers: { 'X-Act-As-Org': 'org' } }); }
)ts"});
    ok &= expect("typescript test file", header_facts(result), {});
  }

  // Python: FastAPI `Header(alias=CONST)` one hop away, a `Header()`
  // parameter's name read as the header (`_` is `-`), `convert_underscores=False`
  // keeps it; `request.headers.get`; senders `headers={...}`, `headers = {...}`,
  // `headers[...] = v`. Not facts: a response's headers, an ordinary dict, an
  // alias this file cannot read.
  {
    const std::string file = "ops/client.py";
    const auto result = cgraph::extract_python({.source_file = file, .relative_path = file, .source = R"py(
WEBAPP_ENV_HEADER = "X-Webapp-Env"

async def get_session(x_webapp_env: Optional[str] = Header(default=None, alias=WEBAPP_ENV_HEADER)):
    return x_webapp_env

def by_name(x_trace_tag: str = Header(None), raw_name: str = Header(None, convert_underscores=False), authorization: str = Header(None), other: str = Header(alias=IMPORTED)):
    return request.headers.get("x-request-tag")

def send():
    requests.post(url, headers={"X-Dd-Thing": key, "Content-Type": "application/json"})
    headers = {"X-Api-Key": key}
    headers["X-Extra-Key"] = other
    plain = {"X-Plain-Key": 1}
    return JSONResponse(content={}, headers={"X-Served-By": "ml"})
)py"});
    ok &= expect("python", header_facts(result),
                 {
                     "provides|" + id(file, "get_session") + "|header:X-Webapp-Env|bound",
                     "provides|" + id(file, "by_name") + "|header:x-trace-tag|bound",
                     "provides|" + id(file, "by_name") + "|header:x-request-tag",
                     "uses|" + id(file, "send") + "|header:X-Dd-Thing",
                     "uses|" + id(file, "send") + "|header:X-Api-Key",
                     "uses|" + id(file, "send") + "|header:X-Extra-Key",
                 });
  }

  // Kotlin: Spring `@RequestHeader("X")`, `(name = CONST)`, `request.getHeader`,
  // Ktor `call.request.header(CONST)` with a companion constant; senders Ktor
  // `header("X", v)`, `headers { append("X", v) }`, OkHttp `.addHeader`. Not
  // facts: a ResponseEntity's `.header`, `response.addHeader`, standard names.
  {
    const std::string file = "idp/TokenController.kt";
    const auto result = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Kotlin, {.source_file = file, .relative_path = file, .source = R"kt(
class TokenController {
    fun token(
        @RequestHeader("Authorization", required = false) authorization: String?,
        @RequestHeader("X-Tenant-ID", required = false) tenantId: String?,
        @RequestHeader(name = PAIRING_HEADER) pairing: String,
    ): ResponseEntity<String> {
        val id = request.getHeader("X-Client-Tag")
        return ResponseEntity.status(400).header("X-Error-Reason", "denied").build()
    }

    fun broker() {
        val pairing = call.request.header(PAIRING_HEADER)
        response.addHeader("X-Served-By", "idp")
    }

    suspend fun send() {
        client.post(url) {
            header("X-Tenant-ID", tenant)
            headers { append("X-Device-Id", device) }
        }
        val request = Request.Builder().url(url).addHeader("X-Trace-Tag", tag).build()
    }

    companion object {
        const val PAIRING_HEADER = "X-Passless-Pairing"
    }
}
)kt"});
    ok &= expect("kotlin", result ? header_facts(*result) : std::set<std::string>{},
                 {
                     "provides|" + id(file, "token") + "|header:X-Tenant-ID|bound",
                     "provides|" + id(file, "token") + "|header:X-Passless-Pairing|bound",
                     "provides|" + id(file, "token") + "|header:X-Client-Tag",
                     "provides|" + id(file, "broker") + "|header:X-Passless-Pairing|bound",
                     "uses|" + id(file, "send") + "|header:X-Tenant-ID",
                     "uses|" + id(file, "send") + "|header:X-Device-Id",
                     "uses|" + id(file, "send") + "|header:X-Trace-Tag",
                 });
  }

  // Go: a `map[string]string` returned by a `*Header*` method, `req.Header.Set`,
  // `http.Header{...}`, a map passed to a `*Headers` function; readers
  // `r.Header.Get(CONST)` and gin's `c.GetHeader`. Not facts: the
  // ResponseWriter's `w.Header().Set`, a response's `resp.Header.Get`, an
  // ordinary map.
  {
    const std::string file = "internal/api/client.go";
    const auto result = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Go, {.source_file = file, .relative_path = file, .source = R"go(
package api

const tenantHeaderName = "X-Tenant-ID"

func (c *Client) tenantHeader() map[string]string {
	return map[string]string{"X-Tenant-ID": c.TenantID}
}

func (c *Client) send(req *http.Request) {
	req.Header.Set("X-Trace-Tag", c.Tag)
	req.Header.Set("Accept", "application/json")
	h := http.Header{"X-Device-Id": {c.Device}}
	c.doWithHeaders(ctx, map[string]string{"X-Extra-Key": "v"})
	labels := map[string]string{"X-Not-A-Header": "v"}
	_ = resp.Header.Get("X-Rate-Remaining")
}

func handle(w http.ResponseWriter, r *http.Request) {
	tenant := r.Header.Get(tenantHeaderName)
	w.Header().Set("X-Served-By", "cli")
}

func ginHandle(c *gin.Context) {
	_ = c.GetHeader("X-Gin-Tag")
}
)go"});
    ok &= expect("go", result ? header_facts(*result) : std::set<std::string>{},
                 {
                     "uses|" + id(file, "tenantHeader") + "|header:X-Tenant-ID",
                     "uses|" + id(file, "send") + "|header:X-Trace-Tag",
                     "uses|" + id(file, "send") + "|header:X-Device-Id",
                     "uses|" + id(file, "send") + "|header:X-Extra-Key",
                     "provides|" + id(file, "handle") + "|header:X-Tenant-ID|bound",
                     "provides|" + id(file, "ginHandle") + "|header:X-Gin-Tag|bound",
                 });
  }

  // TypeScript, every other branch: Express `req.get`, Hono `c.req.header`,
  // `.has`, next/headers `headers()`, `new Headers({...})`, `headers.set` /
  // `.append`, `opts.headers = {...}`, an arrow returning headers. Not facts: a
  // response named `agentRes`, Elysia's `set.headers` (the response), a module
  // constant shadowed by a parameter or a local, a `tableHeaders` of column
  // titles, `headerStyles`.
  {
    const std::string file = "src/branches.ts";
    const auto result = cgraph::extract_typescript({.source_file = file, .relative_path = file, .source = R"ts(
const TAG = 'x-module-tag';
export function viaReq(req: Request) { return req.get('x-express-tag'); }
export function viaHono(c: Ctx) { return c.req.header('x-hono-tag'); }
export function viaHas(request: Request) { return request.headers.has(TAG); }
export async function nextRead() { return (await headers()).get('x-next-tag'); }
export function sendSet() {
  const h = new Headers({ 'X-Ctor-Tag': 'a' });
  h.set('X-Not-Headers-Holder', 'b');
  const headers = new Headers();
  headers.set('X-Set-Tag', v);
  headers.append('X-Append-Tag', w);
  return fetch(u, { headers });
}
export function assign(opts: any) { opts.headers = { 'X-Assigned-Tag': 'v' }; }
export const arrowHeaders = () => ({ 'X-Arrow-Tag': 'v' });
export async function agentProxy() { const agentRes = await fetch(u); return agentRes.headers.get('x-agent-tag'); }
export const elysia = ({ set }: any) => { set.headers['x-elysia-tag'] = 'v'; };
export function shadowParam(TAG: string) { return request.headers.get(TAG); }
export function shadowLocal() { const TAG = other(); return request.headers.get(TAG); }
export function noise() {
  const tableHeaders = { 'Created At': 1, name: 2 };
  const headerStyles = { 'font-size': 1 };
  return [tableHeaders, headerStyles];
}
)ts"});
    ok &= expect("typescript branches", header_facts(result),
                 {
                     "provides|" + id(file, "viaReq") + "|header:x-express-tag",
                     "provides|" + id(file, "viaHono") + "|header:x-hono-tag",
                     "provides|" + id(file, "viaHas") + "|header:x-module-tag",
                     "provides|" + id(file, "nextRead") + "|header:x-next-tag",
                     "uses|" + id(file, "sendSet") + "|header:X-Ctor-Tag",
                     "uses|" + id(file, "sendSet") + "|header:X-Set-Tag",
                     "uses|" + id(file, "sendSet") + "|header:X-Append-Tag",
                     "uses|" + id(file, "assign") + "|header:X-Assigned-Tag",
                     "uses|" + id(file, "arrowHeaders") + "|header:X-Arrow-Tag",
                 });
  }

  // Python, every other branch: a `X.headers["x"]` read, a dict returned by a
  // `*_headers` function. Not facts: a response named `agent_res`, a module
  // constant a local assignment shadows.
  {
    const std::string file = "ops/branches.py";
    const auto result = cgraph::extract_python({.source_file = file, .relative_path = file, .source = R"py(
TAG = "X-Module-Tag"

def read_sub(request):
    return request.headers["x-sub-tag"]

def shadowed(request):
    TAG = compute()
    return request.headers.get(TAG)

def auth_headers():
    return {"X-Returned-Tag": "v"}

def agent(agent_res):
    return agent_res.headers.get("x-agent-tag")
)py"});
    ok &= expect("python branches", header_facts(result),
                 {
                     "provides|" + id(file, "read_sub") + "|header:x-sub-tag",
                     "uses|" + id(file, "auth_headers") + "|header:X-Returned-Tag",
                 });
  }

  // Kotlin, every other branch: `@RequestHeader(value = ...)`, OkHttp
  // `.setHeader`, a WebClient `.uri(responseUrl).header(...)` (an argument
  // naming a response does not make the chain a response).
  {
    const std::string file = "idp/Client.kt";
    const auto result = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Kotlin, {.source_file = file, .relative_path = file, .source = R"kt(
class Client {
    fun read(@RequestHeader(value = "X-Value-Tag") tag: String) = tag

    fun build() {
        val req = Request.Builder().setHeader("X-Set-Header-Tag", v).build()
        webClient.post().uri(responseUrl).header("X-Uri-Tag", v).retrieve()
    }
}
)kt"});
    ok &= expect("kotlin branches", result ? header_facts(*result) : std::set<std::string>{},
                 {
                     "provides|" + id(file, "read") + "|header:X-Value-Tag|bound",
                     "uses|" + id(file, "build") + "|header:X-Set-Header-Tag",
                     "uses|" + id(file, "build") + "|header:X-Uri-Tag",
                 });
  }

  // Go, every other branch: `.Header.Add`, a `Header:` keyed element, a map
  // held in a `*headers` variable, a `var` constant, `.Header.Values` outside
  // a net/http handler (not bound: something must call it).
  {
    const std::string file = "internal/api/branches.go";
    const auto result = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Go, {.source_file = file, .relative_path = file, .source = R"go(
package api

var tagHeaderName = "X-Var-Tag"

func (c *Client) send2(req *http.Request) {
	req.Header.Add("X-Add-Tag", "v")
	r := &Request{Header: map[string]string{"X-Keyed-Tag": "v"}}
	extraHeaders := map[string]string{"X-Held-Tag": "v"}
	_, _ = r, extraHeaders
}

func values(r *http.Request) []string {
	return r.Header.Values(tagHeaderName)
}
)go"});
    ok &= expect("go branches", result ? header_facts(*result) : std::set<std::string>{},
                 {
                     "uses|" + id(file, "send2") + "|header:X-Add-Tag",
                     "uses|" + id(file, "send2") + "|header:X-Keyed-Tag",
                     "uses|" + id(file, "send2") + "|header:X-Held-Tag",
                     "provides|" + id(file, "values") + "|header:X-Var-Tag",
                 });
  }

  // Test sources record nothing; a class that only ends in `test` letters is
  // not one.
  {
    const std::string kotlin = R"kt(
class C { fun token(@RequestHeader("X-Tenant-ID") t: String) = t }
)kt";
    const auto latest = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Kotlin,
        {.source_file = "idp/LatestController.kt", .relative_path = "idp/LatestController.kt", .source = kotlin});
    ok &= expect("Latest.kt is not a test", latest ? header_facts(*latest) : std::set<std::string>{},
                 {"provides|" + id("idp/LatestController.kt", "token") + "|header:X-Tenant-ID|bound"});
    for (const std::string path : {"idp/TokenControllerTests.kt", "idp/TokenControllerTest.kt"}) {
      const auto result = cgraph::extract_configured_language(
          cgraph::DetectedLanguage::Kotlin, {.source_file = path, .relative_path = path, .source = kotlin});
      ok &= expect(path, result ? header_facts(*result) : std::set<std::string>{}, {});
    }
    const std::string ts = "export function f(request: Request) { return request.headers.get('x-act-as-org'); }\n";
    for (const std::string path : {"src/__mocks__/api.ts", "src/mocks/handlers.ts", "scripts/mock-server.ts"}) {
      ok &= expect(path, header_facts(cgraph::extract_typescript({.source_file = path, .relative_path = path, .source = ts})),
                   {});
    }
    const std::string py = "def f(request):\n    return request.headers.get('x-act-as-org')\n";
    ok &= expect("conftest.py",
                 header_facts(cgraph::extract_python(
                     {.source_file = "tests_x/conftest.py", .relative_path = "tests_x/conftest.py", .source = py})),
                 {});
    const std::string go = "package x\nfunc h(w http.ResponseWriter, r *http.Request) { _ = r.Header.Get(\"X-Tenant-ID\") }\n";
    const auto testutil = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Go,
        {.source_file = "internal/testutil/server.go", .relative_path = "internal/testutil/server.go", .source = go});
    ok &= expect("testutil/", testutil ? header_facts(*testutil) : std::set<std::string>{}, {});
  }

  // The facts a TypeScript sender and a Kotlin reader spell differently are
  // one contract id: header names are case-insensitive.
  if (cgraph::contract_id("header", "X-Tenant-ID") != cgraph::contract_id("header", "x-tenant-id")) {
    std::cerr << "header contract ids are not case-insensitive\n";
    ok = false;
  }

  return ok ? 0 : 1;
}
