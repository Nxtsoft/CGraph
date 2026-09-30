#include "cgraph/client_runtime.hpp"

#include "cgraph/change_context.hpp"
#include "cgraph/normalize.hpp"

#include "cgraph/daemon_endpoint.hpp"
#include "cgraph/daemon_server.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/workspace.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace cgraph {
namespace {

std::mutex& lock_map_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, std::weak_ptr<std::mutex>>& lock_map() {
  static std::unordered_map<std::string, std::weak_ptr<std::mutex>> locks;
  return locks;
}

std::shared_ptr<std::mutex> spawn_lock_for(const std::string& root_hash) {
  std::scoped_lock guard(lock_map_mutex());
  auto& weak = lock_map()[root_hash];
  auto lock = weak.lock();
  if (lock == nullptr) {
    lock = std::make_shared<std::mutex>();
    weak = lock;
  }
  return lock;
}

std::chrono::milliseconds backoff_for(const ClientRequest& request, int attempt) {
  const auto multiplier = 1 << std::min(attempt, 10);
  return request.initial_backoff * multiplier;
}

[[nodiscard]] bool waits_for_build(std::string_view operation) {
  static constexpr std::array<std::string_view, 7> kGraphReads{"query",   "path",   "explain", "impact",
                                                               "context", "report", "recall"};
  return std::find(kGraphReads.begin(), kGraphReads.end(), operation) != kGraphReads.end();
}

[[nodiscard]] bool still_building(const nlohmann::json& response) {
  const auto result = response.find("result");
  return result != response.end() && result->is_object() &&
         result->value("graph_state", std::string{}) == "building";
}

// Re-ask a daemon that answered from a graph it is still building, until the
// build publishes or `build_wait` runs out (a workspace request shares one
// `build_wait` across all its member asks). On timeout the last answer is kept,
// with its `graph_state: building` marker, so the caller still sees why.
void settle_building_answer(const ClientRequest& request, const ClientRuntimeHooks& hooks,
                            const DaemonIdentity& identity, const nlohmann::json& frame, ClientResult& result) {
  if (!result.response || !waits_for_build(request.operation) || request.build_wait.count() <= 0) {
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + request.build_wait;
  int attempt = 0;
  while (still_building(*result.response) && std::chrono::steady_clock::now() < deadline) {
    hooks.sleep(std::min(backoff_for(request, attempt++), std::chrono::milliseconds(250)));
    ++result.connect_attempts;
    if (auto response = hooks.connect(identity, frame); response.has_value()) {
      result.response = std::move(response);
    }
  }
}

[[nodiscard]] std::filesystem::path current_executable_path() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  buffer.resize(std::strlen(buffer.c_str()));
  std::error_code ec;
  const auto canonical = std::filesystem::canonical(buffer, ec);
  return ec ? std::filesystem::path{buffer} : canonical;
#elif defined(__linux__)
  std::error_code ec;
  const auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path{} : path;
#else
  return {};
#endif
}

}  // namespace

std::filesystem::path resolve_daemon_path(const std::filesystem::path& requested) {
  if (!requested.empty()) {
    return requested;
  }
  if (const char* env = std::getenv("CGRAPH_DAEMON_PATH"); env != nullptr && env[0] != '\0') {
    return env;
  }
  // Zero-config: graphd ships beside the client/MCP binary (installed layout),
  // or under the sibling daemon/ directory (the CMake build tree).
  if (const auto self = current_executable_path(); !self.empty()) {
    const auto bin_dir = self.parent_path();
    for (const auto& candidate : {bin_dir / "graphd", bin_dir.parent_path() / "daemon" / "graphd"}) {
      std::error_code ec;
      if (std::filesystem::exists(candidate, ec)) {
        return candidate;
      }
    }
  }
  return {};
}

