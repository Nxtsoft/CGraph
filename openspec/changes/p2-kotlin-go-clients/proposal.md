# Kotlin and Go HTTP clients consume endpoints

## Why

On the 2026-09-29 ModSquad probe, passless-app (Kotlin Multiplatform, Ktor) and passless-cli (Go) call idp's Spring API, yet their graphs had `route_resolution.calls = 0`: only the JavaScript extractor recorded client calls. 15 of the 40 hand-labelled ModSquad edges (M15-M29) were blocked by this alone (`no_non_ts_consumers` in `probe-base067/miss_causes.json`). Plan: `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`, Phase 2.

## What Changes

- **Kotlin (Ktor).** A `get`/`post`/`put`/`patch`/`delete`/`head`/`options` member call with an argument, on a receiver whose last name ends in `client`, `http` or `api` (`client`, `httpClient`, `this.client`, `registered.client`; not `clients`, `clientRepository`, `httpCache`), records an `http_call` with the verb as method. The URL is read from a string template: literal text up to `?`/`#`; one leading `$x`/`${x}` is the host; a `$id` filling a segment is `{}`; the enclosing function's parameter at the end is a wrapper tail; `+` concatenation reads the same way. `client.request(url) { method = … }` is recorded unresolved (the verb is in the builder).
- **Go (net/http).** `http.Get`/`Head`/`Post`/`PostForm(url)`, `http.NewRequest(method, url, …)`, `http.NewRequestWithContext(ctx, method, url, …)`, and any call passing a context, then a method (`http.MethodX` or an uppercase literal verb), then a URL-like argument (`c.Do(ctx, http.MethodPost, "/api/v1/auth/refresh", body)`, `scoped.DoForm(ctx, http.MethodPost, "/oauth2/token", form)`, `s.Client.Mutate(cmd.Context(), "PATCH", path, nil)`). The receiver's name cannot decide this in Go (`c`, `scoped` are clients; `r`, `e`, `router` are routers), so the call's shape does: a request carries its `context.Context` first, as `http.NewRequestWithContext` does, while route registrations (gin `r.Handle`, chi `r.Method`, echo `e.Add`, httprouter `HandlerFunc`), `httpmock.RegisterResponder`, `assert.Equal(t, http.MethodPost, r.Method)` and log lines take none. As in the JavaScript extractor, a call with a function-literal argument is a registration or callback, never a request; `httptest` builds server-side requests and is skipped. A literal after the verb must read as a path or URL (`git.Run(ctx, "HEAD", "--quiet")` is no request). The net/http entry points take absolute URLs (a leading value is the host); a client method's path is relative to its own base, so a value in front of it (`n.Base+"/import"`) leaves the call unresolved.
- **Wrappers.** A client call whose URL is a parameter of the enclosing named function (also from inside a lambda / func literal there, e.g. `withContext(Dispatchers.IO) { client.post("$baseUrl$path") }` or `retry(func() error { c.Do(ctx, http.MethodPost, path, body) })`, but never a parameter the lambda declares itself) records `http_wrapper` for that function with the call's method and prefix (`postAuth(ctx, path, req)` -> `c.Do(ctx, http.MethodPost, path, body)` is `POST ""`; `postLoginOutcome(path, …)` -> `client.post("$baseUrl$path")` likewise). A call to a bare name (Kotlin `postLoginOutcome(path = "/api/v1/auth/login")`, Go `c.postAuth(ctx, "/api/v1/auth/login", req)` by its field name) records an `http_call` with its first path-like string argument; resolve_contracts keeps it only when the name binds, in that file, to a wrapper. Unlike the JavaScript extractor, the tail may be any parameter (Go wrappers take `ctx` first) and a leading host before the tail is accepted (`$baseUrl$path`).
- **Unresolved requests are counted.** A primitive request whose URL is unresolvable, or whose method is held in a variable, or whose parameter tail is not a prefix wrapper (`path+"?"+q`) is recorded with an empty path, so `route_resolution.calls_unresolved` counts it.
- `resolve_contracts` is unchanged: the new facts use the existing `http_call` / `http_wrapper` shapes.

## Contract that tests verify

