#pragma once

#include <charconv>
#include <concepts>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace cgraph {

// A malformed command line. what() is the message without a program prefix;
// each executable's main prints `<program>: <what>` and exits 2.
class UsageError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

// The whole of `text` as a base-10 T, or nullopt when it has stray characters
// ("5x"), is empty, or does not fit T.
template <std::integral T>
[[nodiscard]] std::optional<T> parse_integer(std::string_view text) {
  T value{};
  const auto* end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, value);
  if (text.empty() || error != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

// The value of integer flag `flag`, or UsageError
// "<flag> expects an integer, got '<value>'".
template <std::integral T>
[[nodiscard]] T integer_flag(std::string_view flag, std::string_view value) {
  if (const auto parsed = parse_integer<T>(value)) {
    return *parsed;
  }
  throw UsageError(std::string(flag) + " expects an integer, got '" + std::string(value) + "'");
}

// The value of numeric flag `flag`, or UsageError
// "<flag> expects a number, got '<value>'".
[[nodiscard]] double number_flag(std::string_view flag, std::string_view value);

// "<demangled dynamic type>: <what()>", e.g. "std::out_of_range: map::at", for
// last-resort handlers where the type is the only clue to where it came from.
[[nodiscard]] std::string describe_exception(const std::exception& error);

}  // namespace cgraph
