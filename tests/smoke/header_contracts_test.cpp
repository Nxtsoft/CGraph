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

// "<provides|uses>|<source id>|<context>" for every header fact.
std::set<std::string> header_facts(const cgraph::ExtractionResult& result) {
  std::set<std::string> facts;
  for (const auto& relation : result.raw_relations) {
    if ((relation.relation == "provides_contract" || relation.relation == "uses_contract") &&
        relation.context.starts_with("header:")) {
      facts.insert(std::string(relation.relation == "provides_contract" ? "provides" : "uses") + "|" +
                   relation.source_id + "|" + relation.context);
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
                     "provides|" + id(file, "get_session") + "|header:X-Webapp-Env",
                     "provides|" + id(file, "by_name") + "|header:x-trace-tag",
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
                     "provides|" + id(file, "token") + "|header:X-Tenant-ID",
                     "provides|" + id(file, "token") + "|header:X-Passless-Pairing",
                     "provides|" + id(file, "token") + "|header:X-Client-Tag",
                     "provides|" + id(file, "broker") + "|header:X-Passless-Pairing",
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
                     "provides|" + id(file, "handle") + "|header:X-Tenant-ID",
                     "provides|" + id(file, "ginHandle") + "|header:X-Gin-Tag",
                 });
  }

  // The facts a TypeScript sender and a Kotlin reader spell differently are
  // one contract id: header names are case-insensitive.
  if (cgraph::contract_id("header", "X-Tenant-ID") != cgraph::contract_id("header", "x-tenant-id")) {
    std::cerr << "header contract ids are not case-insensitive\n";
    ok = false;
  }

  return ok ? 0 : 1;
}
