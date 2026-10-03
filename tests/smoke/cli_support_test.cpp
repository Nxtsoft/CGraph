#include "cgraph/cli_support.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <system_error>

namespace {

void expect(bool& ok, bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

// The UsageError message `call` throws, or "<no throw>".
template <typename Call>
std::string usage_error_of(Call call) {
  try {
    (void)call();
  } catch (const cgraph::UsageError& error) {
    return error.what();
  }
  return "<no throw>";
}

}  // namespace

int main() {
  bool ok = true;

  // parse_integer takes the whole text or nothing: std::stoi accepted "5x" as 5.
  expect(ok, cgraph::parse_integer<int>("42") == 42, "parse_integer reads 42");
  expect(ok, cgraph::parse_integer<int>("-7") == -7, "parse_integer reads -7");
  expect(ok, !cgraph::parse_integer<int>("5x"), "parse_integer rejects trailing characters");
  expect(ok, !cgraph::parse_integer<int>(" 5"), "parse_integer rejects leading space");
  expect(ok, !cgraph::parse_integer<int>(""), "parse_integer rejects empty text");
  expect(ok, !cgraph::parse_integer<int>("99999999999"), "parse_integer rejects a value outside int");
  expect(ok, cgraph::parse_integer<long long>("99999999999") == 99999999999LL, "parse_integer reads a long long");

  expect(ok, cgraph::integer_flag<int>("--depth", "3") == 3, "integer_flag returns the value");
  const auto depth_error = usage_error_of([] { return cgraph::integer_flag<int>("--depth", "abc"); });
  expect(ok, depth_error == "--depth expects an integer, got 'abc'", "integer_flag names flag and value: " + depth_error);
  const auto range_error = usage_error_of([] { return cgraph::integer_flag<int>("--hops", "99999999999"); });
  expect(ok, range_error == "--hops expects an integer, got '99999999999'", "integer_flag rejects overflow: " + range_error);

  expect(ok, cgraph::number_flag("--threshold", "0.85") == 0.85, "number_flag returns the value");
  const auto threshold_error = usage_error_of([] { return cgraph::number_flag("--threshold", "0.8x"); });
  expect(ok, threshold_error == "--threshold expects a number, got '0.8x'",
         "number_flag rejects trailing characters: " + threshold_error);
  expect(ok, usage_error_of([] { return cgraph::number_flag("--threshold", ""); }) != "<no throw>",
         "number_flag rejects empty text");

  // describe_exception leads with the demangled dynamic type.
  try {
    const std::map<int, int> empty;
    (void)empty.at(1);
  } catch (const std::exception& error) {
    const auto text = cgraph::describe_exception(error);
    expect(ok, text == std::string("std::out_of_range: ") + error.what(), "describe_exception: " + text);
  }
  try {
    throw std::filesystem::filesystem_error("status", std::filesystem::path("/x"),
                                            std::make_error_code(std::errc::permission_denied));
  } catch (const std::exception& error) {
    const auto text = cgraph::describe_exception(error);
    expect(ok, text.starts_with("std::filesystem::") && text.find("filesystem_error: ") != std::string::npos &&
                   text.find("[/x]") != std::string::npos,
           "describe_exception names filesystem_error and keeps the path: " + text);
  }

  if (ok) {
    std::cout << "cli_support_test: ok\n";
  }
  return ok ? 0 : 1;
}
