#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace re2 {
class RE2;
}

namespace cgraph {

// A regular expression that is safe to run over file contents.
//
// Backed by RE2, which matches in time linear in the input and with bounded
// stack. std::regex must not be used on text read from a project: libstdc++'s
// matcher recurses once per character a `*` or `+` consumes, so one long line
// overflowed the stack and killed the indexer, past any try/catch.
//
// Syntax is RE2's, which covers what the extractors use (classes, groups,
// alternation, `\b`, bounded repeats) and has no backreferences or lookaround.
// Text is matched as bytes, never decoded, so input that is not UTF-8 is fine.
// `\w`, `\b` and case folding are ASCII-only, as they were. One difference
// from std::regex: `\s` does not include the vertical tab.
class Pattern {
 public:
  enum class Case { Sensitive, Insensitive };

  explicit Pattern(std::string_view regex, Case letter_case = Case::Sensitive);
  ~Pattern();
  Pattern(const Pattern&) = delete;
  Pattern& operator=(const Pattern&) = delete;

  // One match: groups[0] is the whole match, groups[n] is capture n, each a
  // view into the searched text. A capture that took no part in the match has
  // a null data() pointer.
  using Groups = std::span<const std::string_view>;

  // Calls `visit` for every match in `text`, left to right, each search
  // starting where the previous match ended. After a match that consumed
  // nothing the search resumes one byte further on; std::regex_iterator would
  // first retry at the same position for a non-empty match, so a pattern that
  // can match the empty string finds fewer matches here than it did there.
  void for_each_match(std::string_view text, const std::function<void(Groups)>& visit) const;

  // The first match in `text`, or false. `groups` receives as many groups as it
  // has room for.
  [[nodiscard]] bool search(std::string_view text, std::span<std::string_view> groups = {}) const;

 private:
  std::unique_ptr<re2::RE2> re_;
};

// Offset of a group within the text it was matched in. The group must have
// taken part in the match: an optional capture that did not has no offset.
[[nodiscard]] inline std::size_t offset_in(std::string_view text, std::string_view group) {
  assert(group.data() != nullptr);
  return static_cast<std::size_t>(group.data() - text.data());
}

}  // namespace cgraph
