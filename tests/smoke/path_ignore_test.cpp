#include "cgraph/path_ignore.hpp"

#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;

void write_file(const fs::path& path, std::string contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

// is_skipped_directory covers the always-skip vendor/build/VCS set regardless of
// any .gitignore.
int test_skipped_directory() {
  if (!cgraph::is_skipped_directory("node_modules") || !cgraph::is_skipped_directory(".git") ||
      !cgraph::is_skipped_directory("build") || !cgraph::is_skipped_directory("cgraph-out")) {
    return 1;
  }
  // Generated/scratch dirs that escape a root-anchored .gitignore (nested .next in
  // a worktree, agent worktree checkouts under .agents/) must skip unconditionally.
  if (!cgraph::is_skipped_directory(".next") || !cgraph::is_skipped_directory(".agents") ||
      !cgraph::is_skipped_directory("coverage")) {
    return 1;
  }
  // Python ecosystem dependency/tooling trees: a virtualenv or its site-packages
  // is not project source. `site-packages` catches venv contents regardless of the
  // venv directory's name.
  for (const auto* name : {".venv", "venv", "site-packages", "__pycache__", ".tox", ".nox",
                           ".pytest_cache", ".mypy_cache", ".ruff_cache", ".hypothesis", ".eggs"}) {
    if (!cgraph::is_skipped_directory(name)) {
      return 1;
    }
  }
  // Test-runner output trees (Playwright's report, per-test artifacts, blob
  // reports) are generated, not source: the minified trace-viewer bundles in
  // playwright-report/ otherwise dominate the design and clones reports.
  for (const auto* name : {"playwright-report", "test-results", "blob-report"}) {
    if (!cgraph::is_skipped_directory(name)) {
      return 1;
    }
  }
  // Agent-CLI / spec-tool config dirs are tooling, not project source or docs.
  for (const auto* name : {".claude", ".codex", ".gemini", ".cursor", ".factory", ".opencode",
                           ".windsurf", ".aider", ".specify"}) {
    if (!cgraph::is_skipped_directory(name)) {
      return 1;
    }
  }
  // A normal source directory must not be skipped — including names that merely
  // contain a skipped token, bare `env` (a common config dir; oddly-named venvs
  // are caught structurally by their pyvenv.cfg marker instead), and the
  // non-dotted lookalikes of the agent-tooling names (a real `factory/` package).
  if (cgraph::is_skipped_directory("src") || cgraph::is_skipped_directory("docs") ||
      cgraph::is_skipped_directory("env") || cgraph::is_skipped_directory("my_env_utils") ||
      cgraph::is_skipped_directory("environment") || cgraph::is_skipped_directory("factory") ||
      cgraph::is_skipped_directory("claude") || cgraph::is_skipped_directory("specify") ||
      cgraph::is_skipped_directory("tests") || cgraph::is_skipped_directory("test-utils") ||
      cgraph::is_skipped_directory("playwright")) {
    return 1;  // the runner's source-side dirs and lookalikes are not its output dirs
  }
  return 0;
}

// is_dependency_directory adds a structural check on top of the name list: a
// directory containing a pyvenv.cfg is a virtualenv root and is skipped even when
// its name is not listed.
int test_dependency_directory_marker() {
  const auto root = fs::temp_directory_path() / "cgraph_path_ignore_marker";
  fs::remove_all(root);
  fs::create_directories(root);

  // Oddly-named venv: not in the name list, but has the marker -> skipped.
  write_file(root / "qa-env" / "pyvenv.cfg", "home = /usr/bin\n");
  // Lookalike: a real source dir with no marker -> not skipped.
  fs::create_directories(root / "my_env_utils");
  write_file(root / "my_env_utils" / "helper.py", "def f():\n    return 1\n");
  // Listed name is skipped structurally too, marker or not.
  fs::create_directories(root / ".venv");

  int rc = 0;
  rc |= !cgraph::is_dependency_directory(root / "qa-env");        // marker -> skip
  rc |= !cgraph::is_dependency_directory(root / ".venv");         // listed name -> skip
  rc |= cgraph::is_dependency_directory(root / "my_env_utils");   // neither -> keep

  fs::remove_all(root);
  return rc ? 1 : 0;
}

// A linked git worktree checkout is a full duplicate of the repo; its `.git` is
// a regular FILE (a `gitdir:` pointer), whereas a real repo root's `.git` is a
// directory. is_dependency_directory skips the worktree structurally — the
// checkout dirs are uuid/slug-named under a tool-specific parent (`.anvil`,
// `.agents`, …), so the name list cannot catch them — while never skipping the
// project root.
int test_dependency_directory_worktree() {
  const auto root = fs::temp_directory_path() / "cgraph_path_ignore_worktree";
  fs::remove_all(root);
  fs::create_directories(root);

  // Live linked worktree: `.git` is a regular file with a gitdir pointer -> skip.
  write_file(root / ".anvil" / "worktrees" / "wt1" / ".git",
             "gitdir: /repo/.git/worktrees/wt1\n");
  write_file(root / ".anvil" / "worktrees" / "wt1" / "src" / "a.ts", "export const a = 1;\n");
  // The `<.tool>/worktrees` convention dir itself -> skip (so stale checkouts
  // under it, whose `.git` may be pruned, never get walked).
  // Real repo root: `.git` is a directory -> NOT skipped.
  fs::create_directories(root / "realrepo" / ".git");
  write_file(root / "realrepo" / "index.ts", "export const b = 2;\n");
  // No `.git` at all -> NOT skipped.
  fs::create_directories(root / "plain");
  // A legitimate source module literally named `worktrees` under a NON-dotted
  // parent -> NOT skipped (the dotted-parent guard protects it).
  write_file(root / "src" / "worktrees" / "manager.ts", "export const c = 3;\n");

  int rc = 0;
  rc |= !cgraph::is_dependency_directory(root / ".anvil" / "worktrees" / "wt1");  // .git file    -> skip
  rc |= !cgraph::is_dependency_directory(root / ".anvil" / "worktrees");          // dotted/worktrees -> skip
  rc |= cgraph::is_dependency_directory(root / "realrepo");                       // .git dir     -> keep
  rc |= cgraph::is_dependency_directory(root / "plain");                          // no .git      -> keep
  rc |= cgraph::is_dependency_directory(root / "src" / "worktrees");              // real module  -> keep

  fs::remove_all(root);
  return rc ? 1 : 0;
}

// The regression this module exists to prevent: a root .gitignore that ignores a
// vendored toolchain (the repo's own `/.vcpkg/`) must cause that directory AND
// its contents to match, so every project scanner skips it the same way.
int test_gitignore_matches_vendored_dir() {
  const auto root = fs::temp_directory_path() / "cgraph_path_ignore_test";
  fs::remove_all(root);
  fs::create_directories(root);

  // Mirror this repo's .gitignore shapes: anchored (`/.vcpkg/`, `/build/`) and
  // a bare-name pattern (`vcpkg_installed`) matched at any depth. Comments,
  // blanks, and negations must be ignored by the parser.
  write_file(root / ".gitignore",
             "# comment\n"
             "\n"
             "/build/\n"
             "/.vcpkg/\n"
             "vcpkg_installed/\n"
             "!keep.me\n");

  const auto patterns = cgraph::read_root_gitignore(root);
  // 3 real patterns; comment/blank/negation dropped.
  if (patterns.size() != 3) {
    fs::remove_all(root);
    return 1;
  }

  write_file(root / ".vcpkg" / "ports" / "igraph" / "README.md", "vendored doc\n");
  write_file(root / "build" / "default" / "x.txt", "build artifact\n");
  write_file(root / "src" / "deep" / "vcpkg_installed" / "lib.txt", "nested vendored\n");
  write_file(root / "docs" / "real.md", "real doc\n");

  int rc = 0;
  // Anchored dir + its contents match.
  rc |= !cgraph::matches_simple_gitignore(root, root / ".vcpkg", patterns);
  rc |= !cgraph::matches_simple_gitignore(root, root / ".vcpkg" / "ports" / "igraph" / "README.md", patterns);
  rc |= !cgraph::matches_simple_gitignore(root, root / "build", patterns);
  // Bare-name pattern matches at depth.
  rc |= !cgraph::matches_simple_gitignore(root, root / "src" / "deep" / "vcpkg_installed", patterns);
  // A real source/doc path must NOT match — otherwise we'd skip the project itself.
  rc |= cgraph::matches_simple_gitignore(root, root / "docs" / "real.md", patterns);
  rc |= cgraph::matches_simple_gitignore(root, root / "src", patterns);

  fs::remove_all(root);
  return rc ? 1 : 0;
}

// No .gitignore -> no patterns -> nothing matches (scanner falls back to the
// always-skip directory set only).
int test_empty_gitignore() {
  const auto root = fs::temp_directory_path() / "cgraph_path_ignore_empty";
  fs::remove_all(root);
  fs::create_directories(root);
  const auto patterns = cgraph::read_root_gitignore(root);
  const int rc = (!patterns.empty() ||
                  cgraph::matches_simple_gitignore(root, root / "anything.md", patterns))
                     ? 1
                     : 0;
  fs::remove_all(root);
  return rc;
}

}  // namespace

int main() {
  return test_skipped_directory() || test_dependency_directory_marker() ||
         test_dependency_directory_worktree() || test_gitignore_matches_vendored_dir() ||
         test_empty_gitignore();
}
