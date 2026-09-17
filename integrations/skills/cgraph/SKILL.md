---
name: cgraph
description: "Use FIRST for any question about THIS codebase's structure, symbols, or relationships — where a function/class/file is defined, what calls or imports it, what breaks if you change it, how two parts connect, or to load focused source context before editing or reviewing. Routes to the cgraph MCP tools (graph_query / graph_explain / graph_impact / graph_path / graph_context / graph_report), which serve ranked file:line results, node neighborhoods, transitive blast radius, token-budgeted source bundles, and module dependency maps from a resident per-project graph daemon in ~10ms — far cheaper than grepping and reading files. Prefer over blind grep/read for code navigation, dependency tracing, and impact analysis."
trigger: /cgraph
---

# cgraph

cgraph keeps a live, queryable graph of the current project — symbols (functions,
classes, types, files), their call/import/inheritance/containment edges, and
centrality ranking — served by a resident per-project daemon through MCP tools.

**Reach for these tools before grepping or reading files** when a question is
about code structure or relationships. A graph call is one ~10ms round-trip and
returns exactly the file:line (and often the source) you need, instead of many
grep/read calls that burn context.

## Routing: question → tool

| When the user / task needs… | Call |
| --- | --- |
| "Where is X? Find the symbol named …" | `graph_query` `{query}` — ranked by importance, each hit has `source_file` + `line` |
| "What is X? Show its callers/callees/imports" | `graph_explain` `{id}` — node + neighbors (each with `direction` and a navigable brief) + a source snippet |
| "What breaks if I change X? What depends on it?" | `graph_impact` `{id, direction:"dependents"}` — transitive blast radius, bounded by `max_depth` |
| "What does X rely on?" | `graph_impact` `{id, direction:"dependencies"}` |
| "How does A connect to B?" | `graph_path` `{source, target}` — shortest path, with `path_nodes` briefs |
| "Load context on X" / before editing or reviewing X | `graph_context` `{query or id, budget}` — focal node + most-relevant neighbors with snippets, packed to a token budget |
| "What's the architecture? Give me a module map / what depends on what at the package level / where does a new file belong?" | `graph_report` `{view:"modules", format:"mermaid", scope?, depth?}` — files grouped into modules, imports/calls between them with counts, layers (0 = nothing depends on it), cycles listed; whole rows shed to the budget with `omitted` reported |
| "Which types are duplicated / redundant / bloated? Which interfaces or structs have the same shape? Which types are dead?" | `graph_report` `{view:"types", format:"markdown", scope?, threshold?, min_members?}` — `identical` (groups of differently named types with the same members), `duplicates` (one name in several files, with member overlap), `overlaps` (subset / ≥ threshold Jaccard pairs), `unreferenced` (no other symbol or file refers to them), each with file:line and members |
| "Where is copy-pasted code? Which functions are near-duplicates? What should be extracted into a shared helper?" | `graph_report` `{view:"clones", format:"markdown", scope?, threshold?, min_tokens?}` — `classes` of functions whose bodies are ≥ threshold similar after renaming (members with file:line-line, lowest pairwise similarity, shortest body in tokens); test-only classes in `test_classes` |
| "How does this program start? What are the main flows? Where does a request go? Give me a program-design overview." | `graph_report` `{view:"design", format:"markdown" or "mermaid", scope?, hops?}` — `entry_points` (main / route / page / root) ranked by reach, the top call `flow` from each to `hops`, `layers` by call distance, `unreached` count |
| "Which HTTP endpoints does this service expose? Which handler serves `GET /api/v1/…`? Who calls this endpoint? What breaks for API callers if I change this handler?" | `graph_query` `{query:"GET /api/v1/notebooks"}` — `endpoint` nodes (id `endpoint:<METHOD> <path>` with `{}` per parameter, one per route with its prefixes and mounts composed); `graph_explain` on one shows `handled_by` → the handler and `CONSUMES` ← every caller; `graph_impact` `{id: <handler>, direction:"dependents"}` reaches the endpoint and then its callers |
| "Verify the graph is current before I rely on it" | `graph_update {path:"."}` — blocking content-verified synchronization; returns `freshness.content_root`. Pin subsequent reads by passing the root as `expected_content_root`. |
| "Is the graph current? / I just changed files" | Nothing for ordinary reads — the daemon watches the tree and folds edits in within seconds. Use `graph_update` when you need a verified content_root to pin reads. |

