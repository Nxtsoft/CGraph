#pragma once

// HTTP header contracts (Phase 3.2): `provides_contract` / `uses_contract`
// facts with context `header:<name>` (contracts.hpp). A client that SENDS a
// header uses it; a server handler that READS a header provides it. The name
// is recorded as the code spells it; contract_id lower-cases it, so
// `X-Tenant-ID` sent by one repo and `x-tenant-id` read by another are one
// contract.
//
// Only a header-name-shaped literal in a header context counts: letters,
// digits and dashes with at least one dash (`X-Act-As-Org`), as a key of an
// object/dict/map that is a request's headers, the name argument of a header
// setter, or the name a server reads. Arbitrary dict keys are never read. A
// holder is a request's headers when it is the value of a `headers` option or
// keyword, or is named exactly `headers` / `header` or with a `Headers` /
// `Header` camelCase suffix (`authHeaders`, `getAuthHeaders`, `tenantHeader`)
// or a `_headers` / `_header` suffix; a name merely containing the word
// (`headerStyles`) is not. A name may be a constant one hop away
// (`headers.get(BACKEND_HEADER)` with `const BACKEND_HEADER = 'x-ml-backend'`
// in the same file) unless a local or parameter shadows it; imported
// constants are not followed.
//
// A read the framework binds to a request itself is marked kBoundHeaderRead
// (contracts.hpp): Spring `@RequestHeader`, FastAPI `Header()`, Ktor
// `call.request.header` when `call` is the route's implicit ApplicationCall
// or a parameter typed so, gin `c.GetHeader` when `c` is a `*gin.Context`
// parameter (GetHeader on anything else records nothing), and `r.Header.Get`
// inside a `func(http.ResponseWriter, *http.Request)`. Any other read provides only
// when resolve_contracts finds something reaching its function. Standard HTTP headers (is_standard_http_header:
// `authorization`, `content-type`, `x-request-id`, `x-forwarded-*`) are never
// recorded: they never bridge repositories and every handler reads them.
//
// Each walk is called by its language's extra_walk on every node, with the
// innermost enclosing function as `function_scope_id`. A sender outside any
// function is attributed to the module-level variable whose initializer holds
// it when the JavaScript extractor made a node for it
// (js_syntax::reading_scope_id), else to its file; a reader outside any function is not
// recorded (no handler reads it). A test source (data_contracts.hpp
// is_test_source_path) records nothing: its handlers are fakes standing in for
// another service and its requests go to its own service under test.

#include "cgraph/language_config.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// Held while one JavaScript/TypeScript file is extracted: indexes the file's
// module constants and function parameters once instead of per object
// literal. Scopes nest; each covers one file on its thread. Without one the
// walk still works, re-reading the module each time.
class HeaderContractsFileScope {
 public:
  HeaderContractsFileScope();
  ~HeaderContractsFileScope();
  HeaderContractsFileScope(const HeaderContractsFileScope&) = delete;
  HeaderContractsFileScope& operator=(const HeaderContractsFileScope&) = delete;

 private:
  struct Index;
  std::unique_ptr<Index> index_;
};

// True for a name a header contract is recorded under: token characters
// (letters, digits, `-`), at least one dash, not a standard header.
[[nodiscard]] bool is_contract_header_name(std::string_view name);

// JavaScript / TypeScript: `fetch(u, { headers: { 'X-A': v } })`, an object
// held in or returned by something named `*header*`, `headers['X-A'] = v`,
// `headers.set('X-A', v)`, `new Headers({...})` (senders);
// `request.headers.get('x-a')`, `req.headers['x-a']`, `headers.get(CONST)`,
// `c.req.header('x-a')` (readers). Response headers are not requests.
void js_header_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                         const Fragment& fragment, std::vector<RawRelation>& out);

// Python: `headers={"X-A": v}`, `headers = {...}`, `headers["X-A"] = v`
// (senders); FastAPI `Header(alias=...)` parameters, a `Header()` parameter's
// name with `_` read as `-` (FastAPI's convert_underscores), and
// `request.headers.get("x-a")` / `request.headers["x-a"]` (readers).
void python_header_contracts(const TSNode& node, const ExtractionContext& context,
                             const std::string& function_scope_id, std::vector<RawRelation>& out);

// Kotlin: Ktor `header("X-A", v)`, `headers { append("X-A", v) }`, builder
// `.header` / `.addHeader` / `.setHeader` off a response (senders); Spring
// `@RequestHeader("X-A")` / `(name = ...)` / `(value = ...)`,
// `request.getHeader("X-A")`, Ktor `call.request.header("X-A")` (readers).
void kotlin_header_contracts(const TSNode& node, const ExtractionContext& context,
                             const std::string& function_scope_id, std::vector<RawRelation>& out);

// Go: `req.Header.Set("X-A", v)` / `.Add`, `http.Header{"X-A": ...}`, and a
// `map[string]string{"X-A": v}` returned by a `*header*` function, held in a
// `*header*` variable or field, or passed to a `*header*` function (senders);
// `r.Header.Get("X-A")`, gin's `c.GetHeader("X-A")` (readers).
void go_header_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                         std::vector<RawRelation>& out);

}  // namespace cgraph
