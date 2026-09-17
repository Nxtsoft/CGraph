# Tasks

## 1. Manifest and federation
- [x] 1.1 `workspace_test.cpp`: a directory is a workspace only with the manifest; a valid one
      loads its repos in order with roots resolved against it and round-trips relative;
      a missing, malformed, empty, duplicate-named or dangling manifest is an error with no
      repos, and refuses every op with `workspace_invalid`; discovery finds git repos one level
      down, sorted, skipping dotted and non-repo directories.
- [x] 1.2 `workspace.hpp/.cpp`: `is_workspace_root`, `load_workspace`, `workspace_manifest_json`,
      `discover_workspace_repos`, `RepoAsk`, `federate_workspace_request`.

## 2. The ops
- [x] 2.1 `workspace_test.cpp`: status totals count only reachable repos and name the down one;
      query sums totals, re-ranks by centrality, truncates to the caller's limit and tags every
      hit; explain answers from the repo that has the node, names the others sharing a contract,
      and misses loudly listing the repos searched.
- [x] 2.2 `workspace_test.cpp::test_impact_bridges_the_contract`: a handler in one repo reaches a
      consumer in another at the endpoint's depth plus its own, tagged with its repo and the
      contract crossed; the owning repo's own witnesses are kept; the bridge is named; the seed
      is not its own witness; `max_depth` 1 leaves no budget to cross; a seed no repo has misses.
- [x] 2.3 `workspace_test.cpp::test_path_bridges_and_unsupported_ops`: a cross-repo path joins at
      the contract naming it once, tags each node's repo, and reports the contract and both
      repos; no shared contract is an empty path; update fans out; report/context/remember/
      recall/shutdown are refused with `workspace_op_unsupported` and the repo roots.
- [x] 2.4 The six federated ops and the typed refusal in `federate_workspace_request`.

## 3. Surfaces, docs, verification
- [x] 3.1 `send_thin_client_request` federates a workspace root (so the client, CLI and MCP all
      do); `cgraph workspace init|status` and the usage text.
- [x] 3.2 README, skill, host contract, CLAUDE.md.
- [x] 3.3 `ctest --test-dir build/default`: 81/81 pass. GCC 14 `-fsyntax-only` on mars passes for
      `workspace.cpp`, `client_runtime.cpp`, `cli/main.cpp` and `workspace_test.cpp`.
- [x] 3.4 Field test: a workspace over turing-api (`origin/dev` 26a691c) and turing-webapp
      (`origin/main` 01e03897), both daemons live and warm.
      `workspace status`: 2 repos reachable, 32,594 nodes / 64,420 edges (api 12,095, web 20,499).
      `impact` on the starred-notes handler at the workspace root: **17 witnesses, 3 in api and
      14 in web**, bridged through `endpoint:GET /api/v1/notebooks/starred-notes` at depth 1;
      the same call at the api root alone returns 3 and no web witness. The web witnesses are
      `notebooksApi` at depth 2 and eight hook modules at depth 3 (`use-starred`, `use-notes`,
      `use-tags`, `use-templates`, `use-entity-autolink`, `use-notebooks`,
      `use-generate-experiment-summary`) plus the generated `api-types.d.ts` and its `paths`.
      `query` "starred-notes": 4 hits across both repos, each tagged. `explain` on the endpoint:
      `repo` api, `also_in` `["web"]`. `path` handler → `useStarredNotes`: five nodes,
      `repos ["api","web"]`, `bridged_through` the endpoint. With the web daemon stopped,
      `status` reports `reachable: 1` and `unreachable: ["web"]`. `report` at the workspace root
      is refused with `workspace_op_unsupported` and both repo roots.
      The first run found no cross-repo `path`: the bridge looked for contracts with
      `dependencies` only, while an endpoint points *at* its handler and the `path` op is
      undirected. It now looks in both directions (pinned by the unit test's api/web fixture).
- [ ] 3.5 CI green; merge.