## How to use the results

- `graph_query` matches case-insensitively and can be narrowed with `kind`
  (e.g. `"function"`, `"class"`, `"type"`, `"field"`, `"file"`), `file` (source-path
  substring), and `limit`. On zero matches it returns `suggestions` — the closest symbol names —
  so correct the spelling and retry instead of falling back to grep.
- Every id-taking tool (`graph_explain` / `graph_impact` / `graph_path` /
  `graph_context`) also accepts a bare symbol name (e.g. `"merge_fragments"`);
  the response echoes the canonical `id` it resolved. A miss comes back with
  `found: false` plus `suggestions`. A name several symbols share (`write_file`
  defined in many test files) is never resolved to one of them silently: the
  response is `found: false`, `ambiguous: true`, and `suggestions` holds the
  exact candidates (most central first, `candidate_count` in total) — pick the
  `id` you meant and call again. An empty or missing id is an error, not a search.
- `graph_query` returns ids and `source_file`:`line`. Open the file at `line`
  directly — no second search needed.
- `graph_explain` takes `direction` (`"in"` = callers/importers, `"out"` =
  callees/imports) and `limit`; neighbors are ordered most-important-first and
  `neighbor_count`/`truncated` flag when a hub has more edges than returned.
- `graph_context` is the highest-leverage call before an edit or review: ask for
  a budget (e.g. `4000`) and it returns the focal symbol's source plus its
  neighborhood's source, ranked and trimmed to fit. Read that instead of opening
  files one by one. It reports `omitted` / `truncated` if more existed.
- `graph_context` has two **gather** modes. The default (`gather:"fixed"`) packs
  the whole k-hop neighborhood. When you have a task in hand, pass
  `gather:"adaptive"` **together with a `query`** — it keeps the full 2-hop core
  but expands the third hop only along query-relevant nodes. On the retrieval
  eval that lifted grade-2 recall by **+0.057 for +13% candidate tokens**, versus
  the **+96%** a full 3-hop gather costs. The relevance gate is a no-op without a
  query, so never send `adaptive` without one. The response echoes
  `gather:"adaptive"`, `packing:"knapsack"`, and a `reach` summary
  (`{candidates, expanded_past_core, gated_at_core}` — `expanded_past_core: 0`
  means nothing relevant lay past the 2-hop core, so it collapsed to it). Raise
  or lower `gather_theta` to tighten or loosen the relevance threshold.
- `graph_impact` with `dependents` is the safety check before changing a
  signature or deleting a symbol: it lists everything that would be affected,
  by depth.
- `graph_report` answers architecture questions before any symbol-level call:
  `format:"mermaid"` returns a `graph LR` diagram in `rendered` (paste it as-is
  in a reply), `format:"json"` returns `modules` / `edges` / `layers` / `cycles`.
  Pass `scope:"src"` to leave out unrelated roots and `depth` to zoom (1 =
  top-level directories, 3 = sub-packages). Test roots are excluded unless
  `include_tests:true`. Check `omitted` -- a nonzero count means the budget
  dropped the lightest edges or modules; raise `budget` or narrow `scope` to see
  them.
- `graph_report` with `view:"types"` is the type-cleanup call. `identical`
  groups differently named types that declare exactly the same members -- the
  strongest bloat signal, and the one no other tool gives you. `duplicates`
  says which type names are declared in more than one file and how alike the
  declarations are (`min_jaccard`/`max_jaccard` over their members: 1.0 is a
  copy, 0.0 an unrelated homonym such as a component-local `Props`). `overlaps`
  pairs types whose members nest (`subset`, the smaller at least half of the
  larger) or match at or above `threshold` (`overlap`, default 0.80).
  `unreferenced` lists types no other symbol or file in the graph refers to;
  the graph carries cross-file references only, so a type used in its own file
  lands here too -- treat it as a lead, not a verdict. Members come from
  extracted `field` nodes, so interfaces made only of method signatures have no
  shape to compare. Only json and markdown; `min_members` (default 3) keeps
  `{id, name}` pairs out. Under the budget, identical groups survive first,
  then duplicates, then overlaps, then unreferenced; `omitted` says what fell.
