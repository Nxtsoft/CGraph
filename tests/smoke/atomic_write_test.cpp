#include "cgraph/atomic_write.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

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

// True when `directory` holds no temp file (any name ending in ".tmp").
bool no_temp_files(const fs::path& directory) {
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (entry.path().filename().string().ends_with(".tmp")) {
      std::cerr << "leftover temp file: " << entry.path() << '\n';
      return false;
    }
  }
  return true;
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

// Two writers (threads in this process, or separate processes) replacing the
// same file `rounds` times each, each with its own full-size content. Every
// write must succeed and the file must always be exactly one writer's
// complete content: a temp name shared between writers would let them
// interleave into one temp file and rename a mixed file, or make one writer's
// rename fail because the other already moved the temp away.
bool concurrent_writers_never_mix(const fs::path& path, bool use_processes) {
  constexpr int rounds = 200;
  const std::string a(256 * 1024, 'a');
  const std::string b(256 * 1024, 'b');
  const auto writer = [&](const std::string& contents) {
    int failures = 0;
    for (int round = 0; round < rounds; ++round) {
      if (const auto error = cgraph::try_write_file_atomically(path, contents); !error.empty()) {
        if (failures++ == 0) {
          std::cerr << "concurrent writer failed: " << error << '\n';
        }
      }
      const auto now = read_file(path);
      if (now != a && now != b) {
        std::cerr << "concurrent writers produced a mixed or partial file (" << now.size() << " bytes)\n";
        ++failures;
      }
    }
    return failures;
  };
  int failures = 0;
  if (use_processes) {
    pid_t children[2];
    const std::string* contents[2] = {&a, &b};
    for (int index = 0; index < 2; ++index) {
      children[index] = ::fork();
      if (children[index] == 0) {
        ::_exit(writer(*contents[index]) == 0 ? 0 : 1);
      }
    }
    for (const auto child : children) {
      int status = 0;
      ::waitpid(child, &status, 0);
      failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
  } else {
    int failures_a = 0;
    int failures_b = 0;
    std::thread first([&] { failures_a = writer(a); });
    std::thread second([&] { failures_b = writer(b); });
    first.join();
    second.join();
    failures = failures_a + failures_b;
  }
  return failures == 0;
}

}  // namespace

