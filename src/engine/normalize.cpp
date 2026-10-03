#include "cgraph/normalize.hpp"

#include <utf8proc.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace cgraph {
namespace {

struct Utf8ProcDeleter {
  void operator()(utf8proc_uint8_t* ptr) const noexcept {
    std::free(ptr);
  }
};

using Utf8ProcBuffer = std::unique_ptr<utf8proc_uint8_t, Utf8ProcDeleter>;

[[nodiscard]] std::string map_utf8(std::string_view input, utf8proc_option_t options) {
  utf8proc_uint8_t* mapped = nullptr;
  const auto result = utf8proc_map(
      reinterpret_cast<const utf8proc_uint8_t*>(input.data()),
      static_cast<utf8proc_ssize_t>(input.size()),
      &mapped,
      options);
  if (result < 0 || mapped == nullptr) {
    return {};
  }

  Utf8ProcBuffer buffer(mapped);
  return std::string(reinterpret_cast<const char*>(buffer.get()), static_cast<std::size_t>(result));
}

[[nodiscard]] bool is_word_codepoint(utf8proc_int32_t codepoint) {
  if (codepoint == static_cast<utf8proc_int32_t>('_')) {
    return true;
  }

  switch (utf8proc_category(codepoint)) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
    case UTF8PROC_CATEGORY_ND:
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
      return true;
    default:
      return false;
  }
}

void append_codepoint(std::string& output, utf8proc_int32_t codepoint) {
  utf8proc_uint8_t encoded[4] = {};
  const auto length = utf8proc_encode_char(codepoint, encoded);
  if (length > 0) {
    output.append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(length));
  }
}

[[nodiscard]] std::string replace_non_word_runs(std::string_view normalized) {
  std::string output;
  output.reserve(normalized.size());

  bool pending_underscore = false;
  std::size_t offset = 0;
  while (offset < normalized.size()) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(normalized.data() + offset),
        static_cast<utf8proc_ssize_t>(normalized.size() - offset),
        &codepoint);

    if (consumed <= 0) {
      pending_underscore = !output.empty();
      ++offset;
      continue;
    }

    if (is_word_codepoint(codepoint)) {
      if (codepoint == static_cast<utf8proc_int32_t>('_')) {
        pending_underscore = !output.empty();
        offset += static_cast<std::size_t>(consumed);
        continue;
      }

      if (pending_underscore && !output.empty() && output.back() != '_') {
        output.push_back('_');
      }
      pending_underscore = false;
      append_codepoint(output, codepoint);
    } else {
      pending_underscore = !output.empty();
    }

    offset += static_cast<std::size_t>(consumed);
  }

  while (!output.empty() && output.back() == '_') {
    output.pop_back();
  }

  return output;
}

thread_local std::size_t make_id_count = 0;

}  // namespace

std::size_t make_id_calls() { return make_id_count; }

std::string make_id(std::string_view input) {
  ++make_id_count;
  constexpr auto nfkc_options = static_cast<utf8proc_option_t>(
      UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT);
  constexpr auto casefold_options = static_cast<utf8proc_option_t>(
      UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);

  const auto normalized = map_utf8(input, nfkc_options);
  const auto cleaned = replace_non_word_runs(normalized);
  return map_utf8(cleaned, casefold_options);
}

std::string relative_import_stub(std::string_view kind, const std::filesystem::path& joined) {
  std::size_t up = 0;
  std::string rest;
  for (const auto& part : joined.lexically_normal()) {
    const auto segment = part.generic_string();
    if (segment == ".." && rest.empty()) {
      ++up;
    } else if (!segment.empty() && segment != ".") {
      rest += rest.empty() ? segment : "/" + segment;
    }
  }
  std::string stub = up == 0 ? "import-relative-" : "import-relative-up-" + std::to_string(up) + "-";
  stub += kind;
  stub += ":";
  stub += rest;
  return stub;
}

}  // namespace cgraph
