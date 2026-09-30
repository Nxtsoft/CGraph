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

## 4. Review round 1 (PR #144)

- [x] 4.1 Workspace federation mirrors the seam's owner rule, and a contract reached only inside a repo is not crossed to its proxied callers (saml regression test).
- [x] 4.2 Non-string manifest and prefix members are errors, not JSON exceptions.
- [x] 4.3 `--prefix` naming a repo no `--graph` provides exits 2 (`unknown_prefix_repos`, shared with the manifest check).
- [x] 4.4 The four unimplemented crons routes are dropped (49 routes); one docs URL.

## 5. Review round 2 (PR #144)

- [x] 5.1 One owner rule (`proxy_crosses_at`) shared by seam discover/fuse and every workspace mapping point: `shared_contract`, the impact seed, `reachable_contracts` and `spellings_in` (three-repo saml regression test).
- [x] 5.2 `http.mount_prefix` prefixes the served routes (Python server), normalized as langgraph-api 0.15.1 does (one trailing `/` dropped, empty or `/` is the root, `/noauth` reserved); a rejected value is a warning and no routes; the JS server ignores it.
- [x] 5.3 `seam discover` no longer prints "--prefix prefix names repo".
