## ADDED Requirements

### Requirement: HTTP headers a client sends and a server reads become header contracts
The JavaScript/TypeScript, Python, Kotlin and Go extractors SHALL record `uses_contract` with context `header:<name>` where code sends a request header and `provides_contract` with context `header:<name>` where a server handler reads one, the name as the code spells it (`resolve_contracts` folds case). The source SHALL be the innermost enclosing function; a sender outside any function SHALL be attributed to its file, and a reader outside any function SHALL record nothing. A name SHALL be recorded only when it is header-shaped (letters, digits and `-`, at least one `-`, no leading or trailing `-`) and not a standard HTTP header (`is_standard_http_header`), and only in a header context:
- senders: a key of an object/dict/map literal that is a request's headers (the value of a `headers` option or keyword, held in or assigned to a name containing `header`, returned by a function whose name contains `header`, passed at a same-file function's parameter whose name contains `header`, `new Headers({...})`, Go `http.Header{...}`, spread into such an object), a subscript write or `set`/`append` on such a value, Go `X.Header.Set/Add`, Kotlin Ktor `header(name, v)` and `headers { append(name, v) }`, and builder `.header` / `.addHeader` / `.setHeader`;
- readers: `X.headers.get(name)` / `X.headers[name]` / `headers.get(name)`, next/headers `headers().get(name)`, Hono / Express `req.header(name)` / `req.get(name)`, Python `X.headers.get(name)` / `X.headers[name]`, FastAPI `Header(alias=...)` parameters and a `Header()` parameter's name with `_` read as `-` unless `convert_underscores=False`, Spring `@RequestHeader(name)` / `(name = ...)` / `(value = ...)`, `request.getHeader(name)`, Ktor `call.request.header(name)`, Go `X.Header.Get/Values(name)` and gin `c.GetHeader(name)`.
Code about a response SHALL record nothing: a receiver, callee or variable naming `res`, `resp` or `response` (`nextResponse.headers.set`, `ResponseEntity.status(...).header(...)`, `response.addHeader`, `new Response(body, { headers })`, `JSONResponse(headers=...)`), and Go's ResponseWriter `w.Header().Set`. A name MAY be a constant one hop away in the same file (a module `const`, a Python module assignment, a Kotlin `val` outside functions, a Go `const`/`var`); imported constants and interpolated strings SHALL NOT be read. A test source (`*_test.go`, `*.test.ts`, `*.spec.ts`, `test_*.py`, `*_test.py`, `*Test.kt`, or under `test`, `tests`, `__tests__` or `e2e`) SHALL record no header fact: its handlers are fake servers standing in for another service, and its requests go to its own service under test or to such a fake.

#### Scenario: A TypeScript client and a Kotlin controller share a header
- **GIVEN** `headers["X-Act-As-Org"] = org` in `backendAuthHeaders` and `@RequestHeader("X-Tenant-ID", required = false)` on `token`
- **THEN** `backendAuthHeaders` uses `header:X-Act-As-Org` and `token` provides `header:X-Tenant-ID`, and a repo sending `x-tenant-id` meets `token` at the one id `header:x-tenant-id`

#### Scenario: One constant hop
- **GIVEN** `const BACKEND_HEADER = 'x-ml-backend'` and `headers.get(BACKEND_HEADER)` in `pickBackend`, and Python `WEBAPP_ENV_HEADER = "X-Webapp-Env"` with `Header(default=None, alias=WEBAPP_ENV_HEADER)` on `get_session`
- **THEN** `pickBackend` provides `header:x-ml-backend` and `get_session` provides `header:X-Webapp-Env`

#### Scenario: Standard headers, responses and ordinary keys are not contracts
- **GIVEN** `Authorization`, `Content-Type`, `X-Request-ID` and `user-agent` in request headers, `ResponseEntity.status(400).header("X-Error-Reason", ...)`, `nextResponse.headers.set('x-tenant-id', ...)`, `w.Header().Set("X-Served-By", ...)`, and `{ 'X-Not-A-Header': 1 }` held in `plain`
- **THEN** none of them records a fact

#### Scenario: A test source records no header fact
- **GIVEN** `request.headers.get('x-act-as-org')` and a request sending `X-Act-As-Org` in `client.test.ts`
- **THEN** neither records a fact
