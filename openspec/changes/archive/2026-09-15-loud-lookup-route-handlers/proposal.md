# Fix: node lookup fails loud, and an HTTP route's inline handler is a function (CGR-4 leftovers)

## Why

Two precision items the CGR-4 plan folded into "relationship accuracy" were never built; both
were observed in the field test recorded in the cgraph-next-steps plan (2026-09-14).

**Lookup.** Every id-taking op (`explain`, `impact`, `path`, `context`, `remember`) resolves its
key through `resolve_node`: exact id, then exact label, then the label's leading token
case-insensitively, where "the highest-centrality match wins". Two consequences:

- `seam query explain` with a mistyped parameter name sent `id: ""`. No id or label is empty,
  but `label_symbol` of a label with no leading token (`(anonymous)`) is empty, so the empty key
  matched it in the bare-name tier and the most central such node came back as if asked for.
  Nothing said the parameter was missing.
- `explain {"id": "write_file"}` on CGraph's own graph, where `write_file` is defined in 35 test
  files, returned whichever definition the graph listed first. The exact-label tier took the
  first match; the bare-name tier took the most central. An agent asking about "the" `write_file`
  was answered about one of them with no sign there were others.

**Route handlers.** In turing-api, an Elysia module is one fluent chain assigned to a const:
`const notebookRoutes = new Elysia({prefix}).use(auth).get('/', handler, opts).post(...)`. The
JavaScript extractor emits the const as a single `variable` node spanning the whole chain
(`index.ts:36-255`), and every inline handler is an anonymous arrow, which the walker treats as
a boundary: no node, and the calls inside it (`s.listNotebooks(dbUser)`) are dropped. So a
provider-side source anchor can name the module but never the handler, and the routes have no
callees in the graph. CGR-13 (contract discovery) needs a node per handler to anchor an endpoint
to.

## What Changes

- `lookup_node` replaces the body of `resolve_node`. An empty key names nothing. Exact id wins.
  Otherwise the exact-label tier, then the case-insensitive bare-name tier, each MUST name
  exactly one node; several matches are returned as `candidates`, sorted most central first,
  then label, then id, and `node` stays null. `resolve_node` remains as the single-node view for
  the query intent route and `remember`'s touches, which already treat null as "unresolved".
- `explain`, `impact`, `context` and `path` report an ambiguous key as `found: false` (`focus:
  null`, `source_found: false`) with `ambiguous: true`, `candidate_count`, and the exact
  candidates in `suggestions` (capped at the existing five), so a host that already reads
  `suggestions` on a miss sees the answer set. `context` does not fall through to its free-text
  `query` when the `id` was ambiguous: the ambiguity is the answer.
- `handle_daemon_request` refuses an id-taking read whose key is absent, null, or empty before
  dispatch: `explain`/`impact` need `id`; `path` needs `source` and `target`; `context` needs
  `id`, `q`, or `query`. The refusal is the ordinary `{ok: false, error}` envelope, so the CLI
  exits non-zero and MCP surfaces the message.
- `LanguageConfig::nested_function_scope`: a predicate through which a language opts a nested
  anonymous function back into being a node and a call scope. The walker consults it only where
  it would otherwise treat an arrow as a boundary; `resolve_function_name` still supplies the
  name.
- The JavaScript/TypeScript config sets it to `is_route_handler`: the last function-valued
  argument of a call `<x>.<verb>(<string>, ...)` where `<verb>` is `get`, `post`, `put`, `patch`,
  `delete`, `head`, `options` or `all`. The handler is labelled `<root>.<verb> <path>`, where
  `<root>` is the identifier the fluent chain hangs off (`app`) or, for a chain rooted in a
  constructor, the variable the chain is assigned to (`notebookRoutes`); `<path>` is the string
  literal's text. Earlier function arguments (Express middleware) stay anonymous. The module
  `variable` node is unchanged.
- Host skill text: an ambiguous name is reported, not resolved; pick an id and call again.

### Non-goals
- Path prefixes are not composed (`/notebooks` + `/starred-notes`): that is the endpoint node
  CGR-13 mints, and this handler node is what it will anchor to.
- `router.route('/x').get(handler)` and `app.use('/x', handler)`: no path argument on the verb
  call, or not a verb. Named handlers (`app.get('/x', handleX)`) already resolve as calls to
  `handleX` and need no node.
- Python (`@app.get`), Go (`mux.HandleFunc`), Java (`@GetMapping`): their handlers are named
  functions already.
- The substring and lexical-overlap search tiers of `query` and `context` are unchanged; only
  the exact tiers are gated.

## Impact

- **Touches:** `src/engine/daemon_ops.cpp`, `src/engine/include/cgraph/language_config.hpp`,
  `src/engine/extractor.cpp`, `src/engine/javascript_extractor.cpp`,
  `tests/smoke/daemon_ops_test.cpp`, `tests/smoke/javascript_extractor_test.cpp`,
  `integrations/skills/cgraph/SKILL.md`.
- Behaviour change for hosts: a bare name that used to resolve to the most central of several
  symbols now returns `found: false, ambiguous: true` with the candidates. An op called with no
  key now errors instead of returning an empty or unrelated result.
- Graph change: TypeScript/JavaScript repos that register HTTP routes inline gain one `function`
  node per handler plus the `CALLS` edges from its body. Measured on turing-api (742 files): 475
  handler nodes (0 before), function nodes 825 -> 1300, 989 CALLS edges with a handler as caller;
  `notebookRoutes` alone splits into 34 handlers. On CGraph's own graph `write_file` reports 35
  candidates.

## Capabilities

### Modified Capabilities
- `graph-daemon-client` — id-taking ops resolve exact keys only, report ambiguity, and refuse a
  missing key.
- `deterministic-graph-pipeline` — an HTTP route's inline handler is a function node and a call
  scope in JavaScript and TypeScript.
