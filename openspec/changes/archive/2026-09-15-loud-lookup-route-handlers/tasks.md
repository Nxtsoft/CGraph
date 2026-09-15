# Tasks

## 1. Loud lookup
- [x] 1.1 `daemon_ops_test.cpp`: empty/missing/null `id` on `explain` and `impact` is `ok: false`
      with "id is required"; `path` without `target` and `context` without `id`/`query` likewise;
      `context` with only `query` still succeeds.
- [x] 1.2 `daemon_ops_test.cpp`: two `write_file` nodes -> `explain`/`impact` answer `found: false`,
      `ambiguous: true`, `candidate_count: 2`, `suggestions` most central first; `context` with an
      ambiguous `id` and a resolvable `query` still reports the ambiguity; `path` reports
      `source_ambiguous`; the canonical id and a unique exact label still resolve; a bare name
      shared by two symbols is ambiguous.
- [x] 1.3 `lookup_node` / `NodeLookup` / `describe_miss` in `daemon_ops.cpp`; `resolve_node` delegates.
- [x] 1.4 `missing_key_error` in `handle_daemon_request`, before dispatch.

## 2. Route handlers
- [x] 2.1 `javascript_extractor_test.cpp`: an Elysia chain yields `notebookRoutes.get /` and
      `notebookRoutes.post /:id/notes` function nodes with the arrow's extent, contained by the file,
      each the caller of its body's calls; `app.get('/health', ...)` yields `app.get /health`;
      `app.use`, `router.route('/x').get(...)`, `.map` and `describe` callbacks stay anonymous with
      their calls dropped; Express middleware before the handler stays anonymous; a template-literal
      path is a path.
- [x] 2.2 `LanguageConfig::nested_function_scope`; the walker's arrow gate consults it.
- [x] 2.3 `route_handler_name` / `chain_root_name` / `is_route_handler` in `javascript_extractor.cpp`,
      wired through `resolve_js_function_name` and `base_config`.

## 3. Docs and verification
- [x] 3.1 `integrations/skills/cgraph/SKILL.md`: ambiguity and missing-id behaviour.
- [x] 3.2 `ctest --preset default`: 77/77 pass (93 s, this Mac, Debug preset).
- [x] 3.3 Measured on turing-api (`~/turinglabs/full-turing/turing-api`, 742 files): 475 route-handler
      function nodes (get 186, post 148, delete 65, patch 55, put 21) where the origin/main-era binary
      emitted 0; function nodes 825 -> 1300 (the delta is exactly the handlers); 989 CALLS edges now have
      a handler as caller. `notebookRoutes` (index.ts:31-250) alone yields 34 handlers, each spanning its
      own arrow. The six `*Routes` variable nodes are unchanged. On CGraph's own graph,
      `explain {"id":"write_file"}` answers `ambiguous: true, candidate_count: 35`; `impact {"node": ...}`
      (the mistyped key from the field test) answers `ok: false, "id is required"`; `path` with an
      ambiguous source reports `source_ambiguous`; `explain {"id":"merge_fragments"}` still resolves.
- [ ] 3.4 Obtain non-author review and merge.
