#include "cgraph/cli_support.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <typeinfo>

#if !defined(_MSC_VER)
#include <cxxabi.h>
#endif

namespace cgraph {

double number_flag(std::string_view flag, std::string_view value) {
  // strtod, not from_chars: libc++ on macOS lacks floating-point from_chars.
  const std::string text(value);
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(text.c_str(), &end);
  if (text.empty() || std::isspace(static_cast<unsigned char>(text.front())) != 0 ||
      end != text.c_str() + text.size() || errno == ERANGE) {
    throw UsageError(std::string(flag) + " expects a number, got '" + text + "'");
  }
  return parsed;
}

std::string describe_exception(const std::exception& error) {
#if defined(_MSC_VER)
  // MSVC's type_info::name() is already human-readable ("class std::out_of_range").
  return std::string(typeid(error).name()) + ": " + error.what();
#else
  const char* mangled = typeid(error).name();
  int status = 0;
  const std::unique_ptr<char, decltype(&std::free)> demangled(
      abi::__cxa_demangle(mangled, nullptr, nullptr, &status), &std::free);
  return std::string(status == 0 && demangled ? demangled.get() : mangled) + ": " + error.what();
#endif
}

}  // namespace cgraph
