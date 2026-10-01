## 1. Extractor

- [x] 1.1 `http_wrapper` records `~<index>` for the options a wrapper spreads, `?` for its own unreadable method, and `A|B` for a method choice.
- [x] 1.2 A call to a non-primitive function no longer reads its second argument as options; `O` descriptors carry choices.
- [x] 1.3 A path argument in `http_call_args` is read as a client call reads its URL: constants, locals, `new URL(x)`, `.toString()` and `.href`.

## 2. Contracts

- [x] 2.1 A wrapper with any marker is read from `http_call_args`; the caller's options at the recorded index override the wrapper's method; unreadable options or an unreadable wrapper method no caller overrides are counted unresolved.
- [x] 2.2 A wrapper with no marker sends its own method whatever a caller passes.
- [x] 2.3 Index key `cgraph-index-v1:logic-11`.

## 3. Verification

- [x] 3.1 `contracts_test` `test_first_parameter_wrapper_options` fails on main (`createInput` and `topLevel` consume GET, `ignored` consumes POST) and passes after.
- [x] 3.2 Full default suite: 86/87; only `cgraph_file_watcher_test` fails, the known mars-local failure this change does not touch.
- [x] 3.3 Probe repos (all 8): the only CONSUMES change is turing-agents `library-tool.ts`, 51 -> 53: -GET .../ingredient, -GET .../top-level, +POST .../ingredient, +PATCH .../top-level, +POST /api/v1/composites, +POST /api/v1/inputs. A first cut lost 3 edges (idp-front-end `fetchWithTimeout(backendUrl.toString())`, turing-webapp `jsonFetch(LINKS_URL)` and `authFetch(url)`) because `http_call_args` read only literal paths; task 1.3 fixed it and the rerun lost none.
