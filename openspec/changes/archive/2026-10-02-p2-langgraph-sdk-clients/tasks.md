## 1. Extractor

- [x] 1.1 Table of the SDK 1.11.1 REST methods (`assistants`, `threads`, `runs`, `crons`, `store`) to verb and path, with the argument-dependent branches the SDK takes.
- [x] 1.2 A receiver is an SDK client only when it provably is one (SDK `new Client`, through a local set once, a module constant, a `this.x` field, a same-file factory, or an imported function); such calls record `http_call` or `langgraph_call`.
- [x] 1.3 A module function whose every return is `new Client(...)` records `langgraph_client`.

## 2. Contracts

- [x] 2.1 A `langgraph_call` whose function resolves through the file's imports to a `langgraph_client` is a consumer; any other is skipped without a count.
- [x] 2.2 Index key `cgraph-index-v1:logic-12`.

## 3. Verification

- [x] 3.1 `http_consumers_test` (the LangGraph block) and `contracts_test` `test_langgraph_sdk_clients` fail on main and pass after.
- [x] 3.2 Full default suite: 86/87; only `cgraph_file_watcher_test` fails, the known mars-local failure this change does not touch.
- [x] 3.3 Probe repos (all 8) against bin-v0.7.1: 0 CONSUMES lost; 29 gained, all to Agent Server routes and all checked by hand (turing-webapp 632 -> 656 across 11 files, turing-agents 53 -> 58 from two scripts that build `new Client(...)` themselves). Of turing-webapp's 48 SDK call sites outside tests, 34 are read; the other 14 are `client: Client` parameters (9) and the reassigned `let client` in `use-ic-stream.ts` (5). Seam scorer: Turing HTTP 23/25 -> 24/25 (T15 linked), ModSquad HTTP 23/29 unchanged, no other row changed; `seam discover` matched endpoints 550 -> 555 with "consumed with no provider" unchanged at 28.
