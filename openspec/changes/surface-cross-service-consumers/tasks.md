## 1. Enclosing workspace

- [x] 1.1 `find_enclosing_workspace`: upward search to `$HOME`, member containment, home root replaced by the project root.
- [x] 1.2 Test: member root, nested worktree, unlisted directory, `$HOME` boundary (`workspace_test.cpp`).

## 2. Member-root federation

- [x] 2.1 `ClientRequest::federate`; forwarded asks set it false; shared `forwarding_ask` for workspace roots and member roots.
- [x] 2.2 `impact` and `path` federate from a member root and carry `workspace: {name, home}`; other ops stay home.

## 3. Cross-service change context

- [x] 3.1 `CrossServiceAsk`; collect served and called contracts; ask other repos for direct callers and handlers; own budget.
- [x] 3.2 `change_context_across_workspace` for the CLI and the MCP server (`handle_mcp_request` runner).
- [x] 3.3 Real-daemon test in `client_runtime_test.cpp`; fails when member federation, the cross-service ask, or the served rule is broken.

## 4. Review round 1

- [x] 4.1 Pins go to the home repo only; a failing home pin fails the request.
- [x] 4.2 Removed and added endpoints (base vs target) are contracts, so a moved mount names its old callers; contracts ranked and trimmed by rank; docs no longer claim empty means none.
- [x] 4.3 A manifest listing the root but unusable is reported, not skipped; symlinked members and overlapping members are handled.
- [x] 4.4 The section never causes a rejection; a failing repo is asked once; the shared wait starts at the first ask.
- [x] 4.5 Tests for the provider side, a mount move, the unreachable repo, a small budget, pins, and the served filter (Elysia chain with an app-level route); tool descriptions, CLAUDE.md and host-skill-contract updated.

## 5. Verification

- [x] 5.1 Full default suite: 83 of 84, the one failure `cgraph_file_watcher_test` (flaky on mars at origin/main).
- [x] 5.2 Real flow on the probe repositories: turing-api handler edit names turing-webapp `lib/org-api.ts:83`; idp-front-end login edit names idp `AuthController.kt:105`.
- [x] 5.3 OpenSpec validation (`--strict`).
