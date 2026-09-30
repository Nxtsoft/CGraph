# Status says what it could not resolve, and an edit is warned before it crosses a service

## Why

Phase 1 part 1 (#136) made cross-service answers reachable from inside one repository, but an agent still has to ask. The 2026-09-29 incident mining put missed cross-service consumers first (17 of 52), and a context engine that waits to be asked does not prevent them. Two gaps remain from the plan (`~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`, Phase 1):

- `graph_status` cannot tell "no caller" from "calls CGraph could not resolve". The daemon computed the route and call tallies on every rebuild and threw them away (`resolve_contracts(graph, raw_relations)` in `rebuild_graph` passed no tally); only the one-shot CLI wrote them to `stats.json`.
- Nothing puts cross-service callers in front of an agent before it edits the file that serves or calls them.

## What Changes

- **Status reports `route_resolution`.** `rebuild_graph` returns the `ContractResolution` tally; full rescans and incremental updates store it on `DaemonState` (under `enrichment_mutex`, copied across the private scan and hydration states), and `status` reports it with the same fields as `stats.json` (`contract_resolution_json`, now shared). It is `null` after a fast-load start until the first rebuild. A workspace `status` carries it per repository.
- **Per-file cross-service lookup.** `cross_service_for_file` asks the home daemon for a file's endpoints (`impact` toward dependencies, two hops: `contains` means the file serves the route, `CONSUMES` means it calls it) and runs the same `cross_service_section` change context uses (now public in `change_context.hpp`). It returns the section and plain `summary` lines. `cgraph-client cross-service '{"file": PATH, "wait_ms": N}'` exposes it.
- **Pre-edit hook.** `integrations/hooks/cgraph-pre-edit.sh` (a Claude Code `PreToolUse` hook for `Edit|Write|MultiEdit`) runs `cgraph-client pre-edit`, which reads the hook's stdin JSON, finds the file's repository (nearest `.git`), and prints `hookSpecificOutput.additionalContext` listing each other service behind the file's endpoints. It prints nothing when there is nothing to say, never blocks an edit, and waits at most `CGRAPH_HOOK_WAIT_MS` (default 3000) for cold daemons. Like the existing hook it uses no Python or Node.

## Contract that tests verify

`client_runtime_test.cpp`, real daemons for `api` and `web`:
- `status` from `web` carries `route_resolution` with at least three calls and zero unresolved; with the tally not stored after a rescan, the test fails.
- `pre_edit_hook_output` for an Edit of api's routes file returns a `PreToolUse` `additionalContext` naming `GET /api/v1/stats, called from web src/stats.ts` and `loadStats`; for a file that serves and calls nothing it returns nothing. With served routes ignored, the test fails.

## Measured on the probe repositories

- `cgraph-pre-edit.sh` fed Claude Code's `PreToolUse` JSON for an Edit of turing-api `src/modules/org/index.ts`: six lines, one per route with a caller, e.g. `serves GET /api/v1/org/stats, called from turing-webapp lib/org-api.ts:83 (getOrgStats)`. For `package.json` and for invalid input: no output, exit 0.
- `cgraph-client cross-service` on idp-front-end `app/api/auth/login/route.ts`: `calls POST /api/v1/auth/login, served by idp …/AuthController.kt:105 (login)`.

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
