# Agents inside one service see the services that depend on it

## Why

Workspace federation (CGR-14) answers across repositories only when the tool is pointed at the workspace directory itself. An agent is opened in one service repository, so it never federates: on the 2026-09-29 probe of the Turing and ModSquad repositories no `cgraph.workspace.json` existed anywhere, and the tools an agent uses to understand an edit (`graph_change_context`, `graph_context`) do not cross repositories even at a workspace root. 52 incidents mined from the same repositories' PRs and sessions put missed cross-service consumers (17) and schema coupling (12) first; the lead one is idp#468, which broke passless-app's revoked-session detection. Plan: `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`, Phase 1.

## What Changes

- **The enclosing workspace is found.** `find_enclosing_workspace` walks up from a project root to the nearest ancestor (up to and including `$HOME`) holding a manifest that lists a repository containing that root; the repository is `home`, and its root is replaced by the project root so a worktree answers from its own tree.
- **`impact` and `path` cross from a member root.** The thin client federates those two ops when the root is inside a member; the result carries `workspace: {name, home}`. `query`, `explain`, `context`, `status` and every other op stay in the home repository. Forwarded member asks set `ClientRequest::federate = false`, so a member never re-federates.
- **`change_context` adds `cross_service`.** Given a `CrossServiceAsk`, it collects the endpoints the change serves (an endpoint it edits, one whose handler it reaches through `handled_by`, or one a changed file directly contains) and the endpoints it calls (`CONSUMES` from changed code, or from a function that `CALLS` a changed helper). It asks each other repository for direct `CONSUMES` callers of served endpoints and the `handled_by` handler of called ones, one hop each. Rows carry `contract`, `relation` (`consumer` or `provider`), `repo`, `id`, `label`, `kind`, `path`, `line`, `depth`. At most 24 contracts are asked. The section has its own budget (a quarter of the total, at least 256 tokens), is never shed for impacts, counts trims in `omitted.cross_service`, and lists `unreachable` and `building` repositories.
- `change_context_across_workspace` in the client runtime is the one entry the CLI (`cgraph change-context`) and the MCP server (`graph_change_context`) both call; `handle_mcp_request` takes the runner as an optional argument, since the MCP library does not link the client runtime.

## Contract that tests verify

- `workspace_test.cpp`: a member root finds its workspace with `home` and both roots; a worktree nested in a member is answered from its own root; a directory the manifest does not list, and a manifest above `$HOME`, are not used.
- `client_runtime_test.cpp`, real daemons for `api` and `web`: `impact` on `endpoint:GET /api/v1/stats` from the `api` root returns web's `loadStats` tagged `repo: web` and `workspace.home: api`; `query` for `loadStats` from the `api` root does not return it; `change_context_across_workspace` on an edit inside the api handler returns one contract, `endpoint:GET /api/v1/stats`, and a `consumer` row for `web` `src/stats.ts` `loadStats`. Each of these fails when member federation is disabled, when the cross-service ask is dropped, or when every reachable endpoint counts as served.

## Measured on the probe repositories

- An edit inside turing-api's `GET /api/v1/org/stats` handler: `cross_service.contracts` is that one endpoint, and the one row is turing-webapp `lib/org-api.ts:83` `getOrgStats` (15 s with the other three repositories' daemons starting cold). A first version counted every endpoint reachable from the change: 11 contracts (`GET /health`, `GET /test`, ...) and not the edited one, because the edited endpoint is itself a seed at depth 0 and the router chain spans the file.
- An edit inside idp-front-end's login route: contracts `POST /api/auth/login` (served) and `POST /api/v1/auth/login` (called); the provider row is idp `idp-core/.../AuthController.kt:105` `login`, across TypeScript and Kotlin. A first version counted endpoints consumed by any dependent: 10 contracts including sessions and SAML.

## Non-goals

- `graph_context` across repositories, `unresolved_calls` in `graph_status`, and a pre-edit hook: the next change.
- Contracts other than HTTP endpoints (tables, env vars, claims): Phase 3.
- A repository outside the workspace directory tree does not find the workspace by upward search (it is still a member for everyone else's answers).

## Impact

- `src/engine/workspace.cpp`, `workspace.hpp`: `find_enclosing_workspace`, `EnclosingWorkspace`.
- `src/client/client_runtime.cpp`, `client_runtime.hpp`: `federate` flag, member-root federation, `forwarding_ask`, `cross_service_scope_for`, `change_context_across_workspace`.
- `src/engine/change_context.cpp`, `change_context.hpp`: `CrossServiceAsk`, contract collection, `cross_service` section.
- `src/mcp/mcp_server.cpp`, `mcp_server.hpp`, `src/mcp/main.cpp`, `src/cli/main.cpp`: the runner.
- No graph output, id or index change.
