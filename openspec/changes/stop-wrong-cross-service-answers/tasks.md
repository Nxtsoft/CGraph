## 1. Cold workspace

- [x] 1.1 Real-daemon test: hold one member's build on a FIFO and ask the workspace; fails on bin-v0.6.4 (answer carries `building`, no consumer).
- [x] 1.2 `build_wait` on `ClientRequest` and the settle loop in `send_thin_client_request` for graph-reading ops.
- [x] 1.3 Confirm the test fails with `build_wait` 0 and passes with the default.

## 2. Consumer extraction

- [x] 2.1 Extractor test for typed awaited calls and for a call or member leading a URL; fails on the old extractor (two calls missing, `/oracles` and `/items` minted).
- [x] 2.2 Read the callee through the `await_expression` wrapper when the call has type arguments.
- [x] 2.3 A leading opaque interpolation leaves the call unresolved; remove `dropped_host`.
- [x] 2.4 Bump `kIndexVersionKey` to `logic-8`.

## 3. Fused ids

- [x] 3.1 Test: two services owning `src_db_client_ts` stay two scoped nodes and share one endpoint.
- [x] 3.2 Scope service-local ids in `fuse_seam`; rewrite seam edges through each shadow's service.
- [x] 3.3 Update the existing fuse assertions to the scoped ids.

## 4. Verification

- [ ] 4.1 Full default suite.
- [ ] 4.2 Real flow: re-run the 2026-09-29 probe (Turing and ModSquad) with the new binary; the cold scenario returns its nodes on the first ask, `POST /oracles` and `GET /items`-style truncations are gone, the typed awaited idp-front-end calls appear, and the fused graph holds both `src_db_client_ts` nodes.
- [ ] 4.3 OpenSpec validation.
