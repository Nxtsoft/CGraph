#include "cgraph/client_runtime.hpp"
#include "cgraph/engine.hpp"

#include <cstdlib>
#include <iterator>
#include <chrono>
#include <exception>
#include <iostream>
#include <string>

namespace {

void print_usage() {
  std::cout << "Usage: cgraph-client [--root PATH] [--daemon PATH] <query|path|explain|impact|context|report|update|status|shutdown|remember|recall> [JSON params]\n"
               "       cgraph-client [--root PATH] cross-service '{\"file\": PATH, \"wait_ms\": N}'\n"
               "         the other services on the far side of each endpoint FILE serves or calls\n";
}

int run(int argc, char** argv) {
  const auto info = cgraph::build_info();
  cgraph::ClientRequest request{
      .project_root = std::filesystem::current_path(),
      .operation = "status",
  };

  bool operation_set = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--version") {
      std::cout << "cgraph-client " << info.version << '\n';
      return 0;
    }
    if ((arg == "--root" || arg == "-r") && i + 1 < argc) {
      request.project_root = argv[++i];
      continue;
    }
    if (arg == "--daemon" && i + 1 < argc) {
      request.daemon_path = argv[++i];
      continue;
    }
    if (!operation_set) {
      request.operation = arg;
      operation_set = true;
      continue;
    }
    try {
      request.params = nlohmann::json::parse(arg);
    } catch (const nlohmann::json::parse_error& error) {
      std::cerr << "invalid JSON params: " << error.what() << '\n';
      return 2;
    }
  }

  if (request.operation == "pre-edit") {
    // Claude Code PreToolUse hook: stdin is the hook input; print the hook
    // output only when there is something to say. Always exit 0.
    std::string text{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    const auto input = nlohmann::json::parse(text, nullptr, false);
    const char* wait_env = std::getenv("CGRAPH_HOOK_WAIT_MS");
    const auto wait = std::chrono::milliseconds(wait_env != nullptr ? std::atol(wait_env) : 3000);
    if (const auto output = cgraph::pre_edit_hook_output(input, request, wait)) {
      std::cout << output->dump() << '\n';
    }
    return 0;
  }
  if (request.operation == "cross-service") {
    if (!request.params.contains("file")) {
      std::cerr << "cross-service needs {\"file\": PATH}\n";
      return 2;
    }
    request.build_wait = std::chrono::milliseconds(request.params.value("wait_ms", 30000));
    std::cout << cgraph::cross_service_for_file(request, request.params.at("file").get<std::string>()).dump(2) << '\n';
    return 0;
  }

  const auto hooks = cgraph::default_client_runtime_hooks(request);
  const auto result = cgraph::send_thin_client_request(request, hooks);
  if (!result.response) {
    std::cerr << result.error << '\n';
    return 1;
  }

  std::cout << result.response->dump(2) << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Report any unhandled failure (e.g. a filesystem_error resolving the
  // project root) and exit non-zero instead of aborting with a core dump.
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "cgraph-client: " << error.what() << '\n';
    return 1;
  }
}
