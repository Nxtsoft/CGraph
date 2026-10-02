#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Declared contract scopes between repositories (cross-repo contract matching).
//
// Code rarely proves which repositories share a database: turing-api reads
// `DATABASE_URL`, ml-backend `WEBAPP_DB_URL_*`, and only a comment says they are
// one Postgres. A table or graph label a lone repo's graph knows no database
// for is `table:local:<name>` (contracts.hpp), local to that repo. A declaration
// `{"name": "turing", "repos": ["turing-api", "ml-backend"]}` says both repos use
// database `turing`, so where repos are matched (seam discover/fuse, workspace
// impact/path, change context's cross_service) each one's `table:local:<name>`
// is spelled `table:turing:<name>` and the two meet there. Single-repo graph ids
// are never rewritten, so a repo's graph does not depend on which workspace it
// sits in, and an undeclared repo's tables never join anyone's.
//
// Env names are global (`env:<NAME>`), but nothing in the consumer's code says
// which service a variable such as `ML_BACKEND_URL` addresses. A declaration
// `{"name": "ML_BACKEND_URL", "service": "ml-backend"}` says it: `env:ML_BACKEND_URL`
// is provided by the member `ml-backend`.
namespace cgraph {

struct ContractDatabase {
  std::string name;                // no `:`, never `local`
  std::vector<std::string> repos;  // member repo names, manifest order, no duplicates
};

struct EnvProvider {
  std::string name;     // the variable, as code spells it
  std::string service;  // the member repo it addresses
};

// Reads a JSON array of `{"name", "repos": [...]}` objects. Every problem with
// an entry (not an object, non-string or empty members, a name with `:`, the
// reserved name `local`, an empty repo list) is appended to `errors`.
[[nodiscard]] std::vector<ContractDatabase> parse_contract_databases(const nlohmann::json& entries,
                                                                     std::vector<std::string>& errors);

// Reads a JSON array of `{"name", "service"}` objects, errors as above.
[[nodiscard]] std::vector<EnvProvider> parse_env_providers(const nlohmann::json& entries,
                                                           std::vector<std::string>& errors);

// The CLI spellings `NAME=repoA,repoB` and `NAME=service`; nullopt (with `error`)
// when malformed, by the same rules as the JSON forms.
[[nodiscard]] std::optional<ContractDatabase> parse_database_flag(std::string_view flag, std::string& error);
[[nodiscard]] std::optional<EnvProvider> parse_env_flag(std::string_view flag, std::string& error);

// One error per problem across the declarations: a repo or service that is not
// among `members` (a manifest's repos, or the graphs given to a seam command),
// a database or env name declared twice, and a repo declared in two databases
// (its `table:local:` ids could not say which one they mean).
[[nodiscard]] std::vector<std::string> contract_declaration_errors(std::span<const ContractDatabase> databases,
                                                                   std::span<const EnvProvider> env,
                                                                   std::span<const std::string> members);

// The JSON forms the parsers read.
[[nodiscard]] nlohmann::json contract_databases_json(std::span<const ContractDatabase> databases);
[[nodiscard]] nlohmann::json env_providers_json(std::span<const EnvProvider> env);

// `table:local:<name>` / `label:local:<name>` in `repo`'s graph -> the declared
// database's spelling `table:<database>:<name>`; nullopt when `repo` declares no
// database or `id` is not database-local.
[[nodiscard]] std::optional<std::string> declared_contract_id(std::span<const ContractDatabase> databases,
                                                              std::string_view repo, std::string_view id);

// The inverse: `table:<database>:<name>` -> `table:local:<name>` when `repo` is
// declared in `<database>`; nullopt otherwise.
[[nodiscard]] std::optional<std::string> local_contract_spelling(std::span<const ContractDatabase> databases,
                                                                 std::string_view repo, std::string_view contract);

// The member declared to provide `env:<NAME>`; nullopt when none is or `id` is
// no env id.
[[nodiscard]] std::optional<std::string> env_provider_of(std::span<const EnvProvider> env, std::string_view id);

}  // namespace cgraph
