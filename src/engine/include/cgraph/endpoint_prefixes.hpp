#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Proxy prefixes between repositories (cross-repo contract matching).
//
// A front end often reaches its backend through its own catch-all proxy route:
// idp-front-end calls `/api/backend/v1/users/{id}`, and its Next.js
// `app/api/backend/[...path]/route.ts` forwards to `${BACKEND_URL}/api/v1/users/{id}`.
// Each repo's graph keeps the spelling its own code uses, so the consumer's
// `endpoint:GET /api/backend/v1/users/{}` and the provider's
// `endpoint:GET /api/v1/users/{}` never share an id.
//
// A prefix entry `{repo, from, to}` says: an endpoint `repo` consumes but does not
// serve itself, whose path starts with `from` (on a segment boundary), reaches the
// other repos as the same method at `to` + the rest. Single-repo graph ids are
// never rewritten; the mapping applies only where repos are matched (seam
// discover/fuse and workspace federation). An endpoint the repo serves itself
// (`/api/backend/healthz` next to the catch-all) is never mapped: the framework
// routes the specific route before the catch-all.
namespace cgraph {

struct EndpointPrefix {
  std::string repo;
  std::string from;  // normalized: leading slash, no trailing slash
  std::string to;
};

// Reads a JSON array of `{"repo", "from", "to"}` objects. `from` and `to` must
// be absolute paths (leading `/`); they are normalized (duplicate and trailing
// slashes dropped). Every problem is appended to `errors`.
[[nodiscard]] std::vector<EndpointPrefix> parse_endpoint_prefixes(const nlohmann::json& entries,
                                                                  std::vector<std::string>& errors);

// Parses the CLI spelling `REPO:/from=/to`; nullopt (with `error`) when malformed.
[[nodiscard]] std::optional<EndpointPrefix> parse_endpoint_prefix_flag(std::string_view flag, std::string& error);

// The JSON form parse_endpoint_prefixes reads.
[[nodiscard]] nlohmann::json endpoint_prefixes_json(std::span<const EndpointPrefix> prefixes);

// `endpoint:<METHOD> <from>/rest` -> `endpoint:<METHOD> <to>/rest` for the first
// prefix of `repo` that covers the id's path; nullopt when none does.
[[nodiscard]] std::optional<std::string> proxied_endpoint_id(std::span<const EndpointPrefix> prefixes,
                                                             std::string_view repo, std::string_view endpoint_id);

// The inverse: every spelling `repo` could consume `endpoint_id` under through
// one of its prefixes (`to` replaced by `from`). Empty when no prefix covers it.
[[nodiscard]] std::vector<std::string> consumer_spellings(std::span<const EndpointPrefix> prefixes,
                                                          std::string_view repo, std::string_view endpoint_id);

}  // namespace cgraph