- `graph_report` with `view:"clones"` is the duplicate-code call. Every
  function body is fingerprinted at extraction with identifiers and literals
  normalized away, so two copies that differ only in names, string literals or
  numbers compare equal, and an edited copy scores by how much of its
  token stream survived; `threshold` 0.80 (the default) is the "80% similar"
  fallow and SourcererCC use. `classes` are ranked largest first, then most
  similar, then longest; each member carries `file:line-end_line` so you can
  open all copies at once. Bodies under `min_tokens` (default 30) are getters,
  stubs and one-liners and are skipped. Classes whose members are all under test
  roots are `test_classes` -- real, common, less urgent -- unless
  `include_tests:true` merges them. A `hint` in the response means functions
  have no fingerprint yet (the daemon fast-loaded an older persist): call
  `graph_update` and retry. Only json and markdown.
- `endpoint` nodes are the service's HTTP surface as the code actually serves
  it: an Elysia/Express/Hono route's full path is composed through every
  `prefix`, `.basePath()` and `.use()` mount across files (turing-api's
  `notebookRoutes.get('/starred-notes')` under `/notebooks` under `/api/v1` is
  `endpoint:GET /api/v1/notebooks/starred-notes`), and a Next.js
  `app/api/x/[id]/route.ts` exporting `GET` is `endpoint:GET /api/x/{}`. The
  id carries no repo and `{}` for every parameter, so the same path in another
  repo's graph is the same node. Callers are `CONSUMES` edges into the
  endpoint: direct `fetch`, `api.GET`/`axios.post`, and calls through path
  wrappers like `apiFetch('/notebooks')` whose own `fetch(\`${base}${path}\`)`
  fixes the prefix. An endpoint this repo only calls carries `served: false`
  and no source; "who calls `GET /api/v1/…`" is `graph_impact` on the endpoint
  with `dependents`. Across repos, `cgraph seam discover --graph a=… --graph b=…`
  joins the two graphs' endpoints with no spec and `seam fuse` renders them.
  Contract documents count too: an OpenAPI JSON, `.proto` or `.graphql` file,
  or the openapi-typescript `paths` a client is typed against, gives
  `documented: true` endpoints and `schema` nodes with fields
  (`RESPONDS_WITH` / `ACCEPTS` link them). "Does the code match the API
  spec?" is the `drift:` line `seam discover` prints when a graph documents
  endpoints: promised but unserved, served but undocumented. "Is this type a
  copy of an API schema?" is `graph_report` `{view:"types"}`, where schemas
  are type owners.
- **Workspaces.** When the project root holds `cgraph.workspace.json`, every
  tool answers across the repositories it names, with no new tool and nothing
  copied: each repo keeps its own daemon and watcher. `graph_impact` on an API
  handler then reports the frontend callers too, reached through the shared
  `endpoint:` node, each witness carrying `repo` and `bridged_through`;
  `graph_path` joins two repos at a contract; `graph_query` and
  `graph_explain` merge and tag. A repo whose daemon is down is listed in
  `unreachable`, never dropped. `graph_report` and `graph_context` are
  per-project: at a workspace root they return `workspace_op_unsupported`
  naming the repo roots, so call them with one repo's root. `.group('/v2', app => …)` and `.guard()` callbacks, chains passed
  inline to `.use()`, aliased imports and cast re-exports all compose. A
  route on a router the file only receives as a function parameter is not
  minted (its mount is unknowable from that file); `stats.json`
  `route_resolution` counts these as `routes_unresolved`.
