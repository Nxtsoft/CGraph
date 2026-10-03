#include "cgraph/contract_declarations.hpp"

#include "cgraph/contracts.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace cgraph {
namespace {

// `table:<database>:<name>` split after its kind: the database and the name.
struct ScopedParts {
  std::string_view kind;
  std::string_view database;
  std::string_view name;
};

[[nodiscard]] std::optional<ScopedParts> split_scoped(std::string_view id) {
  const auto kind = contract_kind_of(id);
  if (kind != "table" && kind != "label") {
    return std::nullopt;
  }
  const auto rest = id.substr(kind.size() + 1);
  const auto colon = rest.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= rest.size()) {
    return std::nullopt;
  }
  return ScopedParts{.kind = kind, .database = rest.substr(0, colon), .name = rest.substr(colon + 1)};
}

[[nodiscard]] bool plain_name(std::string_view name) {
  return !name.empty() && std::ranges::none_of(name, [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n'; });
}

// The words a `{"name", "repos"}` declaration's errors use: a database or an
// issuer.
struct ScopeWords {
  std::string_view key;       // the manifest key, `databases`
  std::string_view noun;      // one entry, `database`
  std::string_view reserved;  // why `local` is reserved, after the name
  std::string_view member;    // what a member names, `a table`
};
constexpr ScopeWords kDatabaseWords{.key = "databases", .noun = "database",
                                    .reserved = " for tables no database is declared for", .member = "a table"};
constexpr ScopeWords kIssuerWords{.key = "issuers", .noun = "issuer", .reserved = "", .member = "a claim"};

template <typename Scope>
[[nodiscard]] std::optional<std::string> scope_entry_error(const Scope& scope, const ScopeWords& words) {
  if (!plain_name(scope.name) || scope.name.find(':') != std::string::npos) {
    return "each `" + std::string(words.key) + "` entry needs a `name` with no `:` or whitespace: '" + scope.name + "'";
  }
  if (scope.name == kLocalDatabase) {
    return std::string(words.noun) + " name `local` is reserved" + std::string(words.reserved);
  }
  if (scope.repos.empty() || std::ranges::any_of(scope.repos, [](const std::string& repo) { return repo.empty(); })) {
    return std::string(words.noun) + " '" + scope.name + "' needs a non-empty `repos` list of repo names";
  }
  return std::nullopt;
}

template <typename Scope>
[[nodiscard]] std::vector<Scope> parse_scopes(const nlohmann::json& entries, const ScopeWords& words,
                                              std::vector<std::string>& errors) {
  std::vector<Scope> scopes;
  const std::string key(words.key);
  if (!entries.is_array()) {
    errors.push_back("`" + key + "` must be an array of {\"name\", \"repos\"} objects");
    return scopes;
  }
  for (const auto& entry : entries) {
    if (!entry.is_object()) {
      errors.push_back("each `" + key + "` entry must be an object with `name` and `repos`");
      continue;
    }
    // A non-string member is an entry error, never a JSON type exception.
    const auto name = entry.find("name");
    const auto repos = entry.find("repos");
    if (name == entry.end() || !name->is_string() || repos == entry.end() || !repos->is_array() ||
        !std::ranges::all_of(*repos, [](const nlohmann::json& repo) { return repo.is_string(); })) {
      errors.push_back("each `" + key + "` entry needs a string `name` and a `repos` array of strings: " + entry.dump());
      continue;
    }
    Scope scope{.name = name->get<std::string>(), .repos = {}};
    for (const auto& repo : *repos) {
      auto repo_name = repo.get<std::string>();
      if (std::ranges::find(scope.repos, repo_name) == scope.repos.end()) {
        scope.repos.push_back(std::move(repo_name));
      }
    }
    if (auto error = scope_entry_error(scope, words)) {
      errors.push_back(std::move(*error));
      continue;
    }
    scopes.push_back(std::move(scope));
  }
  return scopes;
}

// `NAME=repoA,repoB`, by the JSON form's rules.
template <typename Scope>
[[nodiscard]] std::optional<Scope> parse_scope_flag(std::string_view flag, const ScopeWords& words, std::string& error) {
  const auto eq = flag.find('=');
  if (eq == std::string_view::npos || eq == 0) {
    error = "expected NAME=repoA,repoB, got '" + std::string(flag) + "'";
    return std::nullopt;
  }
  auto repos = nlohmann::json::array();
  for (auto rest = flag.substr(eq + 1);;) {
    const auto comma = rest.find(',');
    repos.push_back(std::string(rest.substr(0, comma)));
    if (comma == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(comma + 1);
  }
  std::vector<std::string> errors;
  auto parsed = parse_scopes<Scope>(
      nlohmann::json::array({{{"name", std::string(flag.substr(0, eq))}, {"repos", std::move(repos)}}}), words, errors);
  if (!errors.empty()) {
    error = errors.front();
    return std::nullopt;
  }
  return std::move(parsed.front());
}

// A scope declared twice, a member that is not among `members`, and a repo in
// two scopes of one kind.
template <typename Scope>
void scope_errors(std::span<const Scope> scopes, const ScopeWords& words, std::span<const std::string> members,
                  std::vector<std::string>& errors) {
  const std::string noun(words.noun);
  std::set<std::string> names;
  std::map<std::string, std::string> scope_of;  // repo -> the first scope declaring it
  for (const auto& scope : scopes) {
    if (!names.insert(scope.name).second) {
      errors.push_back(noun + " '" + scope.name + "' is declared twice");
    }
    for (const auto& repo : scope.repos) {
      if (std::ranges::find(members, repo) == members.end()) {
        errors.push_back(noun + " '" + scope.name + "' names repo '" + repo + "', which is not among the repos given");
      }
      const auto [slot, inserted] = scope_of.emplace(repo, scope.name);
      if (!inserted && slot->second != scope.name) {
        errors.push_back("repo '" + repo + "' is declared in two " + std::string(words.key) + " ('" + slot->second +
                         "', '" + scope.name + "'); " + std::string(words.member) + " it names could not say which");
      }
    }
  }
}

template <typename Scope>
[[nodiscard]] nlohmann::json scopes_json(std::span<const Scope> scopes) {
  auto array = nlohmann::json::array();
  for (const auto& scope : scopes) {
    array.push_back({{"name", scope.name}, {"repos", scope.repos}});
  }
  return array;
}

template <typename Scope>
[[nodiscard]] bool is_member(const Scope& scope, std::string_view repo) {
  return std::ranges::find(scope.repos, repo) != scope.repos.end();
}

template <typename Scope>
[[nodiscard]] const Scope* scope_of_repo(std::span<const Scope> scopes, std::string_view repo) {
  const auto found = std::ranges::find_if(scopes, [&](const Scope& scope) { return is_member(scope, repo); });
  return found == scopes.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<std::string> env_entry_error(const EnvProvider& env) {
  if (!plain_name(env.name) || env.service.empty()) {
    return "each `env` entry needs a `name` with no whitespace and a `service`: '" + env.name + "'";
  }
  return std::nullopt;
}

}  // namespace

std::vector<ContractDatabase> parse_contract_databases(const nlohmann::json& entries, std::vector<std::string>& errors) {
  return parse_scopes<ContractDatabase>(entries, kDatabaseWords, errors);
}

std::vector<ClaimIssuer> parse_claim_issuers(const nlohmann::json& entries, std::vector<std::string>& errors) {
  return parse_scopes<ClaimIssuer>(entries, kIssuerWords, errors);
}

std::vector<EnvProvider> parse_env_providers(const nlohmann::json& entries, std::vector<std::string>& errors) {
  std::vector<EnvProvider> providers;
  if (!entries.is_array()) {
    errors.push_back("`env` must be an array of {\"name\", \"service\"} objects");
    return providers;
  }
  for (const auto& entry : entries) {
    if (!entry.is_object()) {
      errors.push_back("each `env` entry must be an object with `name` and `service`");
      continue;
    }
    const auto name = entry.find("name");
    const auto service = entry.find("service");
    if (name == entry.end() || !name->is_string() || service == entry.end() || !service->is_string()) {
      errors.push_back("an `env` entry needs a string `name` and a string `service`: " + entry.dump());
      continue;
    }
    EnvProvider provider{.name = name->get<std::string>(), .service = service->get<std::string>()};
    if (auto error = env_entry_error(provider)) {
      errors.push_back(std::move(*error));
      continue;
    }
    providers.push_back(std::move(provider));
  }
  return providers;
}

std::optional<ContractDatabase> parse_database_flag(std::string_view flag, std::string& error) {
  return parse_scope_flag<ContractDatabase>(flag, kDatabaseWords, error);
}

std::optional<ClaimIssuer> parse_issuer_flag(std::string_view flag, std::string& error) {
  return parse_scope_flag<ClaimIssuer>(flag, kIssuerWords, error);
}

std::optional<EnvProvider> parse_env_flag(std::string_view flag, std::string& error) {
  const auto eq = flag.find('=');
  if (eq == std::string_view::npos || eq == 0) {
    error = "expected NAME=service, got '" + std::string(flag) + "'";
    return std::nullopt;
  }
  std::vector<std::string> errors;
  auto parsed = parse_env_providers(
      nlohmann::json::array({{{"name", std::string(flag.substr(0, eq))}, {"service", std::string(flag.substr(eq + 1))}}}),
      errors);
  if (!errors.empty()) {
    error = errors.front();
    return std::nullopt;
  }
  return std::move(parsed.front());
}

std::vector<std::string> contract_declaration_errors(std::span<const ContractDatabase> databases,
                                                     std::span<const EnvProvider> env,
                                                     std::span<const ClaimIssuer> issuers,
                                                     std::span<const std::string> members) {
  std::vector<std::string> errors;
  scope_errors(databases, kDatabaseWords, members, errors);
  std::set<std::string> variables;
  for (const auto& provider : env) {
    if (!variables.insert(provider.name).second) {
      errors.push_back("env '" + provider.name + "' is declared twice");
    }
    if (std::ranges::find(members, provider.service) == members.end()) {
      errors.push_back("env '" + provider.name + "' names service '" + provider.service +
                       "', which is not among the repos given");
    }
  }
  scope_errors(issuers, kIssuerWords, members, errors);
  return errors;
}

nlohmann::json contract_databases_json(std::span<const ContractDatabase> databases) { return scopes_json(databases); }

nlohmann::json claim_issuers_json(std::span<const ClaimIssuer> issuers) { return scopes_json(issuers); }

nlohmann::json env_providers_json(std::span<const EnvProvider> env) {
  auto array = nlohmann::json::array();
  for (const auto& provider : env) {
    array.push_back({{"name", provider.name}, {"service", provider.service}});
  }
  return array;
}

const ContractDatabase* declared_database(std::span<const ContractDatabase> databases, std::string_view repo) {
  return scope_of_repo(databases, repo);
}

const ClaimIssuer* declared_issuer(std::span<const ClaimIssuer> issuers, std::string_view repo) {
  return scope_of_repo(issuers, repo);
}

std::optional<IssuerClaim> issuer_claim(std::span<const ClaimIssuer> issuers, std::string_view id) {
  if (contract_kind_of(id) != "claim") {
    return std::nullopt;
  }
  const auto rest = id.substr(6);
  const auto colon = rest.find(':');
  if (colon == std::string_view::npos || colon + 1 >= rest.size()) {
    return std::nullopt;
  }
  const auto found = std::ranges::find(issuers, rest.substr(0, colon), &ClaimIssuer::name);
  if (found == issuers.end()) {
    return std::nullopt;
  }
  return IssuerClaim{.issuer = &*found, .name = rest.substr(colon + 1)};
}

std::optional<std::string> declared_contract_id(std::span<const ContractDatabase> databases, std::string_view repo,
                                                std::string_view id) {
  const auto parts = split_scoped(id);
  const auto* database = parts && parts->database == kLocalDatabase ? declared_database(databases, repo) : nullptr;
  if (database == nullptr) {
    return std::nullopt;
  }
  return std::string(parts->kind) + ":" + database->name + ":" + std::string(parts->name);
}

std::optional<std::string> crossing_id(std::span<const ContractDatabase> databases, std::span<const EnvProvider> env,
                                       std::span<const ClaimIssuer> issuers, std::string_view repo,
                                       std::string_view id) {
  if (auto declared = declared_contract_id(databases, repo, id)) {
    return declared;
  }
  if (const auto scoped = issuer_claim(issuers, id)) {
    // Already an issuer's crossing id (a seam's node): it crosses for the
    // issuer's members only.
    if (is_registered_jwt_claim(scoped->name) || !is_member(*scoped->issuer, repo)) {
      return std::nullopt;
    }
    return std::string(id);
  }
  if (const auto* issuer = contract_kind_of(id) == "claim" ? declared_issuer(issuers, repo) : nullptr) {
    const auto name = id.substr(6);
    if (is_registered_jwt_claim(name)) {
      return std::nullopt;
    }
    return "claim:" + issuer->name + ":" + std::string(name);
  }
  if (is_bridged_contract(id) || env_provider_of(env, id)) {
    return std::string(id);
  }
  return std::nullopt;
}

std::vector<std::string> contract_spellings(std::span<const ContractDatabase> databases,
                                            std::span<const EnvProvider> env, std::span<const ClaimIssuer> issuers,
                                            std::string_view repo, std::string_view crossing) {
  std::vector<std::string> ids;
  if (const auto scoped = issuer_claim(issuers, crossing)) {
    if (is_registered_jwt_claim(scoped->name)) {
      return ids;
    }
    // A member's graph holds the claim under its own name; no other repo
    // holds it at all.
    if (is_member(*scoped->issuer, repo)) {
      ids.push_back("claim:" + std::string(scoped->name));
    }
    return ids;
  }
  if (contract_kind_of(crossing) == "claim" && declared_issuer(issuers, repo) != nullptr) {
    return ids;  // a member's claims cross at its issuer's id, never unscoped
  }
  if (!is_bridged_contract(crossing) && !env_provider_of(env, crossing)) {
    return ids;
  }
  ids.emplace_back(crossing);
  if (auto local = local_contract_spelling(databases, repo, crossing)) {
    ids.push_back(std::move(*local));
  }
  return ids;
}

std::optional<std::string> local_contract_spelling(std::span<const ContractDatabase> databases, std::string_view repo,
                                                   std::string_view contract) {
  const auto parts = split_scoped(contract);
  if (!parts || parts->database == kLocalDatabase) {
    return std::nullopt;
  }
  for (const auto& database : databases) {
    if (database.name == parts->database && std::ranges::find(database.repos, repo) != database.repos.end()) {
      return std::string(parts->kind) + ":" + std::string(kLocalDatabase) + ":" + std::string(parts->name);
    }
  }
  return std::nullopt;
}

std::optional<std::string> env_provider_of(std::span<const EnvProvider> env, std::string_view id) {
  if (contract_kind_of(id) != "env") {
    return std::nullopt;
  }
  const auto name = id.substr(4);
  for (const auto& provider : env) {
    if (provider.name == name) {
      return provider.service;
    }
  }
  return std::nullopt;
}

}  // namespace cgraph
