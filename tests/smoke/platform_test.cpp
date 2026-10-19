#include "cgraph/platform.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

int fail(const std::string& message) {
  std::cerr << "platform_test: " << message << '\n';
  return 1;
}

// Child mode: spawn_detached starts this same binary with `--child FILE`; it
// writes its own argv[2] into FILE so the parent can check the argument came
// through the command line unchanged.
int run_child(const std::filesystem::path& marker) {
  std::ofstream out(marker, std::ios::binary);
  out << marker.string();
  return out ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--child") {
    return run_child(argv[2]);
  }

  // Command-line quoting, checked against the CommandLineToArgvW rules.
  const struct {
    std::wstring in;
    std::wstring out;
  } cases[] = {
      {L"plain", L"plain"},
      {L"", L"\"\""},
      {L"has space", L"\"has space\""},
      {L"C:\\dir with space\\", L"\"C:\\dir with space\\\\\""},
      {L"say \"hi\"", L"\"say \\\"hi\\\"\""},
      {L"back\\\"slash", L"\"back\\\\\\\"slash\""},
      {L"C:\\no\\space", L"C:\\no\\space"},
  };
  for (const auto& c : cases) {
    if (cgraph::quote_windows_argument(c.in) != c.out) {
      return fail("quote_windows_argument mismatch for case " + std::to_string(&c - cases));
    }
  }

#ifdef _WIN32
  if (cgraph::executable_file_name("graphd") != "graphd.exe") return fail("missing .exe suffix");
#else
  if (cgraph::executable_file_name("graphd") != "graphd") return fail("unexpected suffix");
#endif

  if (cgraph::current_process_id() == 0) return fail("process id is 0");
  if (cgraph::home_directory().empty()) return fail("no home directory");

  const auto self = cgraph::current_executable_path();
  if (self.stem() != "cgraph_platform_test" || !std::filesystem::exists(self)) {
    return fail("current_executable_path: " + self.string());
  }

  // A detached child receives an argument with spaces intact and runs to
  // completion on its own.
  const auto dir = std::filesystem::temp_directory_path() / "cgraph platform test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto marker = dir / "child marker.txt";
  if (!cgraph::spawn_detached(self, {"--child", marker})) {
    return fail("spawn_detached failed");
  }
  std::string seen;
  for (int attempt = 0; attempt < 200 && seen != marker.string(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ifstream in(marker, std::ios::binary);
    seen.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  if (seen != marker.string()) {
    return fail("child did not write its argument: got '" + seen + "'");
  }
#ifdef _WIN32
  // CreateProcessW reports a missing executable at once. (On POSIX fork
  // succeeds first and the child exits 127, so there is nothing to assert.)
  if (cgraph::spawn_detached(dir / "does-not-exist.exe", {})) {
    return fail("spawning a missing executable reported success");
  }
#endif

  if (!cgraph::restrict_directory_to_owner(dir)) return fail("restrict_directory_to_owner failed");
#ifndef _WIN32
  const auto group_or_other = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
  if ((std::filesystem::status(dir).permissions() & group_or_other) != std::filesystem::perms::none) {
    return fail("directory still reachable by group/others");
  }
#endif
  std::filesystem::remove_all(dir);
  return 0;
}
