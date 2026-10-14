## 1. Extraction

- [x] 1.1 `walk_node` threads a call scope beside the function scope; a boundary arrow keeps the enclosing caller, or asks `closure_call_scope` when there is none.
- [x] 1.2 JS/TS `closure_call_scope`: the class for a class-field arrow, else the module-level binding with a node, else the file.
- [x] 1.3 `RawCall::in_closure` marks calls handed down into a closure.
- [x] 1.4 Index version `logic-23`.

## 2. Resolution

- [x] 2.1 `resolve_raw_calls` refuses bare-name guesses for closure calls; `dropped_closure_guess` in the partition and in `stats.json`.

## 3. Tests

- [x] 3.1 Generic walker: closure calls inside a function, at module scope with and without the hook, and a named function inside a closure (`extractor_test.cpp`).
- [x] 3.2 Fixture `tests/fixtures/ts_closure_calls/` through `run_one_shot`: the CALLS set is exactly the seven closure / test-callback calls and no edge reaches a decoy (`javascript_extractor_test.cpp`).
- [x] 3.3 Evidence-only binding for closure calls, unchanged binding outside closures (`graph_builder_test.cpp`).
- [x] 3.4 Each of 3.1-3.3 fails with its half of the change disabled.

## 4. Gates

- [x] 4.1 h2h scorer through cgraph-mcp: turing-api `assertProductCategoryAccess` caller files 7/11 to 11/11, precision 1.0; CGraph `write_file_atomically` 10/10 before and after.
- [x] 4.2 Node/edge deltas on turing-api, turing-webapp and CGraph; only CALLS changes; 20 sampled new CALLS edges checked at their call sites.