- `graph_report` with `view:"design"` is the orientation call for a repo you
  do not know: `entry_points` are where execution starts -- `main`, HTTP
  `route` handlers, framework `page` files (Next.js `app/**/page.tsx`), and
  `root` functions with callees that nothing in the graph calls (exported
  library surface, or unresolved callers) -- ranked by `reach`, the number of
  functions each transitively calls. Each carries its top `flow`: a call tree
  to `hops` (default 3) with four children per node chosen by reach and `more`
  for the rest, so the heaviest path is always drawn. `layers` says how many
  functions sit at each call distance from an entry and in which modules; the
  `unreached` count (with samples, most called first) is code no entry reaches.
  `format:"mermaid"` returns a `flowchart TD` of the kept flows to paste as-is;
  markdown gives the entry table, nested flows and the layer table. Narrow
  with `scope` when a monorepo has several programs.

## Freshness-sensitive navigation

When a task must rely on the graph being current with the worktree — before impact
analysis of a symbol you just moved, or after a batch of file edits — use the
synchronize-then-pin pattern:

```
1. update = graph_update {path: "."}
2. root = update.freshness.content_root
3. graph_query / graph_explain / graph_impact / graph_path / graph_context
       {…, expected_content_root: root}
```

`graph_update` is a blocking content-verified barrier: it hashes every detected
code file and re-extracts any whose content changed. The returned
`freshness.content_root` uniquely identifies that source snapshot. The response
also reports `files_hashed` and `bytes_hashed`. Passing the root as
`expected_content_root` on a subsequent read pins the response to the same
snapshot; the daemon returns an
error instead of graph data if it has published a different root since then.

Ordinary reads (without `expected_content_root`) do not scan the filesystem. They
read the latest published snapshot and return its `freshness` metadata; during
startup or for non-source seam graphs, `freshness.verified` can be `false`. The
daemon keeps source-backed snapshots current through automatic file watching.
Use synchronization and a pin when you need proof that the graph matches specific
source content.

## Session memory (survive /compact and /clear)

The daemon and `graph.json` live **outside your context window**, so a checkpoint
written before a `/compact` or `/clear` is still recall-able afterward. Use this
to carry the task thread across a context reset instead of losing it.

- `graph_remember {title, body, touches?, tags?}` — write one checkpoint. `body`
  is a **distilled** markdown summary of what you did and what's next; `touches`
  lists the code symbols (ids or bare names) the work concerns, so recall can link
  back to them. Persist only the distilled outcome — **never** raw tool output,
  DOM snapshots, or chain-of-thought.
- `graph_recall {query?, limit?}` — return recent checkpoints newest-first, each
  with its body and briefs of the code it touched. After a `/clear`, recall first
  (~KB payload) to restore the thread, then `graph_context` on a linked symbol to
  reload just-enough source.

The discipline is **distill → checkpoint → clear → recall**. Checkpoints are
inert to code analysis (they never shift query/impact/context rankings) and
survive daemon restart, incremental edits, and full rescans — the sidecars under
`cgraph-out/memory/` are the durable source of truth.

## Practicalities

- The `cgraph` MCP server must be registered (it auto-spawns a per-project daemon
  keyed to the project root). The first tool call in a project triggers a
  one-time graph build (seconds), then queries are warm (~10ms).
- While that first build runs, results carry `"graph_state": "building"` — an
  empty result then means "not built yet", not "no match". Retry after a few
  seconds or poll `graph_status` until `build_state` is `"ready"`.
- The daemon watches the project tree (`graph_status` reports `watching`): file
  edits land in the graph automatically within a few seconds, and the graph is
  re-persisted in the background.
- Fall back to grep/read only when cgraph genuinely has no answer — e.g. a
  string literal, a comment, a config value, or a file type cgraph does not
  extract. For symbols and their relationships, prefer the graph.
- cgraph indexes code structure deterministically. Prose/doc relationships
  appear only if semantic enrichment fragments have been ingested; don't assume
  documentation concepts are present unless `graph_status` shows enrichment.