int main() {
  bool ok = true;
  const auto root = fs::temp_directory_path() / ("cgraph_atomic_write_test_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);

  const auto path = root / "graph.json";

  // Success: exact bytes land at the path, no temp left behind, and a second
  // write replaces the first.
  expect(ok, cgraph::try_write_file_atomically(path, "first\n").empty(), "write succeeds");
  expect(ok, read_file(path) == "first\n", "written bytes are exact");
  expect(ok, no_temp_files(root), "no temp file after success");
  expect(ok, cgraph::try_write_file_atomically(path, "second\n").empty(), "overwrite succeeds");
  expect(ok, read_file(path) == "second\n", "overwrite replaces contents");

  // A write that runs out of space part-way: the helper must report it, the
  // prior file must be untouched (never truncated or replaced by a partial
  // file), and the partial temp must be removed.
  const std::string big(64 * 1024, 'x');
  expect(ok, write_under_file_size_limit(path, big, 4096) == 0, "a short write is reported, naming the path");
  expect(ok, read_file(path) == "second\n", "a failed write leaves the prior file untouched");
  expect(ok, no_temp_files(root), "a failed write removes its partial temp file");

  // Same, with no prior file: nothing at all may appear at the path.
  const auto fresh = root / "fresh.json";
  expect(ok, write_under_file_size_limit(fresh, big, 4096) == 0, "a short fresh write is reported");
  expect(ok, !fs::exists(fresh), "a failed fresh write leaves no file at the path");
  expect(ok, no_temp_files(root), "a failed fresh write leaves no temp file");

  // Concurrent writers to one destination never mix and never fail spuriously.
  expect(ok, concurrent_writers_never_mix(root / "threads.json", false),
         "two threads writing one file: every write succeeds, the file is always one writer's whole content");
  expect(ok, concurrent_writers_never_mix(root / "processes.json", true),
         "two processes writing one file: every write succeeds, the file is always one writer's whole content");

  expect(ok, no_temp_files(root), "concurrent writers leave no temp file");

  // A durable write lands the same bytes.
  expect(ok, cgraph::try_write_file_atomically(path, "durable\n", cgraph::WriteDurability::Durable).empty(),
         "durable write succeeds");
  expect(ok, read_file(path) == "durable\n", "durable write lands the exact bytes");

  // A temp file stranded by a crashed writer is swept by the next write to the
  // same path once it is over an hour old -- and nothing else is: not a fresh
  // temp (a writer may still be using it), not another file's temp, and not a
  // name outside the helper's `<name>.<pid>.<n>.tmp` pattern.
  const auto stale = root / "graph.json.123.4.tmp";
  const auto fresh_temp = root / "graph.json.123.5.tmp";
  const auto foreign_pattern = root / "graph.json.tmp";
  const auto other_file_temp = root / "other.json.123.4.tmp";
  for (const auto& stray : {stale, fresh_temp, foreign_pattern, other_file_temp}) {
    std::ofstream(stray) << "stray";
  }
  const auto two_hours_ago = fs::file_time_type::clock::now() - std::chrono::hours(2);
  for (const auto& old : {stale, foreign_pattern, other_file_temp}) {
    fs::last_write_time(old, two_hours_ago);
  }
  expect(ok, cgraph::try_write_file_atomically(path, "after crash\n").empty(), "write after a crash succeeds");
  expect(ok, !fs::exists(stale), "a stale temp of this path is swept");
  expect(ok, fs::exists(fresh_temp), "a fresh temp of this path is kept");
  expect(ok, fs::exists(foreign_pattern), "a name outside the helper's pattern is kept");
  expect(ok, fs::exists(other_file_temp), "another file's temp is kept");
  for (const auto& stray : {fresh_temp, foreign_pattern, other_file_temp}) {
    fs::remove(stray);
  }

  // The throwing form raises FileWriteError naming the path.
  const auto missing_dir_path = root / "no-such-dir" / "graph.json";
  bool threw = false;
  try {
    cgraph::write_file_atomically(missing_dir_path, "x");
  } catch (const cgraph::FileWriteError& error) {
    threw = contains(error.what(), missing_dir_path.string());
  }
  expect(ok, threw, "write_file_atomically throws FileWriteError naming the path");

  // create_output_directories: an empty path is the current directory (no-op),
  // an existing directory is fine, nested missing ones are created, and a
  // read-only parent raises FileWriteError in write_file_atomically's style --
  // never a std::filesystem::filesystem_error.
  cgraph::create_output_directories({});
  cgraph::create_output_directories(root);
  const auto nested = root / "a" / "b";
  cgraph::create_output_directories(nested);
  expect(ok, fs::is_directory(nested), "create_output_directories creates missing parents");
  if (::geteuid() != 0) {  // root bypasses directory permissions
    const auto read_only = root / "read-only";
    fs::create_directories(read_only);
    fs::permissions(read_only, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    const auto blocked = read_only / "out";
    std::string message;
    try {
      cgraph::create_output_directories(blocked);
    } catch (const cgraph::FileWriteError& error) {
      message = error.what();
    }
    expect(ok, message == "failed to write " + blocked.string() + ": cannot create directory (Permission denied)",
           ("create_output_directories under a read-only parent throws FileWriteError: " + message).c_str());
    fs::permissions(read_only, fs::perms::owner_all, fs::perm_options::replace);
  }

  fs::remove_all(root);
  if (ok) {
    std::cout << "atomic_write_test: ok\n";
  }
  return ok ? 0 : 1;
}
