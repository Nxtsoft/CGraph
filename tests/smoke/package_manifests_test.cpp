#include "cgraph/package_manifests.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

int fail(std::string_view message) {
  std::cerr << "package_manifests_test: " << message << '\n';
  return 1;
}

void write_file(const fs::path& path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << contents;
}

std::vector<std::string> names_of(const std::vector<cgraph::WorkspacePackage>& packages) {
  std::vector<std::string> names;
  names.reserve(packages.size());
  for (const auto& package : packages) {
    names.push_back(package.name);
  }
  std::ranges::sort(names);
  return names;
}

// npm/yarn/bun: a `workspaces` array of globs, the package's own name from its
// package.json, a glob hit with no manifest ignored, and node_modules skipped.
int test_npm_workspaces(const fs::path& root) {
  const auto repo = root / "npm";
  write_file(repo / "package.json", R"({"name": "cybertron", "workspaces": ["apps/*", "packages/*", "scripts"]})");
  write_file(repo / "apps" / "web" / "package.json", R"({"name": "@turing/web"})");
  write_file(repo / "apps" / "api" / "package.json", R"({"name": "@turing/api"})");
  write_file(repo / "packages" / "ui" / "package.json", R"({"name": "ui-kit"})");
  write_file(repo / "packages" / "no-manifest" / "index.ts", "export const x = 1;\n");
  write_file(repo / "scripts" / "package.json", R"({"version": "1.0.0"})");  // unnamed: the directory names it
  write_file(repo / "node_modules" / "left-pad" / "package.json", R"({"name": "left-pad"})");
  write_file(repo / "apps" / "web" / "node_modules" / "dep" / "package.json", R"({"name": "dep"})");

  const auto packages = cgraph::discover_workspace_packages(repo);
  if (names_of(packages) != std::vector<std::string>{"@turing/api", "@turing/web", "scripts", "ui-kit"}) {
    for (const auto& package : packages) {
      std::cerr << "  " << package.name << " " << package.root.generic_string() << '\n';
    }
    return fail("npm workspaces: four packages, named by their own manifest or directory, no node_modules");
  }
  if (packages.front().manifest != "package.json") {
    return fail("npm workspaces: the manifest is named");
  }
  // A file lands in the package that contains it; one outside every package has none.
  const auto* web = cgraph::package_for_file(packages, repo / "apps" / "web" / "src" / "index.tsx");
  const auto* ui = cgraph::package_for_file(packages, repo / "packages" / "ui" / "button.tsx");
  if (web == nullptr || web->name != "@turing/web" || ui == nullptr || ui->name != "ui-kit") {
    return fail("npm workspaces: a file belongs to the package whose directory holds it");
  }
  if (cgraph::package_for_file(packages, repo / "tools" / "build.ts") != nullptr ||
      cgraph::package_for_file(packages, repo / "packages" / "no-manifest" / "index.ts") != nullptr) {
    return fail("a file outside every package, or in a glob hit that declares no manifest, belongs to none");
  }
  return 0;
}

// pnpm: the `packages:` list, an exclusion, and the `workspaces.packages`
// object form of npm.
int test_pnpm_and_object_form(const fs::path& root) {
  const auto repo = root / "pnpm";
  write_file(repo / "pnpm-workspace.yaml", R"(# the workspace
packages:
  - 'apps/*'
  - "libs/**"
  - '!libs/private'
onlyBuiltDependencies:
  - esbuild
)");
  write_file(repo / "apps" / "site" / "package.json", R"({"name": "site"})");
  write_file(repo / "libs" / "deep" / "core" / "package.json", R"({"name": "core"})");
  write_file(repo / "libs" / "private" / "package.json", R"({"name": "private"})");
  const auto packages = cgraph::discover_workspace_packages(repo);
  if (names_of(packages) != std::vector<std::string>{"core", "site"} ||
      packages.front().manifest != "pnpm-workspace.yaml") {
    for (const auto& package : packages) {
      std::cerr << "  " << package.name << '\n';
    }
    return fail("pnpm: `**` reaches a nested package, `!` excludes, and a later key ends the list");
  }

  const auto object_form = root / "npm-object";
  write_file(object_form / "package.json", R"({"workspaces": {"packages": ["mods/*"], "nohoist": ["x"]}})");
  write_file(object_form / "mods" / "one" / "package.json", R"({"name": "one"})");
  if (names_of(cgraph::discover_workspace_packages(object_form)) != std::vector<std::string>{"one"}) {
    return fail("npm: the object form of `workspaces` is read");
  }
  return 0;
}

// Cargo: a multi-line `members` array with an `exclude`, names from each
// crate's `[package] name`. go.work: `use` lines and a `use ( … )` block.
int test_cargo_and_go(const fs::path& root) {
  const auto repo = root / "cargo";
  write_file(repo / "Cargo.toml", R"([workspace]
resolver = "2"
members = [
  "crates/*",
  "tools/cli",
]
exclude = ["crates/experimental"]

[workspace.dependencies]
serde = "1"
)");
  write_file(repo / "crates" / "core" / "Cargo.toml", "[package]\nname = \"ripgrep-core\"\nversion = \"0.1.0\"\n");
  write_file(repo / "crates" / "experimental" / "Cargo.toml", "[package]\nname = \"experimental\"\n");
  write_file(repo / "tools" / "cli" / "Cargo.toml", "[package]\nname = \"rg\"\n");
  const auto crates = cgraph::discover_workspace_packages(repo);
  if (names_of(crates) != std::vector<std::string>{"rg", "ripgrep-core"} || crates.front().manifest != "Cargo.toml") {
    for (const auto& package : crates) {
      std::cerr << "  " << package.name << '\n';
    }
    return fail("cargo: members across lines, an excluded crate left out, names from [package]");
  }

  const auto go = root / "go";
  write_file(go / "go.work", R"(go 1.22

use ./gateway // the edge
use (
	./service/orders
	./service/billing
)
)");
  write_file(go / "gateway" / "go.mod", "module github.com/x/gateway\n\ngo 1.22\n");
  write_file(go / "service" / "orders" / "go.mod", "module github.com/x/orders\n");
  write_file(go / "service" / "billing" / "go.mod", "module github.com/x/billing\n");
  const auto modules = cgraph::discover_workspace_packages(go);
  if (names_of(modules) != std::vector<std::string>{"github.com/x/billing", "github.com/x/gateway", "github.com/x/orders"} ||
      modules.front().manifest != "go.work") {
    for (const auto& package : modules) {
      std::cerr << "  " << package.name << '\n';
    }
    return fail("go.work: a bare `use` with a comment and a `use ( … )` block, names from go.mod");
  }
  return 0;
}

// Anything that is not a workspace yields nothing, so the caller falls back to
// directory depth; a nested package wins over the one containing it.
int test_no_workspace_and_nesting(const fs::path& root) {
  const auto plain = root / "plain";
  write_file(plain / "package.json", R"({"name": "single", "version": "1.0.0"})");
  write_file(plain / "src" / "index.ts", "export const x = 1;\n");
  if (!cgraph::discover_workspace_packages(plain).empty()) {
    return fail("a package.json without `workspaces` declares no workspace");
  }
  if (!cgraph::discover_workspace_packages(root / "does-not-exist").empty()) {
    return fail("a root that does not exist declares no workspace");
  }
  const auto broken = root / "broken";
  write_file(broken / "package.json", "{not json");
  if (!cgraph::discover_workspace_packages(broken).empty()) {
    return fail("a manifest that cannot be parsed declares no workspace");
  }
  const auto empty = root / "empty-globs";
  write_file(empty / "package.json", R"({"workspaces": ["apps/*"]})");
  if (!cgraph::discover_workspace_packages(empty).empty()) {
    return fail("globs matching nothing declare no packages");
  }

  const auto nested = root / "nested";
  write_file(nested / "package.json", R"({"workspaces": ["apps/*", "apps/*/plugins/*"]})");
  write_file(nested / "apps" / "shell" / "package.json", R"({"name": "shell"})");
  write_file(nested / "apps" / "shell" / "plugins" / "auth" / "package.json", R"({"name": "shell-auth"})");
  const auto packages = cgraph::discover_workspace_packages(nested);
  if (names_of(packages) != std::vector<std::string>{"shell", "shell-auth"}) {
    return fail("a package nested inside another is its own package");
  }
  const auto* owner = cgraph::package_for_file(packages, nested / "apps" / "shell" / "plugins" / "auth" / "index.ts");
  if (owner == nullptr || owner->name != "shell-auth") {
    return fail("the innermost package owns the file");
  }
  const auto* outer = cgraph::package_for_file(packages, nested / "apps" / "shell" / "main.ts");
  if (outer == nullptr || outer->name != "shell") {
    return fail("a file outside the nested package still belongs to the outer one");
  }
  return 0;
}

}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / "cgraph-package-manifests-test";
  fs::remove_all(root);
  int failures = 0;
  failures += test_npm_workspaces(root);
  failures += test_pnpm_and_object_form(root);
  failures += test_cargo_and_go(root);
  failures += test_no_workspace_and_nesting(root);
  fs::remove_all(root);
  return failures == 0 ? 0 : 1;
}
