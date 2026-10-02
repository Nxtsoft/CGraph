#include "cgraph/contract_declarations.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nlohmann::json;

int fail(std::string_view message) {
  std::cerr << "contract_declarations_test: " << message << '\n';
  return 1;
}

// A declared member's repo-local table or label is spelled as its database's;
// a member outside every database, a table already in a named database, and
// any other kind are never respelled. The inverse only for a member of that
// database.
int test_database_spelling() {
  const std::vector<cgraph::ContractDatabase> databases{{.name = "turing", .repos = {"api", "ml"}}};
  if (cgraph::declared_contract_id(databases, "api", "table:local:users") != "table:turing:users" ||
      cgraph::declared_contract_id(databases, "ml", "label:local:HAS_ROLE") != "label:turing:HAS_ROLE") {
    return fail("a declared member's local table and label take the database's spelling");
  }
  if (cgraph::declared_contract_id(databases, "web", "table:local:users") ||
      cgraph::declared_contract_id(databases, "api", "table:billing:users") ||
      cgraph::declared_contract_id(databases, "api", "header:x-org-id") ||
      cgraph::declared_contract_id(databases, "api", "endpoint:GET /users")) {
    return fail("only a declared member's database-local id is respelled");
  }
  if (cgraph::local_contract_spelling(databases, "ml", "table:turing:users") != "table:local:users" ||
      cgraph::local_contract_spelling(databases, "web", "table:turing:users") ||
      cgraph::local_contract_spelling(databases, "ml", "table:billing:users") ||
      cgraph::local_contract_spelling(databases, "ml", "table:local:users")) {
    return fail("the local spelling is a member's of that database only");
  }
  return 0;
}

// The one crossing rule: a member's local table crosses at its database's id,
// a declared env id is itself, an undeclared env id and a standard header
// cross nowhere. The spellings of a crossing in a member are the crossing id
// and, inside its database, its own local id, whichever spelling the home
// repo's graph used.
int test_crossing_and_spellings() {
  const std::vector<cgraph::ContractDatabase> databases{{.name = "turing", .repos = {"api", "ml"}}};
  const std::vector<cgraph::EnvProvider> env{{.name = "API_URL", .service = "api"}};
  if (cgraph::crossing_id(databases, env, "ml", "table:local:users") != "table:turing:users" ||
      cgraph::crossing_id(databases, env, "api", "table:turing:users") != "table:turing:users" ||
      cgraph::crossing_id(databases, env, "web", "table:local:users") ||
      cgraph::crossing_id(databases, env, "web", "env:API_URL") != "env:API_URL" ||
      cgraph::crossing_id(databases, env, "web", "env:NODE_ENV") ||
      cgraph::crossing_id(databases, env, "web", "header:authorization") ||
      cgraph::crossing_id(databases, env, "web", "header:x-tenant-id") != "header:x-tenant-id") {
    return fail("crossing_id");
  }
  using Ids = std::vector<std::string>;
  if (cgraph::contract_spellings(databases, env, "ml", "table:turing:users") != Ids{"table:turing:users", "table:local:users"} ||
      cgraph::contract_spellings(databases, env, "web", "table:turing:users") != Ids{"table:turing:users"} ||
      cgraph::contract_spellings(databases, env, "ml", "env:API_URL") != Ids{"env:API_URL"} ||
      !cgraph::contract_spellings(databases, env, "ml", "env:NODE_ENV").empty() ||
      !cgraph::contract_spellings(databases, env, "ml", "table:local:users").empty() ||
      !cgraph::contract_spellings(databases, env, "ml", "header:accept").empty()) {
    return fail("contract_spellings");
  }
  if (cgraph::declared_database(databases, "ml") != &databases[0] || cgraph::declared_database(databases, "web") != nullptr) {
    return fail("declared_database");
  }
  return 0;
}

int test_env_provider() {
  const std::vector<cgraph::EnvProvider> env{{.name = "ML_BACKEND_URL", .service = "ml"}};
  if (cgraph::env_provider_of(env, "env:ML_BACKEND_URL") != "ml" || cgraph::env_provider_of(env, "env:ml_backend_url") ||
      cgraph::env_provider_of(env, "header:ML_BACKEND_URL")) {
    return fail("an env id's provider is the declared service, by exact name");
  }
  return 0;
}

