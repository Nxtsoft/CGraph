#include "cgraph/atomic_write.hpp"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

void expect(bool& ok, bool condition, const char* what) {
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

// Runs try_write_file_atomically in a child whose file-size limit is
// `limit_bytes` (SIGXFSZ ignored, so write() fails with EFBIG once the file
// reaches the limit -- exactly how a full disk truncates a file mid-write, but
// deterministic and unprivileged on Linux and macOS). The child exits 0 only
// when the helper reported a failure naming `path`.
int write_under_file_size_limit(const fs::path& path, const std::string& contents, rlim_t limit_bytes) {
  const pid_t child = ::fork();
  if (child == 0) {
    std::signal(SIGXFSZ, SIG_IGN);
    rlimit limit{};
    ::getrlimit(RLIMIT_FSIZE, &limit);
    limit.rlim_cur = limit_bytes;
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      ::_exit(3);
    }
    const auto error = cgraph::try_write_file_atomically(path, contents);
    if (error.empty()) {
      ::_exit(1);
    }
    std::cerr << "child reported: " << error << '\n';
    ::_exit(contains(error, path.string()) ? 0 : 2);
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

}  // namespace

int main() {
  bool ok = true;
  const auto root = fs::temp_directory_path() / ("cgraph_atomic_write_test_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);

  const auto path = root / "graph.json";
  const auto temp_path = root / "graph.json.tmp";

  // Success: exact bytes land at the path, no temp left behind, and a second
  // write replaces the first.
  expect(ok, cgraph::try_write_file_atomically(path, "first\n").empty(), "write succeeds");
  expect(ok, read_file(path) == "first\n", "written bytes are exact");
  expect(ok, !fs::exists(temp_path), "no temp file after success");
  expect(ok, cgraph::try_write_file_atomically(path, "second\n").empty(), "overwrite succeeds");
  expect(ok, read_file(path) == "second\n", "overwrite replaces contents");

  // A write that runs out of space part-way: the helper must report it, the
  // prior file must be untouched (never truncated or replaced by a partial
  // file), and the partial temp must be removed.
  const std::string big(64 * 1024, 'x');
  expect(ok, write_under_file_size_limit(path, big, 4096) == 0, "a short write is reported, naming the path");
  expect(ok, read_file(path) == "second\n", "a failed write leaves the prior file untouched");
  expect(ok, !fs::exists(temp_path), "a failed write removes its partial temp file");

  // Same, with no prior file: nothing at all may appear at the path.
  const auto fresh = root / "fresh.json";
  expect(ok, write_under_file_size_limit(fresh, big, 4096) == 0, "a short fresh write is reported");
  expect(ok, !fs::exists(fresh), "a failed fresh write leaves no file at the path");
  expect(ok, !fs::exists(root / "fresh.json.tmp"), "a failed fresh write leaves no temp file");

  // The throwing form raises FileWriteError naming the path.
  const auto missing_dir_path = root / "no-such-dir" / "graph.json";
  bool threw = false;
  try {
    cgraph::write_file_atomically(missing_dir_path, "x");
  } catch (const cgraph::FileWriteError& error) {
    threw = contains(error.what(), missing_dir_path.string());
  }
  expect(ok, threw, "write_file_atomically throws FileWriteError naming the path");

  fs::remove_all(root);
  if (ok) {
    std::cout << "atomic_write_test: ok\n";
  }
  return ok ? 0 : 1;
}
