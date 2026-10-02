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

[[nodiscard]] std::optional<std::string> database_entry_error(const ContractDatabase& database) {
  if (!plain_name(database.name) || database.name.find(':') != std::string::npos) {
    return "a `databases` entry needs a `name` with no `:` or whitespace: '" + database.name + "'";
  }
  if (database.name == kLocalDatabase) {
    return "database name `local` is reserved for tables no database is declared for";
  }
  if (database.repos.empty() || std::ranges::any_of(database.repos, [](const std::string& repo) { return repo.empty(); })) {
    return "database '" + database.name + "' needs a non-empty `repos` list of repo names";
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> env_entry_error(const EnvProvider& env) {
  if (!plain_name(env.name) || env.service.empty()) {
    return "each `env` entry needs a `name` with no whitespace and a `service`: '" + env.name + "'";
  }
  return std::nullopt;
}

}  // namespace

std::vector<ContractDatabase> parse_contract_databases(const nlohmann::json& entries, std::vector<std::string>& errors) {
  std::vector<ContractDatabase> databases;
  if (!entries.is_array()) {
    errors.push_back("`databases` must be an array of {\"name\", \"repos\"} objects");
    return databases;
  }
  for (const auto& entry : entries) {
    if (!entry.is_object()) {
      errors.push_back("each `databases` entry must be an object with `name` and `repos`");
      continue;
    }
    // A non-string member is an entry error, never a JSON type exception.
    const auto name = entry.find("name");
    const auto repos = entry.find("repos");
    if (name == entry.end() || !name->is_string() || repos == entry.end() || !repos->is_array() ||
        !std::ranges::all_of(*repos, [](const nlohmann::json& repo) { return repo.is_string(); })) {
      errors.push_back("a `databases` entry needs a string `name` and a `repos` array of strings: " + entry.dump());
      continue;
    }
    ContractDatabase database{.name = name->get<std::string>(), .repos = {}};
    for (const auto& repo : *repos) {
      auto repo_name = repo.get<std::string>();
      if (std::ranges::find(database.repos, repo_name) == database.repos.end()) {
        database.repos.push_back(std::move(repo_name));
      }
    }
    if (auto error = database_entry_error(database)) {
      errors.push_back(std::move(*error));
      continue;
    }
    databases.push_back(std::move(database));
  }
  return databases;
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
  auto parsed = parse_contract_databases(
      nlohmann::json::array({{{"name", std::string(flag.substr(0, eq))}, {"repos", std::move(repos)}}}), errors);
  if (!errors.empty()) {
    error = errors.front();
    return std::nullopt;
  }
  return std::move(parsed.front());
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
                                                     std::span<const std::string> members) {
  std::vector<std::string> errors;
  std::set<std::string> names;
  std::map<std::string, std::string> database_of;  // repo -> the first database declaring it
  for (const auto& database : databases) {
    if (!names.insert(database.name).second) {
      errors.push_back("database '" + database.name + "' is declared twice");
    }
    for (const auto& repo : database.repos) {
      if (std::ranges::find(members, repo) == members.end()) {
        errors.push_back("database '" + database.name + "' names repo '" + repo + "', which is not among the repos given");
      }
      const auto [slot, inserted] = database_of.emplace(repo, database.name);
      if (!inserted && slot->second != database.name) {
        errors.push_back("repo '" + repo + "' is declared in two databases ('" + slot->second + "', '" + database.name +
                         "'); a table it names could not say which");
      }
    }
  }
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
  return errors;
}

nlohmann::json contract_databases_json(std::span<const ContractDatabase> databases) {
  auto array = nlohmann::json::array();
  for (const auto& database : databases) {
    array.push_back({{"name", database.name}, {"repos", database.repos}});
  }
  return array;
}

nlohmann::json env_providers_json(std::span<const EnvProvider> env) {
  auto array = nlohmann::json::array();
  for (const auto& provider : env) {
    array.push_back({{"name", provider.name}, {"service", provider.service}});
  }
  return array;
}

std::optional<std::string> declared_contract_id(std::span<const ContractDatabase> databases, std::string_view repo,
                                                std::string_view id) {
  const auto parts = split_scoped(id);
  if (!parts || parts->database != kLocalDatabase) {
    return std::nullopt;
  }
  for (const auto& database : databases) {
    if (std::ranges::find(database.repos, repo) != database.repos.end()) {
      return std::string(parts->kind) + ":" + database.name + ":" + std::string(parts->name);
    }
  }
  return std::nullopt;
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
