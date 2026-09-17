# Tasks

## 1. Facts at extraction
- [x] 1.1 `javascript_extractor_test.cpp`: `route` facts carry chain, verb and path for Elysia and
      Hono chains; `mounts` facts for `.use(x)`, `.use(x as any)`, `.use(x) as unknown as T`,
      `.route('/p', x)`, `.use('/p', x)` and not for `.use(cors())` or `router.route('/x')`;
      `route_prefix` from `new Elysia({ prefix })` and `.basePath()`; an aliased import keeps
      `alias` on its stub; inline `.use(new Elysia(...))` chains and `.group()` parameters root
      at the enclosing chain with the path beneath; a function-parameter router leaves the chain
      empty; a cast alias records `aliases`; a Next.js route file's exported `GET`/`PATCH` record
      `file_route` facts.
- [x] 1.2 `route_registration`, `is_type_wrapper`/`unwrap_expression`, `resolve_chain` (identifier,
      declarator, inline `.use()` argument, `.group()`/`.guard()` parameter, unresolvable),
      `push_route_facts`, `chain_route_prefix`, `route_mount_handler`, `js_extra_walk`,
      alias-carrying `collect_specifier_names`; `ExtraWalk` gains the `RawRelation` sink.

## 2. Endpoint minting
- [x] 2.1 `contracts_test.cpp`: `join_route_path` and `next_route_path` cases; the turing-api
      shape (prefix, cast suite, `/api/v1` chain, `app`, three import hops) mints
      `GET /api/v1/notebooks/starred-notes` with `handled_by`, `contains`, `mounts` edges and
      the tally; Express mount paths and a router mounted twice; an unmounted router; a
      function-parameter router refused and counted; the aliased-import / inline-chain /
      group / guard / cast-alias shapes from the first field test; a Next.js route file; a mount
      cycle; a routerless repo gains nothing.
- [x] 2.2 `contracts.hpp/.cpp`: `is_http_verb`, `join_route_path`, `next_route_path`,
      `resolve_contracts`; `ContractResolution` in `operation_stats`; `build_relation_scopes` /
      `resolve_scoped_name` shared with `resolve_raw_relations`, which skips the four new kinds;
      `resolve_imports` carries an import's `alias` onto the relinked edge.
- [x] 2.3 `pipeline.cpp` and `incremental_update.cpp::rebuild_graph` run `resolve_contracts`
      after `resolve_raw_relations`; `stats.json` gains `route_resolution`.
- [x] 2.4 `dedup_test.cpp`: three sibling endpoints in one community and file survive dedup;
      `dedup.cpp` exempts the `endpoint` kind.

## 3. Docs and verification
- [x] 3.1 Skill routing row and notes, README, CLAUDE.md pipeline note.
- [x] 3.2 `ctest --test-dir build/default` on this Mac (Debug preset): 79/79 pass. The dedup
      endpoint test fails 1-of-3-survive with the exemption removed and passes with it.
- [x] 3.3 Field test, turing-api at `origin/dev` (26a691c, 1,158 files, 5.3-6.4 s): 642 route
      facts (the multiline source scan finds 645 `.<verb>('<path>', handler)` shapes), 633
      endpoints, 627 under `/api/v1`, `app`'s own four at the root (`GET /health`, `/`, `/test`,
      `/sentry-test`), two module-level test apps; 7 refused, all test-local chains
      (`describe(() => { const app = … })` and anonymous test chains); 230 mounts, 4 unresolved.
      `endpoint:GET /api/v1/notebooks/starred-notes` anchors at `modules/notebooks/index.ts:75`.
      The first pass lost 10 `/config/*` routes to `import { config as configModule }`, the deck
      route to `deckModule = deckRoutes as unknown as Elysia`, `/protected` to an inline
      `.use(new Elysia()…)` chain and `app`'s routes to `export type App` folding onto `app`;
      each is now a pinned test case.
- [x] 3.4 Field test, turing-webapp at `origin/main` (01e03897, 1,753 files, 8-9.6 s): 29 route
      facts, 18 endpoints (every `app/api/**/route.ts` export), 11 refused, all MSW mock
      handlers on `http` imported from `msw`.
- [x] 3.5 CGraph's own graph: 1558 nodes / 3538 edges, unchanged; `route_resolution` all zero.
- [x] 3.6 GCC 14 `-fsyntax-only` on mars passes for every changed source and test.
- [ ] 3.7 CI green; merge.
