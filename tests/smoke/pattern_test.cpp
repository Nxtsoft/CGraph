#include "cgraph/pattern.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cgraph::Pattern;

bool ok = true;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

// (offset, text) of capture `group` for every match.
std::vector<std::pair<std::size_t, std::string>> all(const Pattern& pattern, std::string_view text, std::size_t group) {
  std::vector<std::pair<std::size_t, std::string>> found;
  pattern.for_each_match(text, [&](Pattern::Groups groups) {
    found.emplace_back(cgraph::offset_in(text, groups[group]), std::string(groups[group]));
  });
  return found;
}

}  // namespace

int main() {
  using Found = std::vector<std::pair<std::size_t, std::string>>;

  // Matches come left to right, each search starting where the last match
  // ended, with captures reported as views into the searched text.
  const Pattern word{R"((\w+)=(\d+))"};
  expect(all(word, "a=1, bb=22;c=x d=4", 1) == Found{{0, "a"}, {5, "bb"}, {15, "d"}}, "capture 1 of each match");
  expect(all(word, "a=1, bb=22;c=x d=4", 2) == Found{{2, "1"}, {8, "22"}, {17, "4"}}, "capture 2 of each match");
  expect(all(word, "", 0).empty() && all(word, "no pairs here", 0).empty(), "no match, no call");

  // A capture that took no part in the match is null, not an empty view at
  // some offset: callers tell "absent" from "empty" by it.
  const Pattern optional{R"(x(a)?(b*)y)"};
  bool checked = false;
  optional.for_each_match("xy", [&](Pattern::Groups groups) {
    checked = groups.size() == 3 && groups[1].data() == nullptr && groups[2].data() != nullptr && groups[2].empty();
  });
  expect(checked, "unmatched capture is null, empty capture is not");

  // A pattern that can match nothing must still walk the whole text once.
  expect(all(Pattern{"a*"}, "baab", 0) == Found{{0, ""}, {1, "aa"}, {3, ""}, {4, ""}}, "empty matches advance");

  // \b looks at the character before the search position, so a match cannot
  // start in the middle of the word the previous match ended in.
  expect(all(Pattern{R"(\bid)"}, "id idid", 0) == Found{{0, "id"}, {3, "id"}}, "word boundary across matches");

  // Case folding, \w and \b are ASCII-only and text is bytes: a Latin-1 or
  // UTF-8 letter is neither a word character nor folded, and bytes that are
  // not UTF-8 are matched like any others.
  const Pattern insensitive{R"(\bselect\b)", Pattern::Case::Insensitive};
  expect(all(insensitive, "SELECT Select seLECT selects", 0).size() == 3, "ASCII case folding");
  expect(all(Pattern{R"(\w+)"}, "caf\xE9 na\xC3\xAFve", 0) == Found{{0, "caf"}, {5, "na"}, {9, "ve"}}, "non-ASCII is not a word byte");
  expect(all(Pattern{R"([^>]+)"}, "\xFF\xFE>\x80", 0) == Found{{0, "\xFF\xFE"}, {3, "\x80"}}, "arbitrary bytes match");
  expect(all(Pattern{"\xC9", Pattern::Case::Insensitive}, "\xE9\xC9", 0) == Found{{1, "\xC9"}},
         "a Latin-1 letter is a byte, not a letter to fold");

  // search: the first match only, into as many groups as the caller gives.
  std::string_view groups[3];
  expect(word.search("zz k=7 m=8", groups) && groups[0] == "k=7" && groups[1] == "k" && groups[2] == "7", "search fills groups");
  expect(word.search("k=7") && !word.search("k=v"), "search without groups");

  // The reason this class exists: a repetition that consumes megabytes. The
  // same patterns under std::regex overflowed the stack near 50 kB.
  const std::string tag = "<Project " + std::string(4 << 20, 'a') + ">";
  expect(all(Pattern{R"(<\s*(Project)\b[^>]*>)"}, tag, 1) == Found{{1, "Project"}}, "4 MB inside one repetition");
  expect(all(Pattern{R"(\b(?:public|\s)+\s+\w+\s+(\w+)\s*\()"}, std::string(4 << 20, ' ') + "public", 1).empty(),
         "4 MB of whitespace that ends in no match");

  // A pattern is a compiled-in constant, so a bad one is a programming error.
  bool threw = false;
  try {
    const Pattern broken{"(unclosed"};
  } catch (const std::logic_error&) {
    threw = true;
  }
  expect(threw, "an invalid pattern throws at construction");

  return ok ? 0 : 1;
}
