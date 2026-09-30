## 1. Status

- [x] 1.1 `rebuild_graph` returns the contract tally; rescans and incremental updates store it; copied across scan and hydration states.
- [x] 1.2 `status` and workspace `status` report `route_resolution` (shared `contract_resolution_json`).

## 2. Pre-edit warning

- [x] 2.1 `cross_service_section` public; `cross_service_for_file`; `cgraph-client cross-service`.
- [x] 2.2 `pre_edit_hook_output`, `cgraph-client pre-edit`, `integrations/hooks/cgraph-pre-edit.sh` (no Python or Node).

## 3. Review round 1

- [x] 3.1 The per-file walk follows `contains`, `defines`, `method` and `CONSUMES` only (`impact` takes a relation list), so importers claim nothing and class methods count.
- [x] 3.2 The home repository still building, unchecked endpoints and unreachable repositories are named; the headline claims a crossing only when one was found.
- [x] 3.3 One wait for the whole lookup; at most 8 endpoints per hook; relative paths; monorepo members; tallies saved with the graph and restored on fast load.

## 4. Verification

- [x] 4.1 Real-daemon tests for status tallies and the hook (positive and silent); each fails when checked against broken code.
- [x] 4.2 Full default suite: 83 of 84, the one failure `cgraph_file_watcher_test` (flaky on mars).
- [x] 4.3 Real flow: the hook on turing-api `src/modules/org/index.ts` names six turing-webapp callers; silent on `package.json` and invalid input.
- [x] 4.4 OpenSpec validation (`--strict`).