`tests/smoke/configured_extractors_test.cpp`:
- `check_kotlin_http_clients`: exact `http_call`/`http_wrapper` facts for a Ktor client class: direct get/patch/delete (including a call inside a lambda passed to a helper, attributed to the enclosing function), a named-argument wrapper call, a positional wrapper call with a non-path string before the path, two wrappers, and the refusals (`client.request`, `$host$prefix`, an absolute URL, `cache.get`, `restClient.post().uri(…)`, `log("GET", "fine")`).
- `check_go_http_clients`: exact facts for `c.Do`, `scoped.DoForm` with a literal verb and a raw string, `"/users/"+id+"/sessions"`, the `postAuth` wrapper and its call, `Mutate(ctx, "POST", n.Base+"/import")` (unresolved), a query tail (unresolved), `http.NewRequestWithContext` with a variable verb (unresolved) and `http.Get(base + "/healthz")`.
- `check_http_clients_consume_spring_routes`: Kotlin Spring controllers + the Kotlin and Go clients through merge and `resolve_contracts`: `SessionsApi.kt:login` and `internal/api/auth.go:Login` consume `endpoint:POST /api/v1/auth/login` (handled by `AuthController.login`), `revoke` consumes `endpoint:PATCH /api/v1/sessions/{}/invalidate`, `Me` consumes `endpoint:GET /api/v1/auth/me`, and exactly 6 calls are unresolved.
- On origin/main the test exits 9 (`kotlin http facts`: no facts at all).

## Measured on the probe repositories

Graphs for idp, passless-app and passless-cli rebuilt with this binary (the four Turing repositories contain no `.kt`/`.go` file, so their graphs are unchanged), `seam discover`/`fuse`, `score.py`:
- ModSquad links 4 -> 15 of 40 (HTTP 4 -> 15 of 29); gained M15-M21 and M24-M27; lost none. Turing unchanged at 12 of 41.
- passless-app: `calls` 0 -> 43, `consumes` 0 -> 40, `calls_unresolved` 2. passless-cli: `calls` 0 -> 33, `consumes` 0 -> 18, `calls_unresolved` 15. idp's route_resolution and CONSUMES edges are identical (12 before and after).
- Seam: passless-app -> idp 29 links (25 endpoints), passless-cli -> idp 16 (9 endpoints); 20 sampled against source (consumer URL line and provider `@RequestMapping` + method mapping) are all correct. 6 further rows attach the same correct endpoints to idp-front-end's mock backend (`scripts/mock-backend-server.js`) and BFF refresh route, which serve the same paths.
- Still unlinked: M22 (verb and path are data fields, `client.request("$baseUrl${probe.endpoint}")`), M23 (Spring Actuator `/actuator/health` has no code route), M28 and M29 (table-driven `Base:` field concatenated three files away).

## Non-goals

- Data-driven requests (a method and path held in a struct or list), Ktor `defaultRequest` base URLs and `url { path(…) }` builders, Retrofit/OkHttp/Spring `RestClient`/`WebClient` clients, Go `fmt.Sprintf` URLs.
- Cross-file wrapper resolution for Go methods (a package has no per-file imports) and Kotlin (imports are not resolved).
- Inlining Kotlin/Go constants into a URL (`$API_BASE/users`).
- Go client methods without a leading `context.Context` (`c.Do(http.MethodGet, "/x", nil)`), or whose context argument is not named `ctx`/`context`: they are not counted in `calls` at all, the price of not reading route registrations (`r.Handle(http.MethodGet, "/x", h)`) and assertions as requests. `http.NewRequest` and the other net/http entry points are read regardless.

## Impact

- `src/engine/configured_extractors.cpp`: `ClientUrl`, `kotlin_http_walk` (Kotlin `extra_walk`), `go_http_walk` (called from `go_extra_walk`).
- `tests/smoke/configured_extractors_test.cpp`.
- README.md, `integrations/skills/cgraph/SKILL.md`.
- Graph output for Kotlin and Go repositories gains `CONSUMES` edges and `served: false` endpoint nodes; the index version bump is the orchestrator's.

## Follow-up

- Share `ClientUrl` with the JavaScript extractor's `UrlTemplate` once #145 lands; this change keeps its own reduction.
