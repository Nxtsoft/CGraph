#include "cgraph/skills_install.hpp"

#include "cgraph/platform.hpp"

#include <cstdlib>
#include <system_error>

namespace cgraph {
namespace {

// The file whose presence marks a directory as the canonical skills source.
const std::filesystem::path kSourceMarker =
    std::filesystem::path("cgraph-enrich") / "SKILL.md";

[[nodiscard]] bool is_skills_source(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::is_regular_file(dir / kSourceMarker, ec);
}

// Walk from `start` to the filesystem root looking for integrations/skills.
[[nodiscard]] std::filesystem::path walk_up_for_source(std::filesystem::path start) {
  std::error_code ec;
  start = std::filesystem::weakly_canonical(start, ec);
  for (auto dir = start; !dir.empty(); dir = dir.parent_path()) {
    if (const auto candidate = dir / "integrations" / "skills"; is_skills_source(candidate)) {
      return candidate;
    }
    if (dir == dir.parent_path()) {
      break;
    }
  }
  return {};
}

// True when `target` is lexically inside `dir` once both are canonicalized.
[[nodiscard]] bool path_within(const std::filesystem::path& target, const std::filesystem::path& dir) {
  std::error_code ec;
  const auto canon_target = std::filesystem::weakly_canonical(target, ec);
  const auto canon_dir = std::filesystem::weakly_canonical(dir, ec);
  const auto mismatch = std::mismatch(canon_dir.begin(), canon_dir.end(), canon_target.begin(),
                                      canon_target.end());
  return mismatch.first == canon_dir.end();
}

[[nodiscard]] SkillLinkStatus inspect_link(const std::filesystem::path& link,
                                           const std::filesystem::path& expected_target) {
  SkillLinkStatus row;
  row.link = link;
  row.expected_target = expected_target;
  std::error_code ec;
  const auto st = std::filesystem::symlink_status(link, ec);
  if (ec || st.type() == std::filesystem::file_type::not_found) {
    row.state = SkillLinkState::kMissing;
    return row;
  }
  if (st.type() != std::filesystem::file_type::symlink) {
    row.state = SkillLinkState::kNotSymlink;
    return row;
  }
  row.actual_target = std::filesystem::read_symlink(link, ec);
  const auto resolved = std::filesystem::weakly_canonical(link, ec);
  const auto expected = std::filesystem::weakly_canonical(expected_target, ec);
  row.state = (!ec && resolved == expected) ? SkillLinkState::kOk : SkillLinkState::kWrongTarget;
  return row;
}

}  // namespace

std::vector<std::filesystem::path> default_host_skill_dirs() {
  std::vector<std::filesystem::path> dirs;
  const auto home_dir = home_directory();
  if (home_dir.empty()) {
    return dirs;
  }
  for (const auto& host : {home_dir / ".claude", home_dir / ".agents"}) {
    std::error_code ec;
    if (std::filesystem::is_directory(host, ec)) {
      dirs.push_back(host / "skills");
    }
  }
  return dirs;
}

std::filesystem::path resolve_skills_source(const std::filesystem::path& executable,
                                            const std::filesystem::path& cwd) {
  if (auto found = walk_up_for_source(executable.parent_path()); !found.empty()) {
    return found;
  }
  return walk_up_for_source(cwd);
}

std::vector<SkillLinkStatus> skills_status(const SkillsConfig& config) {
  std::vector<SkillLinkStatus> rows;
  rows.reserve(config.host_skill_dirs.size() * kHostSkillNames.size());
  for (const auto& host_dir : config.host_skill_dirs) {
    for (const auto name : kHostSkillNames) {
      rows.push_back(inspect_link(host_dir / name, config.skills_source_dir / name));
    }
  }
  return rows;
}

bool skills_install(const SkillsConfig& config) {
  bool all_ok = true;
  for (const auto& host_dir : config.host_skill_dirs) {
    std::error_code ec;
    std::filesystem::create_directories(host_dir, ec);
    for (const auto name : kHostSkillNames) {
      const auto row = inspect_link(host_dir / name, config.skills_source_dir / name);
      if (row.state == SkillLinkState::kOk) {
        continue;
      }
      if (row.state != SkillLinkState::kMissing) {
        all_ok = false;  // occupied by something we don't own; never clobber
        continue;
      }
      std::filesystem::create_directory_symlink(row.expected_target, row.link, ec);
      if (ec || inspect_link(row.link, row.expected_target).state != SkillLinkState::kOk) {
        all_ok = false;
      }
    }
  }
  return all_ok;
}

bool skills_uninstall(const SkillsConfig& config) {
  bool all_clear = true;
  for (const auto& row : skills_status(config)) {
    if (row.state == SkillLinkState::kMissing) {
      continue;
    }
    const bool owned = (row.state == SkillLinkState::kOk) ||
                       (row.state == SkillLinkState::kWrongTarget &&
                        path_within(row.actual_target, config.skills_source_dir));
    if (!owned) {
      all_clear = false;  // foreign link or real dir: report, don't touch
      continue;
    }
    std::error_code ec;
    std::filesystem::remove(row.link, ec);
    if (ec) {
      all_clear = false;
    }
  }
  return all_clear;
}

}  // namespace cgraph
