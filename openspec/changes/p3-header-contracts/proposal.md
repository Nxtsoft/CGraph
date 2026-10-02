# HTTP headers a client sends and a server reads join repositories (Phase 3.2)

## Why

The foundation (#158) mints `header:<lowercased name>` contract nodes from `provides_contract` / `uses_contract` facts and bridges every non-standard header across repositories, but no extractor emitted header facts. The probe ground truth has four header edges: T38 (`X-Webapp-Env`, turing-api to ml-backend), T39 (`X-ML-Backend`, turing-webapp to turing-api), T41 (`X-Act-As-Org`, turing-agents to turing-api) and M33 (`X-Tenant-ID`, passless-cli to idp).

## What Changes

- New `header_contracts.cpp` (+ header): one walk per language, called from each extractor's `extra_walk` with a one-line hook (JavaScript/TypeScript, Python, Kotlin, Go). A client SENDING a header records `uses_contract header:<name>` from its enclosing function (its file when at module level); a server READING one records `provides_contract header:<name>` from the reading function.
- Senders: keys of a request's headers object (`headers:` option or keyword, a `*header*` variable, a `*header*` function's return value, a same-file function's `*header*` parameter, `new Headers`, Go `http.Header{}`, spread conditionals), subscript writes and `set`/`append` on such a value, Go `req.Header.Set/Add`, Ktor `header()` and `headers { append() }`, builder `.header/.addHeader/.setHeader`.
- Readers: `X.headers.get` / `X.headers[...]` / `headers.get`, next/headers `headers()`, `req.header` / `req.get`, Python `X.headers.get` / `[...]`, FastAPI `Header(alias=)` and parameter-name headers (`x_token` is `x-token`, FastAPI's `convert_underscores`), Spring `@RequestHeader`, `request.getHeader`, Ktor `call.request.header`, Go `r.Header.Get` and gin `c.GetHeader`.
- One constant hop in the same file (module `const`, Python module assignment, Kotlin `val` outside functions, Go `const`/`var`).
- Index version `logic-15` (the orchestrator re-bumps at merge).

## Design choices

- **Header-shaped names only.** Letters, digits and `-` with at least one dash, in a header context; never arbitrary dict keys.
- **Standard headers are not recorded at all** (`is_standard_http_header`: `authorization`, `content-type`, `x-request-id`, `x-forwarded-*`, ...). The foundation already refuses to bridge them; recording them would add a node per repo that every handler touches and no crossing could use.
- **Responses are not requests.** A receiver, callee or variable naming `res`/`resp`/`response` (`nextResponse.headers.set`, `ResponseEntity...header`, `response.addHeader`, `new Response(body, { headers })`, `JSONResponse(headers=)`) and Go's `w.Header().Set` record nothing.
- **Test sources record nothing.** Their handlers are fake servers standing in for another service (`httptest` handlers in passless-cli read `X-Tenant-ID`), and their requests go to the repo's own service (`MockMvc` in idp sends `X-User-ID` to idp). Both would otherwise add cross-repo pairs that are not real.
- **Service-level by design.** A header contract joins every sender with every reader of the name; `X-Tenant-ID` is read by 14 idp handlers and sent from passless-cli and idp-front-end.
- Imported constants are not followed (none of the four target edges needs one).

## Non-goals

- Response headers as contracts (server sets, client reads).
- Headers whose name is built at runtime or imported from another file.
- Java, Rust and other languages' header APIs.
