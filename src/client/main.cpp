#include "cgraph/client_runtime.hpp"
#include "cgraph/engine.hpp"

#include <iostream>
#include <string>

namespace {

void print_usage() {
  std::cout << "Usage: cgraph-client [--root PATH] [--daemon PATH] <query|path|explain|impact|context|report|update|status|shutdown|remember|recall> [JSON params]\n";
}

}  // namespace

int main(int argc, char** argv) {
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

  const auto hooks = cgraph::default_client_runtime_hooks(request);
  const auto result = cgraph::send_thin_client_request(request, hooks);
  if (!result.response) {
    std::cerr << result.error << '\n';
    return 1;
  }

  std::cout << result.response->dump(2) << '\n';
  return 0;
}
