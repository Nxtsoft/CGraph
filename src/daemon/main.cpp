#include "cgraph/daemon_lifecycle.hpp"
#include "cgraph/daemon_ops.hpp"
#include "cgraph/daemon_server.hpp"
#include "cgraph/engine.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/seam.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <string>

namespace {

void print_usage() {
  std::cout << "Usage: graphd [--root PATH] [--idle-timeout SECONDS] [--no-watch] [--version]\n"
               "             [--benchmark-query --graph PATH --query TEXT]\n";
}

int run(int argc, char** argv) {
  const auto info = cgraph::build_info();
  std::filesystem::path graph_path;
  std::filesystem::path root;
  std::string query;
  bool benchmark_query = false;
  bool watch = true;
  std::chrono::seconds idle_timeout{300};

  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--version") {
      std::cout << "graphd " << info.version << '\n';
      return 0;
    }
    if ((arg == "--root" || arg == "-r") && index + 1 < argc) {
      root = argv[++index];
      continue;
    }
    if (arg == "--idle-timeout" && index + 1 < argc) {
      idle_timeout = std::chrono::seconds(std::stoll(argv[++index]));
      continue;
    }
    if (arg == "--no-watch") {
      watch = false;
      continue;
    }
    if (arg == "--benchmark-query") {
      benchmark_query = true;
      continue;
    }
    if (arg == "--graph" && index + 1 < argc) {
      graph_path = argv[++index];
      continue;
    }
    if (arg == "--query" && index + 1 < argc) {
      query = argv[++index];
      continue;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    return 2;
  }

  if (benchmark_query) {
    if (graph_path.empty() || query.empty()) {
      std::cerr << "--benchmark-query requires --graph and --query\n";
      return 2;
    }
    cgraph::DaemonState state;
    if (!cgraph::load_graph_snapshot(state, graph_path)) {
      std::cerr << "failed to load graph: " << graph_path << '\n';
      return 1;
    }
    const auto response = cgraph::handle_daemon_request(state, cgraph::make_request("query", {{"q", query}}));
    if (!response.value("ok", false)) {
      std::cerr << response.value("error", std::string{"daemon query failed"}) << '\n';
      return 1;
    }
    std::cout << response.dump(2) << '\n';
    return 0;
  }

  if (root.empty()) {
    print_usage();
    return 2;
  }

  cgraph::DaemonServerOptions options;
  options.idle_timeout = idle_timeout;
  if (!watch) {
    options.code_poll_interval = std::chrono::milliseconds{0};  // 0 disables live watching
  }
  // A fused-seam output dir is served statically (read-only, no build/watch).
  if (cgraph::is_seam_directory(root)) {
    return cgraph::run_static_seam_server(root, options);
  }
  return cgraph::run_daemon_server(root, options);
}

}  // namespace

int main(int argc, char** argv) {
  // A failure the server does not handle itself (e.g. a filesystem_error
  // creating the socket directory, or an unreadable --idle-timeout) is logged
  // and exits non-zero; an escaped exception would abort with a core dump.
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "graphd: " << error.what() << '\n';
    return 1;
  }
}
