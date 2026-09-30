## 1. Status

- [x] 1.1 `rebuild_graph` returns the contract tally; rescans and incremental updates store it; copied across scan and hydration states.
- [x] 1.2 `status` and workspace `status` report `route_resolution` (shared `contract_resolution_json`).

## 2. Pre-edit warning

- [x] 2.1 `cross_service_section` public; `cross_service_for_file`; `cgraph-client cross-service`.
- [x] 2.2 `pre_edit_hook_output`, `cgraph-client pre-edit`, `integrations/hooks/cgraph-pre-edit.sh` (no Python or Node).

## 3. Verification

- [x] 3.1 Real-daemon tests for status tallies and the hook (positive and silent); each fails when checked against broken code.
- [x] 3.2 Full default suite: 83 of 84, the one failure `cgraph_file_watcher_test` (flaky on mars).
- [x] 3.3 Real flow: the hook on turing-api `src/modules/org/index.ts` names six turing-webapp callers; silent on `package.json` and invalid input.
- [x] 3.4 OpenSpec validation (`--strict`).
