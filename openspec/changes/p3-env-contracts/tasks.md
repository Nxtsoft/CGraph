## 1. Extraction

- [x] 1.1 `env_contracts.cpp`: `uses_contract env:<NAME>` facts for JavaScript/TypeScript (direct, destructured, subscript, typed env objects), Python, Go, Kotlin, Java and Spring application config; one per reading symbol and name.
- [x] 1.2 Hook-in calls in `js_extra_walk`, `python_extra_walk`, `kotlin_http_walk`, `go_extra_walk`, a Java `extra_walk`, and the SpringConfig dispatch.
- [x] 1.3 Index version `logic-15`.

## 2. Tests

- [x] 2.1 `env_contracts_test.cpp`: every reading form and every refusal per language, each fact's source is a node of the file, and `resolve_contracts` mints an unserved, unbridged `env:` node with `CONSUMES` from the reader. Fails with the hooks reverted.

## 3. Real-repo gate

- [x] 3.1 Eight probe repos: no node or edge lost or changed; only `env` nodes and `CONSUMES` edges added; 40 sampled facts hand-checked.
- [x] 3.2 Seam + scorer with `--env NEXT_PUBLIC_API_URL=turing-api --env ML_BACKEND_BASE_URL=ml-backend --env AUTH0_AUDIENCE=turing-api --env BACKEND_URL=idp`: T36, T37, T40, M38 link; no previous link lost; without declarations no env node enters the seam.
