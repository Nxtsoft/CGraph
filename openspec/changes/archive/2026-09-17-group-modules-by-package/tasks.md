# Tasks

## 1. Reading the manifests
- [x] 1.1 `package_manifests_test.cpp`: npm `workspaces` globs with names from each package's own
      manifest, an unnamed package taking its directory name, a glob hit with no manifest ignored,
      `node_modules` skipped at the root and inside a package; the `workspaces.packages` object
      form; pnpm `packages:` with `**` and `!` and a following top-level key ending the list;
      Cargo `members` across lines with `exclude`, names from `[package] name`; `go.work` with a
      bare `use` carrying a comment and a `use ( … )` block, names from `go.mod`; a root with no
      workspace, a malformed manifest and globs matching nothing all declaring none; a nested
      package owning its own files while the outer one keeps the rest.
- [x] 1.2 `package_manifests.hpp/.cpp`: the four readers, the glob matcher, the bounded directory
      walk reusing `is_skipped_directory`, `discover_workspace_packages`, `package_for_file`
      (canonicalizing so a symlinked prefix cannot hide a package).

## 2. The modules view
- [x] 2.1 `report_test.cpp::test_modules_by_package`: a workspace supplies the module names and the
      report says which manifest and how many packages; a file outside every package keeps its
      directory module; dependencies aggregate between packages; `group_by depth` ignores the
      manifest; a root declaring no workspace falls back to depth.
- [x] 2.2 `ModuleGrouping`, `ReportRequest.module_grouping`, `ModulesReport.grouping/manifest/
      packages`, the grouping in `build_modules_report`, the caption, both JSON envelopes, and
      `group_by` parsing with a typed error.
- [x] 2.3 CLI `--group-by`, MCP `group_by`, usage text.

## 3. Docs and verification
- [x] 3.1 README, skill, host contract.
- [x] 3.2 `ctest --test-dir build/default`: 82/82 pass. GCC 14 `-fsyntax-only` on mars passes for
      every changed source and test.
- [x] 3.3 Field test on real monorepos, each with a daemon from this build:
      project-cybertron (npm, 11 packages, 8,443 nodes) names `@cybertron/desktop`,
      `@cybertron/agent-daemon`, `@cybertron/sdk` where depth says `apps/desktop`,
      `services/agent-daemon`, `packages/sdk`; turing-rewrite (npm, 6) names `@turing/database`,
      `@turing/shared-types`; langgraph-example (pnpm, 3) names
      `@langgraph-example/api-server`, `agents`, `web`; ripgrep (Cargo, 10) names `grep-matcher`,
      `grep-printer`, `grep-searcher`, `ignore`, `globset` where depth says `crates/matcher` and
      so on, while `crates/core` (no `Cargo.toml` of its own) correctly stays a directory.
      Module and edge counts are identical under both groupings in every repo, so no file is
      dropped. CGraph itself: `group_by` `depth`, the same five modules. An unknown `group_by`
      is a typed error naming `auto|packages|depth`.
      The first run reported zeroes and missing fields: the roots were served by the installed
      binary from this morning, and a cold daemon answers from an empty graph. The rerun restarts
      each daemon on this build and waits for `build_state: ready`.
- [ ] 3.4 CI green; merge.
