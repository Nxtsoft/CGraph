#include "cgraph/endpoint_prefixes.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::cerr << "endpoint_prefixes_test: " << message << '\n';
  return 1;
}

// A consumer spelling under `from` maps to `to` + the rest, on a segment
// boundary only, and only for the repo the prefix names.
int test_proxied_endpoint_id() {
  const std::vector<cgraph::EndpointPrefix> prefixes{{.repo = "web", .from = "/api/backend", .to = "/api"}};
  const auto mapped = cgraph::proxied_endpoint_id(prefixes, "web", "endpoint:PATCH /api/backend/v1/users/{}/enable");
  if (mapped != "endpoint:PATCH /api/v1/users/{}/enable") {
    return fail("from-prefixed consumer id maps to the to-prefixed id, got " + mapped.value_or("nothing"));
  }
  if (cgraph::proxied_endpoint_id(prefixes, "web", "endpoint:GET /api/backendx/v1") ||
      cgraph::proxied_endpoint_id(prefixes, "web", "endpoint:GET /api/v1/users") ||
      cgraph::proxied_endpoint_id(prefixes, "api", "endpoint:GET /api/backend/v1/users") ||
      cgraph::proxied_endpoint_id(prefixes, "web", "src_api_ts")) {
    return fail("only a path under `from` on a segment boundary, in the named repo, maps");
  }
  if (cgraph::proxied_endpoint_id(prefixes, "web", "endpoint:GET /api/backend") != "endpoint:GET /api") {
    return fail("the prefix itself maps to `to`");
  }
  const std::vector<cgraph::EndpointPrefix> root{{.repo = "web", .from = "/proxy", .to = "/"}};
  if (cgraph::proxied_endpoint_id(root, "web", "endpoint:GET /proxy/health") != "endpoint:GET /health") {
    return fail("a `to` of / strips the prefix");
  }
  return 0;
}

// The inverse spelling round-trips, and a first-wins overlap is respected.
int test_consumer_spellings() {
  const std::vector<cgraph::EndpointPrefix> prefixes{{.repo = "web", .from = "/api/backend", .to = "/api"}};
  const auto spellings = cgraph::consumer_spellings(prefixes, "web", "endpoint:GET /api/v1/users/{}");
  if (spellings != std::vector<std::string>{"endpoint:GET /api/backend/v1/users/{}"}) {
    return fail("the provider id has one consumer spelling through the prefix");
  }
  if (!cgraph::consumer_spellings(prefixes, "web", "endpoint:GET /oauth2/token").empty() ||
      !cgraph::consumer_spellings(prefixes, "api", "endpoint:GET /api/v1/users/{}").empty()) {
    return fail("an id outside `to`, or another repo, has no consumer spelling");
  }
  return 0;
}

// Manifest entries are validated and normalized; the CLI flag reads the same.
int test_parse() {
  std::vector<std::string> errors;
  const auto parsed = cgraph::parse_endpoint_prefixes(
      nlohmann::json::array({{{"repo", "web"}, {"from", "/api/backend/"}, {"to", "//api"}}}), errors);
  if (!errors.empty() || parsed.size() != 1 || parsed[0].from != "/api/backend" || parsed[0].to != "/api") {
    return fail("a prefix entry is normalized (trailing and duplicate slashes dropped)");
  }
  if (cgraph::endpoint_prefixes_json(parsed) !=
      nlohmann::json::array({{{"repo", "web"}, {"from", "/api/backend"}, {"to", "/api"}}})) {
    return fail("the JSON form round-trips");
  }
  for (const auto& bad : {nlohmann::json{{"repo", "web"}, {"from", "api/backend"}, {"to", "/api"}},
                          nlohmann::json{{"from", "/a"}, {"to", "/b"}}, nlohmann::json{{"repo", "web"}, {"from", "/a"}, {"to", "/a"}},
                          nlohmann::json("web")}) {
    std::vector<std::string> bad_errors;
    if (!cgraph::parse_endpoint_prefixes(nlohmann::json::array({bad}), bad_errors).empty() || bad_errors.empty()) {
      return fail("a malformed prefix entry is an error: " + bad.dump());
    }
  }
  std::string error;
  const auto flag = cgraph::parse_endpoint_prefix_flag("idp-front-end:/api/backend=/api", error);
  if (!flag || flag->repo != "idp-front-end" || flag->from != "/api/backend" || flag->to != "/api") {
    return fail("the CLI flag REPO:/from=/to parses");
  }
  if (cgraph::parse_endpoint_prefix_flag("/api/backend=/api", error) || error.empty()) {
    return fail("a flag without a repo is refused");
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += test_proxied_endpoint_id();
  failures += test_consumer_spellings();
  failures += test_parse();
  return failures == 0 ? 0 : 1;
}
