#include "cgraph/path_ignore.hpp"

#include <fstream>
#include <system_error>
#include <unordered_set>

namespace cgraph {

bool is_skipped_directory(std::string_view name) {
  static const std::unordered_set<std::string_view> skipped = {
      ".git",
      ".hg",
      ".svn",
      ".cache",
      ".idea",
      ".vscode",
      ".agents",
      "build",
      "cmake-build-debug",
      "cmake-build-release",
      "coverage",
      "dist",
      ".next",
      "node_modules",
      "target",
      "vendor",
      "cgraph-out",
      "graphify-out",
      // Test-runner output: Playwright writes its HTML report (with minified
      // trace-viewer bundles), its per-test artifacts, and its blob reports
      // here by default, and none of it is source. On a 1,261-file Next.js app
      // the seven bundles under playwright-report/ produced 237 of 1,349
      // `report design` entry points (nine of the top ten by reach) and 46 of
      // 216 clone classes, and were not covered by that repo's .gitignore.
      "playwright-report",
      "test-results",
      "blob-report",
      // Python ecosystem: virtualenvs, installed packages, and tool caches are
      // dependencies/generated state, never project source. `site-packages`
      // catches venv contents regardless of the venv directory's name.
      ".venv",
      "venv",
      "site-packages",
      "__pycache__",
      ".tox",
      ".nox",
      ".pytest_cache",
      ".mypy_cache",
      ".ruff_cache",
      ".hypothesis",
      ".eggs",
      // Agent-CLI and spec-tool config trees: tooling, not project source or
      // documentation (their docs/templates otherwise pollute enrichment plans).
      ".claude",
      ".codex",
      ".gemini",
      ".cursor",
      ".factory",
      ".opencode",
      ".windsurf",
      ".aider",
      ".specify",
  };
  return skipped.contains(name);
}

bool is_dependency_directory(const std::filesystem::path& dir) {
  if (is_skipped_directory(dir.filename().generic_string())) {
    return true;
  }
  std::error_code ec;
  // A virtualenv root carries a pyvenv.cfg marker; skip it even under a
  // non-standard name. Probed once per directory entered (not per file).
  if (std::filesystem::exists(dir / "pyvenv.cfg", ec)) {
    return true;
  }
  // Agent tools keep full duplicate worktree checkouts of the repo. Two markers,
  // because both live and stale checkouts must be skipped:
  //  - A LIVE linked worktree's `.git` is a regular FILE (`gitdir: …`), whereas
  //    the project root's `.git` is a directory. Catches worktrees parked
  //    anywhere, and never matches the root.
  if (std::filesystem::is_regular_file(dir / ".git", ec)) {
    return true;
  }
  //  - Tools park checkouts under `<.tool>/worktrees/<id>/` (`.anvil/worktrees/…`,
  //    `.agents/worktrees/…`). A `worktrees` directory under a DOTTED parent is
  //    that convention; skipping it drops STALE checkouts too (whose `.git` may
  //    already be pruned, leaving the source tree behind). The dotted-parent
  //    guard leaves a legitimate source `worktrees/` module untouched.
  const auto parent = dir.parent_path().filename().generic_string();
  return dir.filename().generic_string() == "worktrees" && !parent.empty() && parent.front() == '.';
}

std::vector<std::string> read_root_gitignore(const std::filesystem::path& root) {
  std::vector<std::string> patterns;
  std::ifstream input(root / ".gitignore");
  std::string line;
  while (std::getline(input, line)) {
    line.erase(line.find_last_not_of(" \t\r\n") + 1);
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
      continue;
    }
    line.erase(0, first);
    if (line.empty() || line[0] == '#' || line[0] == '!') {
      continue;
    }
    if (!line.empty() && line.back() == '/') {
      line.pop_back();
    }
    if (!line.empty()) {
      patterns.push_back(line);
    }
  }
  return patterns;
}

bool matches_simple_gitignore(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::vector<std::string>& patterns) {
  if (patterns.empty()) {
    return false;
  }

  const auto relative = std::filesystem::relative(path, root).generic_string();
  const auto filename = path.filename().generic_string();

  for (const auto& pattern : patterns) {
    if (pattern.empty()) {
      continue;
    }

    if (pattern.front() == '/') {
      if (relative == pattern.substr(1) || relative.starts_with(pattern.substr(1) + "/")) {
        return true;
      }
      continue;
    }

    if (pattern.find('/') == std::string::npos) {
      if (filename == pattern || relative == pattern || relative.starts_with(pattern + "/") ||
          relative.find("/" + pattern + "/") != std::string::npos) {
        return true;
      }
      continue;
    }

    if (relative == pattern || relative.starts_with(pattern + "/")) {
      return true;
    }
  }

  return false;
}

}  // namespace cgraph
