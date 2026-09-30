# Agents inside one service see the services that depend on it

## Why

Workspace federation (CGR-14) answers across repositories only when the tool is pointed at the workspace directory itself. An agent is opened in one service repository, so it never federates: on the 2026-09-29 probe of the Turing and ModSquad repositories no `cgraph.workspace.json` existed anywhere, and the tools an agent uses to understand an edit (`graph_change_context`, `graph_context`) do not cross repositories even at a workspace root. 52 incidents mined from the same repositories' PRs and sessions put missed cross-service consumers (17) and schema coupling (12) first; the lead one is idp#468, which broke passless-app's revoked-session detection. Plan: `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`, Phase 1.

## What Changes

- **The enclosing workspace is found.** `find_enclosing_workspace` walks up from a project root (both the path as given, so a member reached through a symlink is found, and the resolved path) to the nearest ancestor, up to and including `$HOME`, holding a manifest that lists a repository containing that root; the most specific such repository is `home`, and its root is replaced by the project root so a worktree answers from its own tree. A manifest that lists the root but cannot be loaded (a member not cloned here) is returned with its errors, and callers report them in `workspace.errors` instead of answering as if alone.
- **`impact` and `path` cross from a member root.** The thin client federates those two ops when the root is inside a member; the result carries `workspace: {name, home}`. `query`, `explain`, `context`, `status` and every other op stay in the home repository. Forwarded member asks set `ClientRequest::federate = false`, so a member never re-federates. An `expected_content_root` pin names the home graph: only the home ask carries it, and a home pin that fails fails the request, as it does for a lone project.
- **`change_context` adds `cross_service`.** Given a `CrossServiceAsk`, it collects the endpoints the change serves (rank 0: an endpoint it edits; rank 1: one whose handler it reaches through `handled_by`, or one a changed file directly contains), the endpoints it removes or adds (rank 0: served in the base snapshot and not the target, or the reverse, which is how a changed mount prefix or route path shows up), and the endpoints it calls (rank 1: `CONSUMES` from changed code; rank 2: from a function that `CALLS` a changed helper). In rank order, at most 24 contracts are asked: each other repository for direct `CONSUMES` callers of served, removed or added endpoints and for the `handled_by` handler of called ones, one hop each; a repository that fails is asked once and named in `unreachable`. Rows carry `contract`, `rank`, `relation` (`consumer` or `provider`), `repo`, `id`, `label`, `kind`, `path`, `line`. The section is bounded by a quarter of the budget, trimmed lowest rank first (rows, then contracts), is never shed for impacts, and never causes a rejection: when mandatory evidence needs the room it shrinks to a stub counting what it held, then goes away entirely.
- **The shared build wait starts at the first cross-repository ask**, after change context has built its own two snapshots.
- `change_context_across_workspace` in the client runtime is the one entry the CLI (`cgraph change-context`) and the MCP server (`graph_change_context`) both call; `handle_mcp_request` takes the runner as an optional argument, since the MCP library does not link the client runtime.

## Contract that tests verify

- `workspace_test.cpp`: a member root finds its workspace with `home` and both roots; a worktree nested in a member is answered from its own root; a directory the manifest does not list, and a manifest above `$HOME`, are not used.
- `workspace_test.cpp` also: a member reached through a symlink, the more specific of two overlapping members, and a manifest naming a member that is not present (returned with errors, not skipped).
- `client_runtime_test.cpp`, real daemons for `api` (an Elysia router chain mounted by an app that also serves `/ping`) and `web`, with `billing` listed but unreachable:
  - `impact` from the `api` root returns web's `loadStats` with `workspace.home: api`; `query` for `loadStats` from there does not.
  - A wrong `expected_content_root` fails the member `impact`; the home repo's real pin still returns web's caller.
  - An edit inside the stats handler: contracts exactly `endpoint:GET /api/v1/stats`, a `consumer` row for web's `loadStats`, none for `loadHealth` or `loadPing`, and `billing` named unreachable once.
  - Moving the app's prefix from `/api/v1` to `/api/v2`: the old routes are removed contracts and both web callers are named.
  - An edit inside web's `loadStats`: a `provider` row in api's `src/routes/stats.ts`.
  - The smallest budget at which change context alone succeeds still succeeds with the section.
  - Each fails when checked against broken code: counting every reachable endpoint as served (adds `/ping`), dropping removed contracts, sending the pin to every member, or rejecting before shrinking the section.

## Measured on the probe repositories

- Review found three blockers in the first version (pins sent to every member; a mount change reported no contracts while the docs promised empty meant none; a manifest naming an absent member silently turned the feature off) and the fixes above address them.
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
