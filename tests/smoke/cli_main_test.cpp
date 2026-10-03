// End-to-end coverage of the cgraph CLI's output writing (src/cli/main.cpp):
// when a write fails part-way (a full disk), the command must exit non-zero,
// name the file, and leave no truncated file behind. The failure is simulated
// with RLIMIT_FSIZE in the child (SIGXFSZ ignored -> write() fails with EFBIG
// once a file reaches the limit), which truncates a file mid-write exactly like
// a full disk but is deterministic and unprivileged on Linux and macOS.

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

constexpr rlim_t kUnlimited = RLIM_INFINITY;

void expect(bool& ok, bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

struct RunResult {
  int exit_code = -1;
  std::string stderr_text;
};

// Runs `cgraph args...` with the given file-size limit. stderr is captured
// through a pipe (pipes are not subject to RLIMIT_FSIZE).
RunResult run_cgraph(const fs::path& cgraph, const std::vector<std::string>& args, rlim_t file_size_limit) {
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    return {};
  }
  const pid_t child = ::fork();
  if (child == 0) {
    ::close(pipe_fds[0]);
    ::dup2(pipe_fds[1], STDERR_FILENO);
    ::close(pipe_fds[1]);
    std::signal(SIGXFSZ, SIG_IGN);  // ignored dispositions survive exec
    rlimit limit{};
    ::getrlimit(RLIMIT_FSIZE, &limit);
    limit.rlim_cur = file_size_limit;
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      ::_exit(126);
    }
    std::vector<char*> argv;
    std::string program = cgraph.string();
    argv.push_back(program.data());
    std::vector<std::string> owned = args;
    for (auto& arg : owned) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    ::execv(program.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(pipe_fds[1]);
  RunResult result;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(pipe_fds[0], buffer, sizeof(buffer));
    if (count <= 0) {
      break;
    }
    result.stderr_text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(pipe_fds[0]);
  int status = 0;
  ::waitpid(child, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: cgraph_cli_main_test <cgraph binary> <one_shot fixture root>\n";
    return 2;
  }
  const fs::path cgraph = argv[1];
  const fs::path fixture = argv[2];
  bool ok = true;

  const auto root = fs::temp_directory_path() / ("cgraph_cli_main_test_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);

  // Unrestricted baseline: a one-shot build and a seam discover to feed fuse.
  const auto out = root / "out";
  auto run = run_cgraph(cgraph, {"--root", fixture.string(), "--out", out.string()}, kUnlimited);
  expect(ok, run.exit_code == 0, "unrestricted one-shot build exits 0: " + run.stderr_text);
  const auto graph = (out / "graph.json").string();
  const auto drop = root / "drop";
  run = run_cgraph(cgraph, {"seam", "discover", "--graph", "a=" + graph, "--graph", "b=" + graph, "--out", drop.string()},
                   kUnlimited);
  expect(ok, run.exit_code == 0, "unrestricted seam discover exits 0: " + run.stderr_text);
  const auto seam = (drop / "chunk_00.json").string();
  const std::vector<std::string> fuse_inputs{"seam", "fuse", "--seam", seam, "--graph", "a=" + graph, "--graph", "b=" + graph};

  // seam fuse: graph.json does not fit. A prior good graph.json must survive
  // untouched, the command must fail naming the file, and neither a temp file
  // nor the seam marker (which would make graphd serve the dir) may appear.
  const auto fused = root / "fused";
  fs::create_directories(fused);
  std::ofstream(fused / "graph.json") << "PRIOR\n";
  auto fuse_args = fuse_inputs;
  fuse_args.insert(fuse_args.end(), {"--out", fused.string()});
  run = run_cgraph(cgraph, fuse_args, 4096);
  expect(ok, run.exit_code != 0, "seam fuse exits non-zero when graph.json cannot be fully written");
  expect(ok, contains(run.stderr_text, (fused / "graph.json").string()),
         "seam fuse names the file it failed to write: " + run.stderr_text);
  expect(ok, read_file(fused / "graph.json") == "PRIOR\n", "seam fuse leaves the prior graph.json untouched");
  expect(ok, !fs::exists(fused / "graph.json.tmp"), "seam fuse leaves no temp file");
  expect(ok, !fs::exists(fused / ".cgraph-seam"), "seam fuse does not mark a failed output dir");

  // The same fuse, unrestricted, still succeeds.
  run = run_cgraph(cgraph, fuse_args, kUnlimited);
  expect(ok, run.exit_code == 0, "unrestricted seam fuse exits 0: " + run.stderr_text);
  expect(ok, read_file(fused / "graph.json").size() > 4096, "unrestricted seam fuse writes the full graph.json");

  // seam discover: the fragment does not fit.
  const auto small_drop = root / "small-drop";
  run = run_cgraph(cgraph,
                   {"seam", "discover", "--graph", "a=" + graph, "--graph", "b=" + graph, "--out", small_drop.string()},
                   64);
  expect(ok, run.exit_code != 0, "seam discover exits non-zero when chunk_00.json cannot be fully written");
  expect(ok, contains(run.stderr_text, (small_drop / "chunk_00.json").string()),
         "seam discover names the file it failed to write: " + run.stderr_text);
  expect(ok, !fs::exists(small_drop / "chunk_00.json"), "seam discover leaves no truncated chunk_00.json");
  expect(ok, !fs::exists(small_drop / "chunk_00.json.tmp"), "seam discover leaves no temp file");

  // One-shot build (write_exports): graph.html does not fit.
  const auto limited_out = root / "limited-out";
  run = run_cgraph(cgraph, {"--root", fixture.string(), "--out", limited_out.string()}, 4096);
  expect(ok, run.exit_code != 0, "one-shot build exits non-zero when an export cannot be fully written");
  expect(ok, contains(run.stderr_text, (limited_out / "graph.html").string()),
         "one-shot build names the file it failed to write: " + run.stderr_text);
  expect(ok, !fs::exists(limited_out / "graph.html"), "one-shot build leaves no truncated graph.html");
  expect(ok, !fs::exists(limited_out / "graph.html.tmp"), "one-shot build leaves no temp file");

  fs::remove_all(root);
  if (ok) {
    std::cout << "cli_main_test: ok\n";
  }
  return ok ? 0 : 1;
}