ClientRuntimeHooks default_client_runtime_hooks(const ClientRequest& request) {
  ClientRuntimeHooks hooks;
  hooks.connect = [](const DaemonIdentity& identity, const nlohmann::json& frame) -> std::optional<nlohmann::json> {
    return request_over_unix_socket(unix_socket_path(identity), frame);
  };
  hooks.spawn = [daemon_path = resolve_daemon_path(request.daemon_path)](const DaemonIdentity& identity) {
    if (daemon_path.empty()) {
      return false;
    }
#ifdef _WIN32
    (void)identity;
    return false;
#else
    const auto pid = ::fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      // Detach the daemon from the client: a new session (no controlling
      // terminal) and stdio pointed at /dev/null, so the resident daemon never
      // holds the client's pipes open (which would make the client's caller
      // hang waiting for EOF) and outlives this short-lived client cleanly.
      ::setsid();
      if (const int devnull = ::open("/dev/null", O_RDWR); devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
          ::close(devnull);
        }
      }
      const auto daemon = daemon_path.string();
      const auto root = identity.project_root.string();
      ::execl(daemon.c_str(), daemon.c_str(), "--root", root.c_str(), nullptr);
      std::_Exit(127);
    }
    return true;
#endif
  };
  hooks.sleep = [](std::chrono::milliseconds delay) {
    std::this_thread::sleep_for(delay);
  };
  return hooks;
}

namespace {

// Asks every member of `workspace` through this same runtime. Forwarded asks
// never re-federate, and share one build wait for the whole request: members and
// contract hops are asked in turn, so each ask gets what is left.
[[nodiscard]] RepoAsk forwarding_ask(const ClientRequest& request, const ClientRuntimeHooks& hooks,
                                     std::shared_ptr<std::pair<std::size_t, int>> tallies) {
  // The shared wait starts at the first ask, not when the ask is made: a caller
  // may build its own snapshots first (change_context builds two).
  auto build_deadline = std::make_shared<std::optional<std::chrono::steady_clock::time_point>>();
  return [request, hooks, build_deadline, tallies](const WorkspaceRepo& repo, const std::string& op,
                                                   const nlohmann::json& params,
                                                   std::string& error) -> std::optional<nlohmann::json> {
    if (!*build_deadline) {
      *build_deadline = std::chrono::steady_clock::now() + request.build_wait;
    }
    ClientRequest forwarded = request;
    forwarded.project_root = repo.root;
    forwarded.operation = op;
    forwarded.params = params;
    forwarded.federate = false;
    forwarded.build_wait = std::max(std::chrono::milliseconds(0),
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        **build_deadline - std::chrono::steady_clock::now()));
    auto answer = send_thin_client_request(forwarded, hooks);
    tallies->first += answer.spawned ? 1 : 0;
    tallies->second += answer.connect_attempts;
    if (!answer.response) {
      error = answer.error;
      return std::nullopt;
    }
    return std::move(*answer.response);
  };
}

[[nodiscard]] ClientResult federate_through(const Workspace& workspace, const ClientRequest& request,
                                            const ClientRuntimeHooks& hooks) {
  ClientResult result;
  auto tallies = std::make_shared<std::pair<std::size_t, int>>(0, 0);
  result.response = federate_workspace_request(workspace, request.operation, request.params,
                                               forwarding_ask(request, hooks, tallies));
  result.spawned = tallies->first > 0;
  result.connect_attempts = tallies->second;
  return result;
}

}  // namespace

