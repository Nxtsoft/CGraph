# Feature: module grouping from workspace manifests (CGR-15)

## Why

`report modules` groups files by the first two directory components, which is a guess at a
question the repository has already answered. A monorepo declares its units in its build manifest:
`workspaces` in a `package.json`, `packages:` in a `pnpm-workspace.yaml`, `members` in a Cargo
`[workspace]`, `use` in a `go.work`. Reading it makes the module map say `@turing/web` and
`ui-kit`, the names the team says out loud and the ones the build tool enforces, instead of
`apps`, `packages` and `services`, which say only where the directories happen to sit. This is the
last item of the CGR-7/CGR-12 plan.

## What Changes

- **`package_manifests.cpp`** (new) reads the four manifests and expands their globs. Only the
  slice that names members is read: a `workspaces` array or `workspaces.packages` object, the
  `packages:` list of a pnpm workspace (including the inline `[a, b]` form), `members` and
  `exclude` of a Cargo `[workspace]` (arrays may span lines), and `use` directives of a `go.work`
  (bare or in a `use ( … )` block). Globs support a literal segment, `*` for one segment, `**`
  for any number, and a leading `!` to exclude. A matched directory is a package only when it
  declares a manifest of its own, and its name comes from that manifest (`name`, Cargo's
  `[package] name`, go.mod's `module`), else the directory name. The walk reuses the project
  scanners' skip list, so a `**` glob cannot descend into `node_modules`, and is bounded to five
  levels and 512 packages.
- **`report modules` groups by package when one is declared.** A file inside a package joins it;
  a file outside every package keeps its directory name, so nothing is dropped. The response
  carries `group_by` (`packages` or `depth`), the `manifest` that supplied them and the `packages`
  count, and the Markdown and Mermaid captions say "12 packages from package.json" instead of
  "depth 2" — a reader has to know which question the diagram answered.
- **`group_by` selects explicitly**: `auto` (default: packages when the manifest declares any,
  else depth), `packages`, `depth`. Surfaced as `--group-by` on `cgraph report modules` and
  `group_by` on the MCP `graph_report`.
- **No new node.** Grouping stays a report-time view, exactly as the plan requires: no `package`
  node enters `graph.json`, extraction is untouched, and a stale manifest costs nothing but a
  regrouped report.

### Non-goals
- Nx `project.json` and Bazel `BUILD` files: both name targets rather than directories, and no
  field repository here uses them.
- The dependency edges a manifest declares (`dependencies` between workspace packages). The
  module graph's edges stay the ones the code actually makes, which is the point of the view; a
  declared-but-unused dependency is a different report.
- Package version, private flag or publish config.
- Cross-repository packages: a workspace of repositories (CGR-14) groups each repository's own
  report; a package graph spanning repositories is not this change.

## Impact

- **Touches:** `src/engine/include/cgraph/package_manifests.hpp` and
  `src/engine/package_manifests.cpp` (new), `src/engine/report.cpp`,
  `src/engine/include/cgraph/report.hpp`, `src/engine/CMakeLists.txt`, `src/cli/main.cpp`,
  `src/mcp/mcp_server.cpp`, `tests/smoke/package_manifests_test.cpp` (new),
  `tests/smoke/report_test.cpp`, `tests/smoke/CMakeLists.txt`, docs.
- **A repository without a workspace manifest is unchanged**, including CGraph itself: `group_by`
  reports `depth` and every module name is what it was.
- `graph.json` and every export are untouched.
- **Measured** on four real monorepos (tasks.md 3.3).

## Capabilities

### Modified Capabilities
- `graph-daemon-client` — the `report` op's `modules` view groups by workspace package when the project declares one, and `group_by` selects.
- `host-integration-mcp` — `graph_report` exposes `group_by`.
