#include "cgraph/endpoint_prefixes.hpp"

#include "cgraph/contracts.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace cgraph {
namespace {

constexpr std::string_view kEndpointPrefix = "endpoint:";

// `endpoint:<METHOD> <path>` split into its method and path.
struct EndpointParts {
  std::string_view method;
  std::string_view path;
};

[[nodiscard]] std::optional<EndpointParts> split_endpoint(std::string_view id) {
  if (!id.starts_with(kEndpointPrefix)) {
    return std::nullopt;
  }
  id.remove_prefix(kEndpointPrefix.size());
  const auto space = id.find(' ');
  if (space == std::string_view::npos || space + 1 >= id.size() || id[space + 1] != '/') {
    return std::nullopt;
  }
  return EndpointParts{.method = id.substr(0, space), .path = id.substr(space + 1)};
}

// The rest of `path` beneath `prefix` ("" for the prefix itself), or nullopt
// when `path` is not under it on a segment boundary: `/api/backend` covers
// `/api/backend/v1` but not `/api/backendx`.
[[nodiscard]] std::optional<std::string_view> beneath(std::string_view path, std::string_view prefix) {
  if (prefix == "/") {
    return path;
  }
  if (!path.starts_with(prefix)) {
    return std::nullopt;
  }
  const auto rest = path.substr(prefix.size());
  if (!rest.empty() && rest.front() != '/') {
    return std::nullopt;
  }
  return rest;
}

[[nodiscard]] std::string respell(const EndpointParts& parts, std::string_view to, std::string_view rest) {
  return std::string(kEndpointPrefix) + std::string(parts.method) + " " +
         canonical_route_path(join_route_path(to, rest));
}

[[nodiscard]] std::optional<std::string> normalized_prefix(const std::string& value) {
  if (value.empty() || value.front() != '/') {
    return std::nullopt;
  }
  return canonical_route_path(join_route_path("", value));
}

}  // namespace

std::vector<EndpointPrefix> parse_endpoint_prefixes(const nlohmann::json& entries, std::vector<std::string>& errors) {
  std::vector<EndpointPrefix> prefixes;
  if (!entries.is_array()) {
    errors.push_back("`prefixes` must be an array of {\"repo\", \"from\", \"to\"} objects");
    return prefixes;
  }
  for (const auto& entry : entries) {
    if (!entry.is_object()) {
      errors.push_back("each `prefixes` entry must be an object with `repo`, `from` and `to`");
      continue;
    }
    // A non-string member is an entry error, never a JSON type exception: a
    // parent manifest's typo must not take down every tool beneath it.
    const bool strings = std::ranges::all_of(std::array{"repo", "from", "to"}, [&](const char* key) {
      const auto member = entry.find(key);
      return member == entry.end() || member->is_string();
    });
    if (!strings) {
      errors.push_back("`repo`, `from` and `to` in a `prefixes` entry must be strings: " + entry.dump());
      continue;
    }
    const auto repo = entry.value("repo", std::string{});
    const auto from = normalized_prefix(entry.value("from", std::string{}));
    const auto to = normalized_prefix(entry.value("to", std::string{}));
    if (repo.empty() || !from || !to) {
      errors.push_back("each `prefixes` entry needs a `repo` and absolute `from` and `to` paths (leading /)");
      continue;
    }
    if (*from == *to) {
      errors.push_back("prefix for repo '" + repo + "' maps " + *from + " to itself");
      continue;
    }
    prefixes.push_back(EndpointPrefix{.repo = repo, .from = *from, .to = *to});
  }
  return prefixes;
}

std::optional<EndpointPrefix> parse_endpoint_prefix_flag(std::string_view flag, std::string& error) {
  const auto colon = flag.find(':');
  const auto eq = flag.find('=', colon == std::string_view::npos ? 0 : colon);
  if (colon == std::string_view::npos || eq == std::string_view::npos || colon == 0) {
    error = "expected REPO:/from=/to, got '" + std::string(flag) + "'";
    return std::nullopt;
  }
  std::vector<std::string> errors;
  const nlohmann::json entry{{"repo", std::string(flag.substr(0, colon))},
                             {"from", std::string(flag.substr(colon + 1, eq - colon - 1))},
                             {"to", std::string(flag.substr(eq + 1))}};
  auto parsed = parse_endpoint_prefixes(nlohmann::json::array({entry}), errors);
  if (!errors.empty()) {
    error = errors.front();
    return std::nullopt;
  }
  return std::move(parsed.front());
}

std::vector<std::string> unknown_prefix_repos(std::span<const EndpointPrefix> prefixes, std::span<const std::string> repos) {
  std::vector<std::string> errors;
  for (const auto& prefix : prefixes) {
    if (std::ranges::find(repos, prefix.repo) == repos.end()) {
      errors.push_back("prefix names repo '" + prefix.repo + "', which is not among the repos given");
    }
  }
  return errors;
}

nlohmann::json endpoint_prefixes_json(std::span<const EndpointPrefix> prefixes) {
  auto array = nlohmann::json::array();
  for (const auto& prefix : prefixes) {
    array.push_back({{"repo", prefix.repo}, {"from", prefix.from}, {"to", prefix.to}});
  }
  return array;
}

std::optional<std::string> proxied_endpoint_id(std::span<const EndpointPrefix> prefixes, std::string_view repo,
                                               std::string_view endpoint_id) {
  const auto parts = split_endpoint(endpoint_id);
  if (!parts) {
    return std::nullopt;
  }
  for (const auto& prefix : prefixes) {
    if (prefix.repo != repo) {
      continue;
    }
    if (const auto rest = beneath(parts->path, prefix.from)) {
      return respell(*parts, prefix.to, *rest);
    }
  }
  return std::nullopt;
}

std::vector<std::string> consumer_spellings(std::span<const EndpointPrefix> prefixes, std::string_view repo,
                                            std::string_view endpoint_id) {
  std::vector<std::string> spellings;
  const auto parts = split_endpoint(endpoint_id);
  if (!parts) {
    return spellings;
  }
  for (const auto& prefix : prefixes) {
    if (prefix.repo != repo) {
      continue;
    }
    if (const auto rest = beneath(parts->path, prefix.to)) {
      auto spelling = respell(*parts, prefix.from, *rest);
      // Only a spelling that maps back to this id through the same repo's
      // prefixes: with two overlapping `from`s the first one wins both ways.
      if (proxied_endpoint_id(prefixes, repo, spelling) == std::string(endpoint_id)) {
        spellings.push_back(std::move(spelling));
      }
    }
  }
  return spellings;
}

}  // namespace cgraph
