## 1. Cold workspace

- [x] 1.1 Real-daemon test: hold one member's build on a FIFO and ask the workspace; fails on bin-v0.6.4 (answer carries `building`, no consumer).
- [x] 1.2 `build_wait` on `ClientRequest` and the settle loop in `send_thin_client_request` for graph-reading ops.
- [x] 1.3 Confirm the test fails with `build_wait` 0 and passes with the default.

## 2. Consumer extraction

- [x] 2.1 Extractor test for typed awaited calls and for a call or member leading a URL; fails on the old extractor (two calls missing, `/oracles` and `/items` minted).
- [x] 2.2 Read the callee through the `await_expression` wrapper when the call has type arguments.
- [x] 2.3 A leading call leaves the request unresolved; a leading local or member stays the host (a stricter first version lost 113 provider matches on the probe repositories and was replaced).
- [x] 2.4 Bump `kIndexVersionKey` to `logic-8`.

## 3. Fused ids

- [x] 3.1 Test: two services owning `src_db_client_ts` stay two scoped nodes and share one endpoint.
- [x] 3.2 Scope service-local ids in `fuse_seam`; rewrite seam edges through each shadow's service.
- [x] 3.3 Update the existing fuse assertions to the scoped ids.

## 4. Verification

- [x] 4.1 Full default suite: 83 of 84, the one failure `cgraph_file_watcher_test` (fails on mars at origin/main too, passes in CI).
- [x] 4.2 Real flow: re-ran the 2026-09-29 probe on the eight pinned repositories. With no daemon running, the first workspace `impact` on `GET /api/v1/org/stats` returned 9 nodes from turing-api and turing-webapp after a 22.5 s wait (bin-v0.6.4: `total: 0`). Seam discover matches 426 Turing endpoints (unchanged) and 228 ModSquad (was 124). The ModSquad login scenario now reaches idp-front-end's `app/api/auth/login/route.ts` at depth 2 (bin-v0.6.4 returned only tests and the mock server). `POST /oracles`, `POST /runs` and `GET /cluster` are no longer minted. The fused Turing graph holds `turing-api::src_db_client_ts` (304 imports) and `turing-agents::src_db_client_ts` (1) where it held one node with 305.
- [x] 4.3 OpenSpec validation (`--strict`).
