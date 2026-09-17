#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Workspace packages (CGR-15): the units a monorepo already names for itself.
//
// `report modules` groups files by directory depth, which is a guess. A monorepo
// has already answered the question in its build manifest: npm/yarn/bun
// `workspaces`, `pnpm-workspace.yaml`, Cargo `[workspace] members`, or `go.work`
// `use`. Reading it makes the module map say `@turing/api` and `ui-kit` -- the
// names the team says out loud -- instead of `apps`, `packages` and `services`.
//
// This is a report-time view, exactly like directory grouping: no `package` node
// is added to `graph.json`, so Graphify parity is untouched and a stale manifest
// costs nothing but a regrouped report.
//
// Only the slice of each format that names members is read: a `workspaces` array
// or `workspaces.packages`, the `packages:` list of a pnpm workspace, the
// `members` and `exclude` arrays of a Cargo `[workspace]`, and the `use`
// directives of a `go.work`. Globs support a literal segment, `*` for one
// segment and `**` for any number, with a leading `!` excluding.
namespace cgraph {

struct WorkspacePackage {
  std::string name;                  // the manifest's own `name`, else the directory name
  std::filesystem::path root;        // absolute package directory
  std::string manifest;              // the file that declared the workspace
};

// The packages the project at `root` declares, sorted by descending path depth
// so the most specific package owns a file, then by name. Empty when the root
// declares no workspace, when its manifest lists no member that exists, or when
// the manifest cannot be read.
[[nodiscard]] std::vector<WorkspacePackage> discover_workspace_packages(const std::filesystem::path& root);

// The package whose directory contains `source_file`, or nullptr. `packages`
// must be in the order `discover_workspace_packages` returns.
[[nodiscard]] const WorkspacePackage* package_for_file(
    std::span<const WorkspacePackage> packages, const std::filesystem::path& source_file);

}  // namespace cgraph
