## 1. LangGraph config

- [x] 1.1 `detect_language` recognizes `langgraph.json` (`langgraph-config`).
- [x] 1.2 `extract_langgraph_config`: server and graph nodes, one `file_route` per Agent Server route, `http.disable_<group>` honored.
- [x] 1.3 Tests: `langgraph_config_test.cpp`, `detect_test.cpp`.

## 2. Proxy prefixes

- [x] 2.1 `endpoint_prefixes.cpp`: parse, normalize, map, inverse, CLI flag.
- [x] 2.2 `seam discover` / `seam fuse` take `--prefix`; `CONSUMED_AT.via`; per-prefix log; fuse refuses an unjoined proxied edge.
- [x] 2.3 Workspace manifest `prefixes`; `impact` and `path` cross the proxy.
- [x] 2.4 Tests: `endpoint_prefixes_test.cpp`, `seam_test.cpp`, `workspace_test.cpp`.

## 3. Verification

- [x] 3.1 Fail-before/pass-after for each test.
- [x] 3.2 Probe scoring, precision sample, lost-match check, live workspace federation.
- [x] 3.3 README and SKILL updated.
