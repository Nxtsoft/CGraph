#include "cgraph/package_manifests.hpp"

#include "cgraph/path_ignore.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace cgraph {
namespace {

namespace fs = std::filesystem;

// How deep a `**` or `*` glob is expanded below the workspace root. Package
// directories sit one or two levels down in every layout seen in the wild
// (`apps/*`, `packages/*/`, `crates/*`); the bound keeps a `**` pattern from
// walking a whole checkout.
constexpr int kMaxGlobDepth = 5;
// A guard against a manifest that globs the world.
constexpr std::size_t kMaxPackages = 512;

[[nodiscard]] std::string trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  const auto end = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(begin, end - begin + 1));
}

// Strips one layer of matching single or double quotes.
[[nodiscard]] std::string unquote(std::string text) {
  if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) {
    return text.substr(1, text.size() - 2);
  }
  return text;
}

[[nodiscard]] std::vector<std::string> split_segments(std::string_view path) {
  std::vector<std::string> segments;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto piece = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (!piece.empty() && piece != ".") {
      segments.emplace_back(piece);
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return segments;
}

// Glob match over path segments: a literal matches itself, `*` matches one
// segment, `**` matches any number (including none).
[[nodiscard]] bool segments_match(std::span<const std::string> pattern, std::span<const std::string> path) {
  if (pattern.empty()) {
    return path.empty();
  }
  if (pattern.front() == "**") {
    for (std::size_t skip = 0; skip <= path.size(); ++skip) {
      if (segments_match(pattern.subspan(1), path.subspan(skip))) {
        return true;
      }
    }
    return false;
  }
  if (path.empty()) {
    return false;
  }
  if (pattern.front() != "*" && pattern.front() != path.front()) {
    return false;
  }
  return segments_match(pattern.subspan(1), path.subspan(1));
}

struct Pattern {
  std::vector<std::string> segments;
  bool exclude = false;
};

[[nodiscard]] Pattern parse_pattern(std::string text) {
  Pattern pattern;
  text = unquote(trim(text));
  if (!text.empty() && text.front() == '!') {
    pattern.exclude = true;
    text.erase(0, 1);
  }
  if (text.starts_with("./")) {
    text.erase(0, 2);
  }
  while (!text.empty() && text.back() == '/') {
    text.pop_back();
  }
  pattern.segments = split_segments(text);
  return pattern;
}

// The deepest level any pattern needs, so the walk stops there.
[[nodiscard]] int walk_depth_for(const std::vector<Pattern>& patterns) {
  int depth = 1;
  for (const auto& pattern : patterns) {
    if (std::ranges::find(pattern.segments, "**") != pattern.segments.end()) {
      return kMaxGlobDepth;
    }
    depth = std::max(depth, static_cast<int>(pattern.segments.size()));
  }
  return std::min(depth, kMaxGlobDepth);
}

// Every directory under `root` to `max_depth`, root-relative, skipping the
// directories the project scanners already skip (VCS, build output, vendored
// dependency trees) so a manifest globbing `**` cannot walk node_modules.
void collect_directories(const fs::path& root, const fs::path& current, int depth, int max_depth,
                         std::vector<std::vector<std::string>>& out) {
  if (depth > max_depth) {
    return;
  }
  std::error_code error;
  fs::directory_iterator entries(current, error);
  if (error) {
    return;
  }
  for (const auto& entry : entries) {
    if (!entry.is_directory(error) || error) {
      continue;
    }
    const auto name = entry.path().filename().generic_string();
    if (name.empty() || name.front() == '.' || is_skipped_directory(name)) {
      continue;
    }
    const auto relative = entry.path().lexically_relative(root).generic_string();
    out.push_back(split_segments(relative));
    collect_directories(root, entry.path(), depth + 1, max_depth, out);
  }
}

// The `name` a package directory declares, or empty when it declares no
// manifest of its own (and so is not a package, only a directory a glob hit).
[[nodiscard]] bool package_name_at(const fs::path& directory, std::string& name) {
  std::error_code error;
  if (const auto package_json = directory / "package.json"; fs::is_regular_file(package_json, error)) {
    std::ifstream input(package_json);
    try {
      nlohmann::json manifest;
      input >> manifest;
      name = manifest.is_object() ? manifest.value("name", std::string{}) : std::string{};
    } catch (const nlohmann::json::exception&) {
      name.clear();
    }
    if (name.empty()) {
      name = directory.filename().generic_string();
    }
    return true;
  }
  if (const auto cargo = directory / "Cargo.toml"; fs::is_regular_file(cargo, error)) {
    std::ifstream input(cargo);
    std::string line;
    bool in_package = false;
    while (std::getline(input, line)) {
      const auto text = trim(line);
      if (text.starts_with("[")) {
        in_package = text.starts_with("[package]");
        continue;
      }
      if (in_package && text.starts_with("name")) {
        if (const auto equals = text.find('='); equals != std::string::npos) {
          name = unquote(trim(text.substr(equals + 1)));
        }
      }
    }
    if (name.empty()) {
      name = directory.filename().generic_string();
    }
    return true;
  }
  if (const auto go_mod = directory / "go.mod"; fs::is_regular_file(go_mod, error)) {
    std::ifstream input(go_mod);
    std::string line;
    while (std::getline(input, line)) {
      const auto text = trim(line);
      if (text.starts_with("module ")) {
        name = trim(text.substr(7));
        break;
      }
    }
    if (name.empty()) {
      name = directory.filename().generic_string();
    }
    return true;
  }
  return false;
}

// ---- the four manifest readers ---------------------------------------------

// npm / yarn / bun: `"workspaces": ["apps/*"]` or `{"packages": [...]}`.
[[nodiscard]] bool read_npm_workspaces(const fs::path& root, std::vector<Pattern>& patterns, std::string& manifest) {
  std::error_code error;
  const auto path = root / "package.json";
  if (!fs::is_regular_file(path, error)) {
    return false;
  }
  nlohmann::json document;
  try {
    std::ifstream input(path);
    input >> document;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
  if (!document.is_object()) {
    return false;
  }
  const auto workspaces = document.find("workspaces");
  if (workspaces == document.end()) {
    return false;
  }
  const nlohmann::json* globs = nullptr;
  if (workspaces->is_array()) {
    globs = &*workspaces;
  } else if (workspaces->is_object()) {
    if (const auto packages = workspaces->find("packages"); packages != workspaces->end() && packages->is_array()) {
      globs = &*packages;
    }
  }
  if (globs == nullptr) {
    return false;
  }
  for (const auto& glob : *globs) {
    if (glob.is_string()) {
      patterns.push_back(parse_pattern(glob.get<std::string>()));
    }
  }
  manifest = "package.json";
  return !patterns.empty();
}

// pnpm: the `packages:` list of `pnpm-workspace.yaml`. Only that key is read --
// a `- glob` item per line, until the next top-level key.
[[nodiscard]] bool read_pnpm_workspace(const fs::path& root, std::vector<Pattern>& patterns, std::string& manifest) {
  std::error_code error;
  const auto path = root / "pnpm-workspace.yaml";
  if (!fs::is_regular_file(path, error)) {
    return false;
  }
  std::ifstream input(path);
  std::string line;
  bool in_packages = false;
  while (std::getline(input, line)) {
    const auto text = trim(line);
    if (text.empty() || text.front() == '#') {
      continue;
    }
    if (!line.empty() && line.front() != ' ' && line.front() != '\t' && line.front() != '-') {
      in_packages = text.starts_with("packages:");
      // `packages: ["a", "b"]` on one line.
      if (in_packages) {
        const auto inline_list = text.substr(text.find(':') + 1);
        if (const auto open = inline_list.find('['); open != std::string::npos) {
          std::stringstream items(inline_list.substr(open + 1, inline_list.rfind(']') - open - 1));
          std::string item;
          while (std::getline(items, item, ',')) {
            if (const auto parsed = parse_pattern(item); !parsed.segments.empty()) {
              patterns.push_back(parsed);
            }
          }
          in_packages = false;
        }
      }
      continue;
    }
    if (in_packages && text.starts_with("-")) {
      if (const auto parsed = parse_pattern(text.substr(1)); !parsed.segments.empty()) {
        patterns.push_back(parsed);
      }
    }
  }
  manifest = "pnpm-workspace.yaml";
  return !patterns.empty();
}

// Cargo: `members` and `exclude` of the `[workspace]` table, one array that may
// span lines.
[[nodiscard]] bool read_cargo_workspace(const fs::path& root, std::vector<Pattern>& patterns, std::string& manifest) {
  std::error_code error;
  const auto path = root / "Cargo.toml";
  if (!fs::is_regular_file(path, error)) {
    return false;
  }
  std::ifstream input(path);
  std::string line;
  bool in_workspace = false;
  bool exclude = false;
  bool in_array = false;
  const auto take = [&](std::string_view body) {
    std::stringstream items{std::string(body)};
    std::string item;
    while (std::getline(items, item, ',')) {
      auto parsed = parse_pattern(item);
      if (parsed.segments.empty()) {
        continue;
      }
      parsed.exclude = exclude;
      patterns.push_back(std::move(parsed));
    }
  };
  while (std::getline(input, line)) {
    auto text = trim(line);
    if (text.empty() || text.front() == '#') {
      continue;
    }
    if (text.front() == '[') {
      in_workspace = text.starts_with("[workspace]");
      in_array = false;
      continue;
    }
    if (!in_workspace) {
      continue;
    }
    if (!in_array) {
      const bool members = text.starts_with("members");
      const bool excludes = text.starts_with("exclude");
      if (!members && !excludes) {
        continue;
      }
      exclude = excludes;
      const auto open = text.find('[');
      if (open == std::string::npos) {
        continue;
      }
      const auto close = text.rfind(']');
      if (close != std::string::npos && close > open) {
        take(text.substr(open + 1, close - open - 1));
        continue;
      }
      take(text.substr(open + 1));
      in_array = true;
      continue;
    }
    if (const auto close = text.rfind(']'); close != std::string::npos) {
      take(text.substr(0, close));
      in_array = false;
      continue;
    }
    take(text);
  }
  manifest = "Cargo.toml";
  return !patterns.empty();
}

// Go: `use ./dir`, or a `use ( … )` block.
[[nodiscard]] bool read_go_work(const fs::path& root, std::vector<Pattern>& patterns, std::string& manifest) {
  std::error_code error;
  const auto path = root / "go.work";
  if (!fs::is_regular_file(path, error)) {
    return false;
  }
  std::ifstream input(path);
  std::string line;
  bool in_block = false;
  while (std::getline(input, line)) {
    auto text = trim(line);
    if (const auto comment = text.find("//"); comment != std::string::npos) {
      text = trim(text.substr(0, comment));
    }
    if (text.empty()) {
      continue;
    }
    if (in_block) {
      if (text == ")") {
        in_block = false;
        continue;
      }
      if (auto parsed = parse_pattern(text); !parsed.segments.empty()) {
        patterns.push_back(std::move(parsed));
      }
      continue;
    }
    if (!text.starts_with("use")) {
      continue;
    }
    const auto rest = trim(text.substr(3));
    if (rest == "(") {
      in_block = true;
      continue;
    }
    if (auto parsed = parse_pattern(rest); !parsed.segments.empty()) {
      patterns.push_back(std::move(parsed));
    }
  }
  manifest = "go.work";
  return !patterns.empty();
}

}  // namespace

std::vector<WorkspacePackage> discover_workspace_packages(const fs::path& root) {
  std::vector<WorkspacePackage> packages;
  if (root.empty()) {
    return packages;
  }
  std::error_code error;
  auto canonical = fs::weakly_canonical(root, error);
  if (error) {
    canonical = root.lexically_normal();
  }
  if (!fs::is_directory(canonical, error)) {
    return packages;
  }

  std::vector<Pattern> patterns;
  std::string manifest;
  // One workspace declaration per root; the first that names members wins, in
  // the order a polyglot repo would expect its own build tool to be found.
  if (!read_npm_workspaces(canonical, patterns, manifest) && !read_pnpm_workspace(canonical, patterns, manifest) &&
      !read_cargo_workspace(canonical, patterns, manifest) && !read_go_work(canonical, patterns, manifest)) {
    return packages;
  }

  std::vector<std::vector<std::string>> directories;
  collect_directories(canonical, canonical, 1, walk_depth_for(patterns), directories);

  std::set<std::string> taken;  // package directory, root-relative
  for (const auto& segments : directories) {
    bool included = false;
    for (const auto& pattern : patterns) {
      if (!segments_match(pattern.segments, segments)) {
        continue;
      }
      if (pattern.exclude) {
        included = false;
        break;
      }
      included = true;
    }
    if (!included) {
      continue;
    }
    fs::path directory = canonical;
    std::string relative;
    for (const auto& segment : segments) {
      directory /= segment;
      relative += relative.empty() ? segment : "/" + segment;
    }
    std::string name;
    if (!package_name_at(directory, name) || !taken.insert(relative).second) {
      continue;  // a glob hit that declares no manifest of its own is just a directory
    }
    packages.push_back(WorkspacePackage{.name = std::move(name), .root = std::move(directory), .manifest = manifest});
    if (packages.size() >= kMaxPackages) {
      break;
    }
  }

  // Deepest first, so a package nested inside another owns its own files.
  std::ranges::sort(packages, [](const WorkspacePackage& lhs, const WorkspacePackage& rhs) {
    const auto left = std::distance(lhs.root.begin(), lhs.root.end());
    const auto right = std::distance(rhs.root.begin(), rhs.root.end());
    if (left != right) {
      return left > right;
    }
    return lhs.root < rhs.root;
  });
  return packages;
}

const WorkspacePackage* package_for_file(std::span<const WorkspacePackage> packages, const fs::path& source_file) {
  // Package roots are canonical, so the file must be too, or a symlinked prefix
  // (macOS `/var` -> `/private/var`) makes every file look outside every package.
  std::error_code error;
  auto normalized = fs::weakly_canonical(source_file, error);
  if (error) {
    normalized = source_file.lexically_normal();
  }
  for (const auto& package : packages) {
    const auto relative = normalized.lexically_relative(package.root);
    const auto text = relative.generic_string();
    if (!text.empty() && text != "." && !text.starts_with("..")) {
      return &package;
    }
  }
  return nullptr;
}

}  // namespace cgraph