ClientResult send_thin_client_request(const ClientRequest& request, ClientRuntimeHooks hooks) {
  ClientResult result;
  if (request.operation.empty()) {
    result.error = "missing operation";
    return result;
  }
  // A workspace root is not a project: it federates the op to the member repos,
  // each of which is reached exactly as a lone project would be (same hooks, same
  // auto-spawn, same daemon per root). This is the only place federation is
  // entered, so the thin client and the MCP server both get it.
  if (request.federate && is_workspace_root(request.project_root)) {
    return federate_through(load_workspace(request.project_root), request, hooks);
  }
  // A root inside a workspace member answers from its own graph, except that the
  // consequences of a change (`impact`) and the route between two nodes (`path`)
  // cross the workspace: those are the questions another service can change.
  if (request.federate && (request.operation == "impact" || request.operation == "path")) {
    if (auto enclosing = find_enclosing_workspace(request.project_root)) {
      nlohmann::json tag{{"name", enclosing->workspace.name}, {"home", enclosing->home}};
      ClientResult result;
      if (!enclosing->workspace.ok()) {
        // The manifest lists this root but cannot be used: answer from home and
        // say why the other services are missing.
        ClientRequest alone = request;
        alone.federate = false;
        result = send_thin_client_request(alone, hooks);
        tag["errors"] = enclosing->workspace.errors;
      } else {
        // A content-root pin names the home repo's graph. Only the home ask
        // carries it, and a home pin that fails fails the request, as it does
        // for a lone project; other members answer from their current graphs.
        const bool pinned = request.params.contains("expected_content_root");
        std::optional<nlohmann::json> home_failure;
        auto tallies = std::make_shared<std::pair<std::size_t, int>>(0, 0);
        const auto forward = forwarding_ask(request, hooks, tallies);
        const RepoAsk ask = [&](const WorkspaceRepo& repo, const std::string& op, const nlohmann::json& params,
                                std::string& error) -> std::optional<nlohmann::json> {
          auto scoped = params;
          if (repo.name != enclosing->home) {
            scoped.erase("expected_content_root");
          }
          auto envelope = forward(repo, op, scoped, error);
          if (pinned && repo.name == enclosing->home && envelope && !envelope->value("ok", false) && !home_failure) {
            home_failure = envelope;
          }
          return envelope;
        };
        result.response = federate_workspace_request(enclosing->workspace, request.operation, request.params, ask);
        result.spawned = tallies->first > 0;
        result.connect_attempts = tallies->second;
        if (home_failure) {
          result.response = std::move(home_failure);
          return result;
        }
      }
      if (result.response && result.response->contains("result") && (*result.response)["result"].is_object()) {
        (*result.response)["result"]["workspace"] = std::move(tag);
      }
      return result;
    }
  }
  if (request.max_connect_attempts <= 0) {
    result.error = "max_connect_attempts must be positive";
    return result;
  }
  if (!hooks.connect) {
    result.error = "missing connect hook";
    return result;
  }
  if (!hooks.spawn) {
    result.error = "missing spawn hook";
    return result;
  }
  if (!hooks.sleep) {
    hooks.sleep = [](std::chrono::milliseconds delay) {
      std::this_thread::sleep_for(delay);
    };
  }

  const auto identity = daemon_identity_for(request.project_root);
  const auto frame = make_request(request.operation, request.params);

  ++result.connect_attempts;
  if (auto response = hooks.connect(identity, frame); response.has_value()) {
    result.response = std::move(response);
    settle_building_answer(request, hooks, identity, frame, result);
    return result;
  }

  const auto spawn_lock = spawn_lock_for(identity.root_hash);
  {
    std::scoped_lock guard(*spawn_lock);
    ++result.connect_attempts;
    if (auto response = hooks.connect(identity, frame); response.has_value()) {
      result.response = std::move(response);
      settle_building_answer(request, hooks, identity, frame, result);
      return result;
    }
    result.spawned = hooks.spawn(identity);
    if (!result.spawned) {
      result.error =
          "failed to spawn daemon: graphd not found (pass --daemon PATH, set CGRAPH_DAEMON_PATH, "
          "or install graphd next to this binary)";
      return result;
    }
  }

  for (int attempt = 0; attempt < request.max_connect_attempts; ++attempt) {
    hooks.sleep(backoff_for(request, attempt));
    ++result.connect_attempts;
    if (auto response = hooks.connect(identity, frame); response.has_value()) {
      result.response = std::move(response);
      settle_building_answer(request, hooks, identity, frame, result);
      return result;
    }
  }

  std::ostringstream error;
  error << "daemon did not accept connections after " << result.connect_attempts << " attempts";
  result.error = error.str();
  return result;
}

std::optional<CrossServiceScope> cross_service_scope_for(const ClientRequest& request, ClientRuntimeHooks hooks) {
  auto enclosing = find_enclosing_workspace(request.project_root);
  if (!enclosing) {
    return std::nullopt;
  }
  return CrossServiceScope{.enclosing = std::move(*enclosing),
                           .ask = forwarding_ask(request, hooks, std::make_shared<std::pair<std::size_t, int>>(0, 0))};
}

nlohmann::json change_context_across_workspace(const nlohmann::json& parameters, const ClientRequest& base) {
  ClientRequest request = base;
  request.project_root = parameters.value("target_root", std::string{});
  const auto scope = request.project_root.empty()
                         ? std::nullopt
                         : cross_service_scope_for(request, default_client_runtime_hooks(request));
  if (!scope) {
    return change_context(parameters);
  }
  const CrossServiceAsk ask{.enclosing = &scope->enclosing, .ask = scope->ask};
  return change_context(parameters, &ask);
}

