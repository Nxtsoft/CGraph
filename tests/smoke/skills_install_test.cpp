#include "cgraph/skills_install.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;

void expect(bool& ok, bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

void write_file(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

}  // namespace

// Exercises the skills install/status/uninstall lifecycle against a real temp
// tree: source resolution walks up to integrations/skills, install creates
// exactly the expected symlinks and is idempotent, occupied paths are never
// clobbered, and uninstall removes only links pointing into this repo's source.
int main() {
  bool ok = true;

  // Canonicalize so equality checks against resolver output (which canonicalizes,
  // e.g. macOS /var -> /private/var) compare like with like.
  const auto base = fs::weakly_canonical(fs::temp_directory_path()) / "cgraph_skills_install_test";
  fs::remove_all(base);

  // Fake repo with the canonical skills, and two fake host skill dirs.
  const auto repo = base / "repo";
  const auto source = repo / "integrations" / "skills";
  for (const auto name : cgraph::kHostSkillNames) {
    write_file(source / name / "SKILL.md", std::string("# ") + std::string(name) + "\n");
  }
  const auto host_claude = base / "home" / ".claude" / "skills";
  const auto host_agents = base / "home" / ".agents" / "skills";

  // Source resolution: from a nested executable dir and from a nested cwd.
  const auto exe = repo / "build" / "default" / "src" / "cli" / "cgraph";
  write_file(exe, "");
  expect(ok, cgraph::resolve_skills_source(exe, base) == source,
         "resolve_skills_source finds the source from the executable path");
  expect(ok, cgraph::resolve_skills_source(base / "nowhere" / "cgraph", repo / "src") == source,
         "resolve_skills_source falls back to walking up from cwd");
  expect(ok, cgraph::resolve_skills_source(base / "nowhere" / "cgraph", base).empty(),
         "resolve_skills_source returns empty when no ancestor has the source");

  cgraph::SkillsConfig config;
  config.skills_source_dir = source;
  config.host_skill_dirs = {host_claude, host_agents};

  // Status before install: every row missing.
  auto rows = cgraph::skills_status(config);
  expect(ok, rows.size() == 4, "status has one row per host dir x skill");
  for (const auto& row : rows) {
    expect(ok, row.state == cgraph::SkillLinkState::kMissing, "pre-install rows are missing");
  }

  // Install creates the links (host dirs made on demand) and is idempotent.
  expect(ok, cgraph::skills_install(config), "install succeeds on a clean tree");
  rows = cgraph::skills_status(config);
  for (const auto& row : rows) {
    expect(ok, row.state == cgraph::SkillLinkState::kOk, "post-install rows are ok");
    expect(ok, fs::is_regular_file(row.link / "SKILL.md"), "link resolves to a real SKILL.md");
  }
  expect(ok, cgraph::skills_install(config), "re-install is a no-op success");

  // An edit to the canonical source is visible through the link (symlink, not copy).
  write_file(source / "cgraph-enrich" / "SKILL.md", "# edited\n");
  {
    std::ifstream in(host_claude / "cgraph-enrich" / "SKILL.md");
    std::string first_line;
    std::getline(in, first_line);
    expect(ok, first_line == "# edited", "source edits are live through the symlink");
  }

  // Occupied paths are never clobbered: a real dir and a foreign symlink both
  // fail install and survive untouched.
  fs::remove(host_claude / "cgraph");
  fs::create_directories(host_claude / "cgraph");
  write_file(base / "foreign" / "SKILL.md", "# foreign\n");
  fs::remove(host_agents / "cgraph-enrich");
  fs::create_directory_symlink(base / "foreign", host_agents / "cgraph-enrich");
  expect(ok, !cgraph::skills_install(config), "install reports failure on occupied paths");
  expect(ok, fs::is_directory(host_claude / "cgraph") &&
                 !fs::is_symlink(host_claude / "cgraph"),
         "real dir is not replaced");
  expect(ok, fs::read_symlink(host_agents / "cgraph-enrich") == base / "foreign",
         "foreign symlink is not replaced");
  rows = cgraph::skills_status(config);
  int not_symlink = 0;
  int wrong_target = 0;
  for (const auto& row : rows) {
    not_symlink += row.state == cgraph::SkillLinkState::kNotSymlink ? 1 : 0;
    wrong_target += row.state == cgraph::SkillLinkState::kWrongTarget ? 1 : 0;
  }
  expect(ok, not_symlink == 1 && wrong_target == 1, "status classifies occupied paths");

  // Uninstall removes only links owned by this source; the real dir and the
  // foreign link remain and are reported.
  expect(ok, !cgraph::skills_uninstall(config), "uninstall reports the unowned leftovers");
  expect(ok, !fs::exists(fs::symlink_status(host_agents / "cgraph")) &&
                 !fs::exists(fs::symlink_status(host_claude / "cgraph-enrich")),
         "owned links are removed");
  expect(ok, fs::is_directory(host_claude / "cgraph"), "real dir survives uninstall");
  expect(ok, fs::is_symlink(host_agents / "cgraph-enrich"), "foreign link survives uninstall");

  // A wrong-target link that still points inside the configured source (e.g.
  // left behind by an older layout) is owned and removed.
  fs::remove(host_agents / "cgraph-enrich");
  fs::create_directory_symlink(source / "cgraph", host_agents / "cgraph-enrich");
  expect(ok, !cgraph::skills_uninstall(config), "real-dir leftover still reported");
  expect(ok, !fs::exists(fs::symlink_status(host_agents / "cgraph-enrich")),
         "owned link is removed on the second uninstall");

  fs::remove_all(base);
  if (!ok) {
    return 1;
  }
  std::cout << "skills install lifecycle ok\n";
  return 0;
}
