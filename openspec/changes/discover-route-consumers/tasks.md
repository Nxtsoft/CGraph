# Tasks

## 1. Consumer facts
- [x] 1.1 `javascript_extractor_test.cpp`: `http_wrapper` for `apiFetch(path)` over an inlined
      `base` const and for a fixed-method `del(path)`; `http_call` facts for a wrapper call from an
      object-literal arrow (attributed to the module variable, `{}` for `${id}`, query cut), a
      direct `fetch` with host interpolation and a segment parameter, openapi-fetch `api.GET`,
      `axios.post` over `+` concatenation, and a URL in a variable (empty path); no fact for
      `http.get('/x', handler)` (a route) or `cache.get('/key')` (no client).
- [x] 1.2 `module_const_value`, `UrlTemplate` / `collect_url_template`, `looks_like_http_client`,
      `consumer_scope_id`, `options_method`, `http_call_handler` wired into `js_extra_walk`.

## 2. Resolution and ids
- [x] 2.1 `contracts_test.cpp::test_canonical_ids`: `:id`, `{id}`, `[id]` → `{}`, `*` kept; a
      served endpoint keeps its spelling in label and `path` with `{}` in the id.
- [x] 2.2 `contracts_test.cpp::test_consumers`: wrapper consumers with method from options or
      GET; `served: false` external endpoints with no source; direct fetch with a segment
      parameter; fixed-method wrapper; openapi-fetch `{id}`; a consumer of the repo's own
      Next.js route joins the served node; external URL and Map lookup refused; the tally.
- [x] 2.3 `canonical_route_path`; `resolve_contracts` mints by canonical id, takes over a
      consumer-minted node when a route serves it, resolves wrappers and adds `CONSUMES`;
      `ContractResolution` gains four fields; `resolve_raw_relations` skips the two new kinds.

## 3. seam discover
- [x] 3.1 `seam_test.cpp::test_discover`: two graphs → services, the matched endpoint with
      `SERVED_BY` / `HANDLED_BY` / `CONSUMES` / `CONSUMED_AT` and code-ref shadows, the served
      copy winning over the placeholder, a consumer-only endpoint kept and marked, an unused
      endpoint dropped, a missing handler node adding no shadow, the `matched 1` log line, a
      valid ingestable fragment, fuse, byte-stable regeneration, a missing graph refused.
- [x] 3.2 `SeamGraph` reads `links` and string properties; `discover_seam`; CLI
      `cgraph seam discover` and usage text.

## 4. Docs and verification
- [x] 4.1 README, skill, host contract, CLAUDE.md.
- [x] 4.2 `ctest --test-dir build/default`: 79/79 pass. GCC 14 `-fsyntax-only` on mars passes for
      every changed source and test.
- [x] 4.3 Field test, turing-webapp at `origin/main` (01e03897, 1,753 files, 9.5-11.7 s): 632 client
      calls, 113 unresolved (105 `fetch(url)` with the URL in a variable, absolute external URLs,
      all-parameter paths), 514 `CONSUMES` edges, 423 consumed-only endpoints; all 18 own-route
      consumers join their served nodes. turing-api: 53 calls, 13 unresolved, 30 `CONSUMES`, 9
      consumed-only. Three precision passes on the way: a first parameter filling a whole segment
      is `{}` not the wrapper tail; a wrapper over an imported base constant resolves the constant
      project-wide (unique name) instead of yielding a prefix-less path; an all-parameter call path
      is refused; env-derived hosts built with `.replace()` count as hosts; a local variable is
      never a project-wide reference.
- [x] 4.4 `seam discover --graph api=… --graph web=…`: api serves 633 / consumes 18, web serves 18 /
      consumes 443; 410 matched, 33 consumed with no provider, 241 served with no consumer; 1,712
      fragment nodes / 2,299 edges; `seam fuse` renders 33k nodes; `seam query impact` on the
      starred-notes handler (dependents, depth 3) reaches the endpoint at 1, `service:web` and
      `notebooksApi` at 2, and the webapp hooks importing it (`use-notes`, `use-tags`,
      `use-entity-autolink`, `use-templates`) at 3.
- [x] 4.5 CGraph's own graph unchanged (1558 / 3538).
- [ ] 4.6 CI green; merge.
