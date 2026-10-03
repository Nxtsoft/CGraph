#include "cgraph/cli_support.hpp"
#include "cgraph/client_runtime.hpp"
#include "cgraph/mcp_server.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int run(int argc, char** argv) {
  cgraph::ClientRequest base;
  // Claude Code (and other hosts) set CLAUDE_PROJECT_DIR to the project root and
  // do not guarantee the server's working directory, so prefer it over cwd.
  if (const auto* project = std::getenv("CLAUDE_PROJECT_DIR"); project != nullptr && project[0] != '\0') {
    base.project_root = project;
  } else {
    base.project_root = std::filesystem::current_path();
  }
  // Daemon discovery (CGRAPH_DAEMON_PATH, graphd beside this binary) happens in
  // resolve_daemon_path via default_client_runtime_hooks; --daemon overrides it.
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if ((arg == "--root" || arg == "-r") && index + 1 < argc) {
      base.project_root = argv[++index];
    } else if (arg == "--daemon" && index + 1 < argc) {
      base.daemon_path = argv[++index];
    }
  }

  // Route every tool's daemon op to the resident per-project daemon through the
  // thin-client runtime (connect, auto-spawn, backoff) instead of answering from
  // an empty in-process state.
  const auto dispatch = [&](const nlohmann::json& daemon_request) -> nlohmann::json {
    cgraph::ClientRequest request = base;
    request.operation = daemon_request.value("op", std::string{});
    request.params = daemon_request.value("params", nlohmann::json::object());
    const auto result = cgraph::send_thin_client_request(request, cgraph::default_client_runtime_hooks(request));
    if (result.response) {
      return *result.response;
    }
    return nlohmann::json{{"ok", false}, {"error", result.error}};
  };

  std::string line;
  while (std::getline(std::cin, line)) {
    const auto request = nlohmann::json::parse(line, nullptr, false);
    if (request.is_discarded()) {
      std::cout << nlohmann::json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}}.dump() << '\n';
      continue;
    }
    const auto response = cgraph::handle_mcp_request(request, dispatch, [&](const nlohmann::json& arguments) {
      return cgraph::change_context_across_workspace(arguments, base);
    });
    if (!response.empty()) {
      std::cout << response.dump() << '\n';
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Per-request failures are answered as JSON-RPC errors (handle_mcp_request);
  // anything else (e.g. a filesystem_error resolving the working directory at
  // startup) is reported and exits non-zero instead of aborting.
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "cgraph-mcp: " << cgraph::describe_exception(error) << '\n';
    return 1;
  }
}
