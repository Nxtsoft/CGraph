# Status says what it could not resolve, and an edit is warned before it crosses a service

## Why

Phase 1 part 1 (#136) made cross-service answers reachable from inside one repository, but an agent still has to ask. The 2026-09-29 incident mining put missed cross-service consumers first (17 of 52), and a context engine that waits to be asked does not prevent them. Two gaps remain from the plan (`~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`, Phase 1):

- `graph_status` cannot tell "no caller" from "calls CGraph could not resolve". The daemon computed the route and call tallies on every rebuild and threw them away (`resolve_contracts(graph, raw_relations)` in `rebuild_graph` passed no tally); only the one-shot CLI wrote them to `stats.json`.
- Nothing puts cross-service callers in front of an agent before it edits the file that serves or calls them.

## What Changes

- **Status reports `route_resolution`.** `rebuild_graph` returns the `ContractResolution` tally; full rescans and incremental updates store it on `DaemonState` (under `enrichment_mutex`, copied across the private scan and hydration states), and `status` reports it with the same fields as `stats.json` (`contract_resolution_json`, now shared). The index manifest saves it with the graph, so a fast-loaded daemon restores the tallies of the build its graph came from; a manifest written before this change restores none, and status then says `null`. A workspace `status` carries it per repository.
- **`impact` can follow a set of relations.** `relation` accepts a list as well as one name (`trace_impact` gains an overload taking a set), so a walk can follow a file's own structure (`contains`, `defines`, `method`, `CONSUMES`) without going through its imports.
- **Per-file cross-service lookup.** `cross_service_for_file` asks the home daemon for a file's endpoints (`impact` toward dependencies over `contains`, `defines`, `method` and `CONSUMES` only, three hops: `contains` at depth 1 means the file declares the route, `CONSUMES` means one of its functions or class methods calls it; an imported file's endpoints are never claimed) and runs the same `cross_service_section` change context uses (now public in `change_context.hpp`, with a contract cap). The home repository is asked through the same ask as the others, so one wait covers the lookup. It returns the section and plain `summary` lines, including when the home repository is still building, when endpoints were left unchecked by the cap, and which repositories could not answer. A relative `file` is relative to the project root; a file outside it is an error. `cgraph-client cross-service '{"file": PATH, "wait_ms": N}'` exposes it.
- **Pre-edit hook.** `integrations/hooks/cgraph-pre-edit.sh` (a Claude Code `PreToolUse` hook for `Edit|Write|MultiEdit`) runs `cgraph-client pre-edit`, which reads the hook's stdin JSON, takes the file's repository (nearest `.git`) as the root when a workspace lists it and otherwise the workspace member holding the file (members sharing one repository), and prints `hookSpecificOutput.additionalContext`. It opens "crosses into other services" only when a caller or handler was found, and "could not fully check" when it can only say what it could not ask. It checks at most 8 of the file's endpoints per edit and names the rest. It prints nothing when there is nothing to say, never blocks an edit, and waits at most `CGRAPH_HOOK_WAIT_MS` (default 3000) for cold daemons, across all of them. Like the existing hook it uses no Python or Node.

## Contract that tests verify

`client_runtime_test.cpp`, real daemons for `api` and `web`:
- `status` from `web` carries `route_resolution` with at least three calls and zero unresolved, and still does after the daemon restarts from its saved graph.
- `pre_edit_hook_output` for an Edit of api's routes file names `GET /api/v1/stats, called from web src/stats.ts` and `loadStats`. It is silent for a plain file, for a file that only imports the routes file, and for one that only imports a caller. For a class method that fetches `/api/v1/ping`, it names api's `src/server.ts`. It still warns when the members share one repository. While the edited repository is still building, it says "(this repository) is still building" under "could not fully check".
- `incremental_update_test.cpp`: incremental updates keep the tally (1 call, then 2 with 1 unresolved).
- Each of these fails when checked against broken code: walking through imports, going silent while home builds, dropping the tally on fast load, no monorepo fallback, not storing the tally after a rescan, ignoring served routes.

## Review round 1

Two blockers: a file that only imports a routes file claimed its endpoints (13 false lines for a turing-api integration test), and the hook was silent while the edited repository was still building. Both are fixed as above, along with a doubled wait, 7-second warm edits (now capped at 8 endpoints: 2.3 s on turing-api's largest routes file), silently dropped endpoints, missed class methods, monorepos, relative paths, and tallies missing after fast load.

## Measured on the probe repositories

- `cgraph-pre-edit.sh` fed Claude Code's `PreToolUse` JSON for an Edit of turing-api `src/modules/org/index.ts`: six lines, one per route with a caller, e.g. `serves GET /api/v1/org/stats, called from turing-webapp lib/org-api.ts:83 (getOrgStats)`. For `package.json` and for invalid input: no output, exit 0.
- `cgraph-client cross-service` on idp-front-end `app/api/auth/login/route.ts`: `calls POST /api/v1/auth/login, served by idp …/AuthController.kt:105 (login)`.
- turing-api `src/modules/luna/__tests__/index.int.test.ts` (imports the luna routes): silent in 0.07 s (13 false lines before the fix). `src/modules/project/index.ts`: 2.3 s warm, 8 endpoints checked, "25 more endpoint(s) in this file were not checked".

## Non-goals

- `graph_context` across repositories (still refused at a workspace root).
- Hooks for hosts other than Claude Code.
- Contracts other than HTTP endpoints (Phase 3).

## Impact

- `src/engine/incremental_update.cpp`, `daemon_server.cpp`, `daemon_ops.cpp`, `include/cgraph/daemon_ops.hpp`, `operation_stats.cpp/.hpp`, `workspace.cpp`: the tally in status.
- `src/engine/change_context.cpp`, `include/cgraph/change_context.hpp`: `cross_service_section` public.
- `src/client/client_runtime.cpp/.hpp`, `src/client/main.cpp`: `cross_service_for_file`, `pre_edit_hook_output`, `cross-service` and `pre-edit` commands.
- `integrations/hooks/cgraph-pre-edit.sh`; README and skill.
- No graph output, id or index change.