// Parsing, the flag forms, the round trip, and every malformed or conflicting
// declaration reported as an error, never an exception.
int test_parse_and_validate() {
  std::vector<std::string> errors;
  const auto databases = cgraph::parse_contract_databases(
      json::parse(R"([{"name": "turing", "repos": ["api", "ml", "api"]}])"), errors);
  const auto env = cgraph::parse_env_providers(json::parse(R"([{"name": "ML_BACKEND_URL", "service": "ml"}])"), errors);
  if (!errors.empty() || databases.size() != 1 || databases[0].repos != std::vector<std::string>{"api", "ml"} ||
      env.size() != 1) {
    return fail("valid declarations parse, a repeated repo once");
  }
  if (cgraph::contract_databases_json(databases) != json::parse(R"([{"name": "turing", "repos": ["api", "ml"]}])") ||
      cgraph::env_providers_json(env) != json::parse(R"([{"name": "ML_BACKEND_URL", "service": "ml"}])")) {
    return fail("declarations round-trip through their JSON form");
  }
  std::string error;
  const auto flag = cgraph::parse_database_flag("turing=api,ml", error);
  const auto env_flag = cgraph::parse_env_flag("ML_BACKEND_URL=ml", error);
  if (!flag || flag->name != "turing" || flag->repos != std::vector<std::string>{"api", "ml"} || !env_flag ||
      env_flag->service != "ml") {
    return fail("the flag forms parse: " + error);
  }
  for (const auto* bad : {"turing", "=api", "local=api", "a:b=api", "turing=", "turing=api,,ml"}) {
    error.clear();
    if (cgraph::parse_database_flag(bad, error) || error.empty()) {
      return fail(std::string("a malformed --database is an error: ") + bad);
    }
  }
  for (const auto* bad : {"ML_BACKEND_URL", "=ml", "ML_BACKEND_URL="}) {
    error.clear();
    if (cgraph::parse_env_flag(bad, error) || error.empty()) {
      return fail(std::string("a malformed --env is an error: ") + bad);
    }
  }
  for (const auto* text : {R"({"name": "x"})", R"([{"name": 3, "repos": ["api"]}])", R"([{"name": "x", "repos": "api"}])",
                           R"([{"name": "x", "repos": [7]}])", R"([{"name": "x", "repos": []}])", R"(["x"])"}) {
    std::vector<std::string> problems;
    try {
      if (!cgraph::parse_contract_databases(json::parse(text), problems).empty() || problems.empty()) {
        return fail(std::string("a malformed `databases` is an error: ") + text);
      }
    } catch (const std::exception& exception) {
      return fail(std::string("a malformed `databases` threw: ") + exception.what());
    }
  }
  for (const auto* text : {R"([{"name": "X"}])", R"([{"name": "X", "service": 1}])", R"([{"name": "", "service": "ml"}])"}) {
    std::vector<std::string> problems;
    try {
      if (!cgraph::parse_env_providers(json::parse(text), problems).empty() || problems.empty()) {
        return fail(std::string("a malformed `env` is an error: ") + text);
      }
    } catch (const std::exception& exception) {
      return fail(std::string("a malformed `env` threw: ") + exception.what());
    }
  }
  const std::vector<std::string> members{"api", "ml", "web"};
  if (!cgraph::contract_declaration_errors(databases, env, members).empty()) {
    return fail("declarations naming members are valid");
  }
  const std::vector<cgraph::ContractDatabase> stranger{{.name = "turing", .repos = {"api", "billing"}}};
  const std::vector<cgraph::ContractDatabase> twice{{.name = "a", .repos = {"api"}}, {.name = "b", .repos = {"api"}}};
  const std::vector<cgraph::ContractDatabase> duplicate{{.name = "a", .repos = {"api"}}, {.name = "a", .repos = {"ml"}}};
  const std::vector<cgraph::EnvProvider> unknown_service{{.name = "X", .service = "billing"}};
  const std::vector<cgraph::EnvProvider> env_twice{{.name = "X", .service = "ml"}, {.name = "X", .service = "api"}};
  if (cgraph::contract_declaration_errors(stranger, {}, members).empty() ||
      cgraph::contract_declaration_errors(twice, {}, members).empty() ||
      cgraph::contract_declaration_errors(duplicate, {}, members).empty() ||
      cgraph::contract_declaration_errors({}, unknown_service, members).empty() ||
      cgraph::contract_declaration_errors({}, env_twice, members).empty()) {
    return fail("an unknown repo or service, a repo in two databases, or a name declared twice is an error");
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += test_database_spelling();
  failures += test_env_provider();
  failures += test_crossing_and_spellings();
  failures += test_parse_and_validate();
  return failures == 0 ? 0 : 1;
}
