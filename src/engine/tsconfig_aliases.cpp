#include "cgraph/json_text.hpp"
#include "cgraph/tsconfig_aliases.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace cgraph {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string read_file(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// Strips the trailing `*` from a wildcard pattern, returning the literal prefix.
// Returns nullopt for a non-wildcard entry (those address a single module and
// are far rarer; cgraph resolves only the wildcard form here).
[[nodiscard]] std::optional<std::string> wildcard_prefix(std::string_view value) {
  if (!value.empty() && value.back() == '*') {
    return std::string(value.substr(0, value.size() - 1));
  }
  return std::nullopt;
}

}  // namespace

std::vector<PathAlias> load_path_aliases(const fs::path& root) {
  std::string contents;
  // Resolve symlinks (on macOS `/tmp` -> `/private/tmp`) so the alias base
  // matches the canonical source-file paths the file index is keyed by.
  std::error_code canon_ec;
  fs::path config_dir = fs::weakly_canonical(root, canon_ec);
  if (canon_ec) {
    config_dir = root;
  }
  for (const auto* name : {"tsconfig.json", "jsconfig.json"}) {
    const auto path = config_dir / name;
    if (std::error_code ec; fs::exists(path, ec)) {
      contents = read_file(path);
      config_dir = path.parent_path();
      break;
    }
  }
  if (contents.empty()) {
    return {};
  }

  // tsconfig is JSON-with-comments; tolerate that, and never throw on a
  // malformed config — a missing alias map simply means no alias resolution.
  const auto json = cgraph::try_parse_json(contents, cgraph::JsonComments::Ignored);
  if (json.is_discarded() || !json.is_object()) {
    return {};
  }
  const auto options = json.find("compilerOptions");
  if (options == json.end() || !options->is_object()) {
    return {};
  }
  const auto paths = options->find("paths");
  if (paths == options->end() || !paths->is_object()) {
    return {};
  }

  // baseUrl defaults to the directory containing the config (TypeScript's
  // behavior once `paths` is present).
  fs::path base = config_dir;
  if (const auto base_url = options->find("baseUrl"); base_url != options->end() && base_url->is_string()) {
    base = (config_dir / base_url->get<std::string>()).lexically_normal();
  }

  std::vector<PathAlias> aliases;
  for (const auto& [pattern, replacements] : paths->items()) {
    const auto prefix = wildcard_prefix(pattern);
    if (!prefix || !replacements.is_array()) {
      continue;
    }
    PathAlias alias{.pattern_prefix = *prefix};
    for (const auto& replacement : replacements) {
      if (!replacement.is_string()) {
        continue;
      }
      const auto repl_prefix = wildcard_prefix(replacement.get<std::string>());
      if (!repl_prefix) {
        continue;
      }
      alias.replacement_prefixes.push_back((base / *repl_prefix).lexically_normal().generic_string());
    }
    if (!alias.replacement_prefixes.empty()) {
      aliases.push_back(std::move(alias));
    }
  }
  return aliases;
}

std::vector<std::string> expand_path_alias(std::span<const PathAlias> aliases, const std::string& spec) {
  std::vector<std::string> candidates;
  for (const auto& alias : aliases) {
    if (spec.size() < alias.pattern_prefix.size() ||
        spec.compare(0, alias.pattern_prefix.size(), alias.pattern_prefix) != 0) {
      continue;
    }
    const auto tail = spec.substr(alias.pattern_prefix.size());
    for (const auto& replacement : alias.replacement_prefixes) {
      // The replacement is the resolved directory the alias maps into; the
      // matched tail is a path relative to it.
      candidates.push_back((fs::path(replacement) / tail).lexically_normal().generic_string());
    }
  }
  return candidates;
}

}  // namespace cgraph