nlohmann::json cross_service_for_file(const ClientRequest& base, const std::filesystem::path& file) {
  nlohmann::json out{{"file", file.generic_string()}, {"workspace", nullptr}, {"summary", nlohmann::json::array()}};
  const auto hooks = default_client_runtime_hooks(base);
  const auto scope = cross_service_scope_for(base, hooks);
  if (!scope) {
    return out;
  }
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(base.project_root, error);
  const auto relative = std::filesystem::weakly_canonical(file, error).lexically_relative(root).generic_string();
  out["file"] = relative;
  // The file's own endpoints: a route it declares is reached through `contains`,
  // one its functions call through `CONSUMES` (file -> function -> endpoint).
  ClientRequest home = base;
  home.operation = "impact";
  home.params = {{"id", make_id(relative)}, {"direction", "dependencies"}, {"max_depth", 2}};
  home.federate = false;
  const auto answer = send_thin_client_request(home, hooks);
  CrossServiceContracts contracts;
  if (answer.response && answer.response->value("ok", false)) {
    for (const auto& node : (*answer.response)["result"].value("nodes", nlohmann::json::array())) {
      const auto id = node.value("id", std::string{});
      const auto via = node.value("via", std::string{});
      if (!id.starts_with("endpoint:")) continue;
      auto& contract = contracts[id];
      if (via == "contains") {
        contract.roles.insert("serves");
        contract.rank = 0;
      } else if (via == "CONSUMES") {
        contract.roles.insert("consumes");
        contract.rank = std::min(contract.rank, 1);
      }
      if (contract.roles.empty()) contracts.erase(id);
    }
  }
  const CrossServiceAsk ask{.enclosing = &scope->enclosing, .ask = scope->ask};
  auto section = cross_service_section(ask, contracts);
  for (const auto& row : section["rows"]) {
    const auto contract = row.value("contract", std::string{}).substr(std::string_view("endpoint:").size());
    const bool consumer = row.value("relation", std::string{}) == "consumer";
    out["summary"].push_back(relative + (consumer ? " serves " : " calls ") + contract + (consumer ? ", called from " : ", served by ") +
                             row.value("repo", std::string{}) + " " + row.value("path", std::string{}) + ":" +
                             std::to_string(row.value("line", 0)) + " (" + row.value("label", std::string{}) + ")");
  }
  for (const auto& gap : section["unreachable"]) {
    out["summary"].push_back("could not ask " + gap.value("repo", std::string{}) + ": its callers are unknown");
  }
  for (const auto& repo : section["building"]) {
    out["summary"].push_back(repo.get<std::string>() + " is still building: its callers may be missing");
  }
  out["workspace"] = section.at("workspace");
  out["cross_service"] = std::move(section);
  return out;
}

std::optional<nlohmann::json> pre_edit_hook_output(const nlohmann::json& hook_input, const ClientRequest& base,
                                                   std::chrono::milliseconds wait) {
  if (!hook_input.is_object()) {
    return std::nullopt;
  }
  const auto tool_input = hook_input.value("tool_input", nlohmann::json::object());
  const auto file_text = tool_input.is_object() ? tool_input.value("file_path", std::string{}) : std::string{};
  std::error_code error;
  if (file_text.empty() || !std::filesystem::is_regular_file(file_text, error)) {
    return std::nullopt;
  }
  const auto file = std::filesystem::weakly_canonical(file_text, error);
  // The repository the file belongs to: the nearest directory with a `.git`
  // entry (a worktree's is a file), which the workspace search starts from.
  std::filesystem::path repo;
  for (auto dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
    if (std::filesystem::exists(dir / ".git", error)) {
      repo = dir;
      break;
    }
    if (dir == dir.parent_path()) break;
  }
  if (repo.empty()) {
    return std::nullopt;
  }
  ClientRequest request = base;
  request.project_root = repo;
  request.build_wait = wait;
  const auto found = cross_service_for_file(request, file);
  const auto& lines = found.at("summary");
  if (lines.empty()) {
    return std::nullopt;
  }
  std::string context = "cgraph: editing " + found.value("file", std::string{}) + " crosses into other services (" +
                        found.value("workspace", std::string{}) + " workspace):";
  for (const auto& line : lines) {
    context += "\n- " + line.get<std::string>();
  }
  context += "\nCheck those callers before changing a route, request or response shape.";
  return nlohmann::json{{"hookSpecificOutput", {{"hookEventName", "PreToolUse"}, {"additionalContext", context}}}};
}

}  // namespace cgraph
