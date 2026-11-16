#include "cgraph/pattern.hpp"

#include <re2/re2.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace cgraph {

Pattern::Pattern(std::string_view regex, Case letter_case) {
  re2::RE2::Options options;
  options.set_encoding(re2::RE2::Options::EncodingLatin1);  // match bytes; sources are not always UTF-8
  options.set_case_sensitive(letter_case == Case::Sensitive);
  options.set_log_errors(false);
  re_ = std::make_unique<re2::RE2>(regex, options);
  if (!re_->ok()) {
    // Patterns are compiled-in constants, so this is a programming error.
    throw std::logic_error("invalid pattern `" + std::string(regex) + "`: " + re_->error());
  }
}

Pattern::~Pattern() = default;

void Pattern::for_each_match(std::string_view text, const std::function<void(Groups)>& visit) const {
  std::vector<std::string_view> groups(static_cast<std::size_t>(re_->NumberOfCapturingGroups()) + 1);
  std::size_t pos = 0;
  while (pos <= text.size() && re_->Match(text, pos, text.size(), re2::RE2::UNANCHORED, groups.data(),
                                          static_cast<int>(groups.size()))) {
    visit(groups);
    const auto end = offset_in(text, groups[0]) + groups[0].size();
    pos = groups[0].empty() ? end + 1 : end;  // an empty match must still move forward
  }
}

bool Pattern::search(std::string_view text, std::span<std::string_view> groups) const {
  return re_->Match(text, 0, text.size(), re2::RE2::UNANCHORED, groups.data(), static_cast<int>(groups.size()));
}

}  // namespace cgraph
