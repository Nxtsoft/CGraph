#include "cgraph/spring_actuator.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/normalize.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace cgraph {
namespace {

constexpr std::string_view kAppRelation = "actuator_app";
constexpr std::string_view kConfigRelation = "actuator_config";
constexpr std::string_view kResources = "src/main/resources";

// --- shared text helpers ------------------------------------------------------

[[nodiscard]] std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

[[nodiscard]] std::uint32_t line_at(std::string_view source, std::size_t offset) {
  const auto end = source.begin() + static_cast<std::ptrdiff_t>(std::min(offset, source.size()));
  return static_cast<std::uint32_t>(std::count(source.begin(), end, '\n') + 1);
}

// Spring's relaxed binding: `base-path`, `basePath` and `base_path` bind the
// same property. Each segment is lowercased with `-` and `_` dropped.
[[nodiscard]] std::string relaxed_key(std::string_view key) {
  std::string out;
  out.reserve(key.size());
  for (const char ch : key) {
    if (ch == '-' || ch == '_') {
      continue;
    }
    out.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
  }
  return out;
}

// An endpoint id as the exposure filter compares it: lowercase alphanumerics.
[[nodiscard]] std::string endpoint_id(std::string_view id) {
  std::string out;
  for (const char ch : id) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      out.push_back(ch);
    } else if (ch >= 'A' && ch <= 'Z') {
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else if (ch == '*') {
      out.push_back(ch);
    }
  }
  return out;
}

// Resolves `${NAME:default}` placeholders to their defaults, the value the code
// runs with when nothing in the environment overrides it. A placeholder without
// a default is unknowable here: nullopt.
[[nodiscard]] std::optional<std::string> resolve_placeholders(std::string_view value) {
  std::string out;
  for (std::size_t index = 0; index < value.size();) {
    const auto placeholder = find_spring_placeholder(value, index);
    const auto open = placeholder.open;
    if (open == std::string_view::npos) {
      out.append(value.substr(index));
      break;
    }
    out.append(value.substr(index, open - index));
    const auto close = placeholder.close;
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    const auto inner = placeholder.inner;
    const auto colon = inner.find(':');
    if (colon == std::string_view::npos || inner.find("${") != std::string_view::npos) {
      return std::nullopt;
    }
    out.append(inner.substr(colon + 1));
    index = close + 1;
  }
  return out;
}

// The keys that decide which actuator paths a configuration serves.
[[nodiscard]] bool relevant_key(std::string_view key) {
  return key.starts_with("management.") || key == "server.port" || key == "server.servlet.contextpath" ||
         key == "spring.webflux.basepath" || key == "spring.mvc.servlet.path" ||
         key == "spring.config.activate.onprofile" || key == "spring.profiles" ||
         key == "spring.main.webapplicationtype";
}

// Whether a (relaxed) key is, or is a parent of, a key that decides the paths.
// A parent the reader cannot open (an anchor, a tag, a flow mapping, a merge)
// may hold any of them.
[[nodiscard]] bool may_hold_relevant(std::string_view key) {
  if (key.empty() || key == "management" || relevant_key(key)) {
    return true;
  }
  static constexpr std::array<std::string_view, 8> kParents = {
      "server", "server.servlet", "spring", "spring.webflux", "spring.mvc", "spring.mvc.servlet",
      "spring.config", "spring.main"};
  return std::ranges::find(kParents, key) != kParents.end() || key == "spring.config.activate";
}

// --- application config parsing ---------------------------------------------

struct ConfigEntry {
  std::string key;  // as written, dotted
  std::string value;
  bool known = true;  // false for YAML the reader does not model (anchors, block scalars, flow maps)
  std::uint32_t line = 1;
};

struct ConfigDocument {
  std::vector<ConfigEntry> entries;
  std::unordered_map<std::string, std::size_t> lists;  // list-valued key -> its entry, so appends stay linear
  bool unreadable = false;  // a structure that may hold a deciding key could not be read
};

// Appends `item` to a list-valued entry (YAML sequences, `key[0]=` properties).
void append_item(ConfigDocument& document, const std::string& key, std::string item, bool known, std::uint32_t line) {
  const auto [slot, inserted] = document.lists.emplace(key, document.entries.size());
  if (inserted) {
    document.entries.push_back(ConfigEntry{.key = key, .value = std::move(item), .known = known, .line = line});
    return;
  }
  auto& entry = document.entries[slot->second];
  entry.value += "," + item;
  entry.known = entry.known && known;
}


// How deep flow sequences nest before the value is not modeled.
constexpr int kMaxFlowDepth = 8;

// A YAML scalar's value, or nullopt for a form the reader does not model.
[[nodiscard]] std::optional<std::string> yaml_scalar(std::string_view text, int depth = 0) {
  text = trim(text);
  if (text.empty()) {
    return std::string{};
  }
  if (text.front() == '"' || text.front() == '\'') {
    const char quote = text.front();
    if (text.size() < 2 || text.back() != quote) {
      return std::nullopt;
    }
    std::string out;
    const auto body = text.substr(1, text.size() - 2);
    for (std::size_t index = 0; index < body.size(); ++index) {
      if (quote == '\'' && body[index] == '\'' && index + 1 < body.size() && body[index + 1] == '\'') {
        out.push_back('\'');
        ++index;
      } else if (quote == '"' && body[index] == '\\' && index + 1 < body.size()) {
        out.push_back(body[++index]);
      } else {
        out.push_back(body[index]);
      }
    }
    return out;
  }
  if (text.front() == '&' || text.front() == '*' || text.front() == '!' || text.front() == '|' ||
      text.front() == '>' || text.front() == '{') {
    return std::nullopt;
  }
  if (text.front() == '[') {
    if (text.back() != ']' || depth >= kMaxFlowDepth) {
      return std::nullopt;
    }
    std::string out;
    auto body = text.substr(1, text.size() - 2);
    while (!body.empty()) {
      const auto comma = body.find(',');
      const auto item = yaml_scalar(body.substr(0, comma), depth + 1);
      if (!item) {
        return std::nullopt;
      }
      if (!item->empty()) {
        out += (out.empty() ? "" : ",") + *item;
      }
      body = comma == std::string_view::npos ? std::string_view{} : body.substr(comma + 1);
    }
    return out;
  }
  return std::string(text);
}

// The block-mapping subset of YAML Spring config files are written in: nested
// `key: value` maps, scalar sequences, `---` documents. Keys flatten to dotted
// paths the way Spring binds them.
[[nodiscard]] std::vector<ConfigDocument> parse_yaml(std::string_view source) {
  struct Frame {
    int indent;
    std::string key;
    std::uint32_t line;
    bool opaque;  // a mapping inside a sequence item: nothing below it is read
  };
  std::vector<ConfigDocument> documents(1);
  std::vector<Frame> stack;
  int block_indent = -1;  // inside a `|` / `>` block scalar indented beyond this
  std::uint32_t line_number = 0;
  for (std::size_t start = 0; start <= source.size();) {
    const auto newline = source.find('\n', start);
    auto line = source.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
    start = newline == std::string_view::npos ? source.size() + 1 : newline + 1;
    ++line_number;
    int indent = 0;
    while (static_cast<std::size_t>(indent) < line.size() && line[static_cast<std::size_t>(indent)] == ' ') {
      ++indent;
    }
    const auto content = trim(strip_yaml_comment(line.substr(static_cast<std::size_t>(indent))));
    if (block_indent >= 0) {
      if (content.empty() || indent > block_indent) {
        continue;
      }
      block_indent = -1;
    }
    if (content.empty()) {
      continue;
    }
    if (indent == 0 && (content == "---" || content.starts_with("--- ") || content == "...")) {
      documents.emplace_back();
      stack.clear();
      continue;
    }
    if (content.starts_with("%")) {
      continue;  // a directive
    }
    const bool item = content == "-" || content.starts_with("- ");
    while (!stack.empty() && (item ? stack.back().indent > indent : stack.back().indent >= indent)) {
      stack.pop_back();
    }
    if (!stack.empty() && stack.back().opaque) {
      continue;
    }
    auto& document = documents.back();
    if (item) {
      if (stack.empty()) {
        continue;  // a top-level sequence binds nothing
      }
      const auto value = trim(content.substr(1));
      const auto colon = value.find(": ");
      if (value.empty() || colon != std::string_view::npos || value.ends_with(':') || value.starts_with("- ")) {
        stack.push_back(Frame{.indent = indent, .key = stack.back().key, .line = line_number, .opaque = true});
        continue;  // a mapping or nested sequence inside a sequence: not modeled
      }
      const auto scalar = yaml_scalar(value);
      document.unreadable = document.unreadable || (!scalar && may_hold_relevant(relaxed_key(stack.back().key)));
      append_item(document, stack.back().key, scalar.value_or(""), scalar.has_value(), stack.back().line);
      continue;
    }
    // `key: value` or `key:`; a quoted key keeps its colons.
    std::string key;
    std::string_view rest;
    if (content.front() == '"' || content.front() == '\'') {
      const auto close = content.find(content.front(), 1);
      if (close == std::string_view::npos || close + 1 >= content.size() || content[close + 1] != ':') {
        continue;
      }
      key = std::string(content.substr(1, close - 1));
      rest = content.substr(close + 2);
    } else {
      std::size_t colon = std::string_view::npos;
      for (std::size_t index = 0; index < content.size(); ++index) {
        if (content[index] == ':' && (index + 1 == content.size() || content[index + 1] == ' ')) {
          colon = index;
          break;
        }
      }
      if (colon == std::string_view::npos) {
        continue;
      }
      key = std::string(trim(content.substr(0, colon)));
      rest = content.substr(colon + 1);
    }
    const auto full = stack.empty() ? key : stack.back().key + "." + key;
    rest = trim(rest);
    if (rest.empty()) {
      stack.push_back(Frame{.indent = indent, .key = full, .line = line_number, .opaque = false});
      continue;
    }
    // A merge key, a flow mapping, or an anchor or tag opening a nested block
    // (`management: &m`, `management: !!map`): what lies beneath cannot be
    // placed. If it may hold a deciding key the document is unreadable;
    // either way its children are skipped rather than mis-nested.
    const bool opens_block = (rest.front() == '&' || rest.front() == '!') && rest.find(' ') == std::string_view::npos;
    if (key == "<<" || rest.front() == '{' || opens_block) {
      const auto parent = key == "<<" ? (stack.empty() ? std::string{} : stack.back().key) : full;
      document.unreadable = document.unreadable || may_hold_relevant(relaxed_key(parent));
      stack.push_back(Frame{.indent = indent, .key = full, .line = line_number, .opaque = true});
      continue;
    }
    if (rest.front() == '|' || rest.front() == '>') {
      block_indent = indent;
    }
    const auto scalar = yaml_scalar(rest);
    if (!scalar) {
      // An alias (`management: *m`), a tag then an anchor (`!!map &m`), a block
      // scalar: a value the reader cannot place. If it may hold a deciding
      // key the document is unreadable, and lines beneath it are skipped.
      document.unreadable = document.unreadable || may_hold_relevant(relaxed_key(full));
      stack.push_back(Frame{.indent = indent, .key = full, .line = line_number, .opaque = true});
    }
    document.entries.push_back(
        ConfigEntry{.key = full, .value = scalar.value_or(""), .known = scalar.has_value(), .line = line_number});
  }
  return documents;
}

// `.properties` with Spring's `#---` document separator. Keys end at the first
// unescaped `=`, `:` or whitespace; `key[0]=` items join into one list.
[[nodiscard]] std::vector<ConfigDocument> parse_properties(std::string_view source) {
  std::vector<ConfigDocument> documents(1);
  std::uint32_t line_number = 0;
  for (std::size_t start = 0; start < source.size();) {
    ++line_number;
    const auto first_line = line_number;
    std::string logical;
    // Join continuation lines (an odd number of trailing backslashes).
    while (start < source.size()) {
      const auto newline = source.find('\n', start);
      auto line = source.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
      start = newline == std::string_view::npos ? source.size() : newline + 1;
      if (line.ends_with('\r')) {
        line.remove_suffix(1);
      }
      const auto piece = trim(line);
      std::size_t slashes = 0;
      while (slashes < piece.size() && piece[piece.size() - 1 - slashes] == '\\') {
        ++slashes;
      }
      if (slashes % 2 == 1) {
        logical.append(piece.substr(0, piece.size() - 1));
        ++line_number;
        continue;
      }
      logical.append(piece);
      break;
    }
    const auto text = trim(logical);
    if (text == "#---" || text == "!---") {
      documents.emplace_back();
      continue;
    }
    if (text.empty() || text.front() == '#' || text.front() == '!') {
      continue;
    }
    std::string key;
    std::size_t index = 0;
    for (; index < text.size(); ++index) {
      const char ch = text[index];
      if (ch == '\\' && index + 1 < text.size()) {
        key.push_back(text[++index]);
        continue;
      }
      if (ch == '=' || ch == ':' || ch == ' ' || ch == '\t') {
        break;
      }
      key.push_back(ch);
    }
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
      ++index;
    }
    if (index < text.size() && (text[index] == '=' || text[index] == ':')) {
      ++index;
    }
    std::string value;
    const auto rest = trim(text.substr(std::min(index, text.size())));
    for (std::size_t at = 0; at < rest.size(); ++at) {
      if (rest[at] == '\\' && at + 1 < rest.size()) {
        const char next = rest[++at];
        value.push_back(next == 't' ? '\t' : next == 'n' ? '\n' : next);
      } else {
        value.push_back(rest[at]);
      }
    }
    auto& document = documents.back();
    if (const auto open = key.rfind('['); open != std::string::npos && key.ends_with(']')) {
      append_item(document, key.substr(0, open), std::move(value), true, first_line);
      continue;
    }
    document.entries.push_back(ConfigEntry{.key = std::move(key), .value = std::move(value), .known = true, .line = first_line});
  }
  return documents;
}

// --- build file reading -------------------------------------------------------

// The source with comments blanked to spaces (newlines kept, so offsets and
// lines still match). `xml` blanks `<!-- -->`, otherwise `//` and `/* */`
// outside string literals.
[[nodiscard]] std::string blank_comments(std::string_view source, bool xml) {
  std::string out(source);
  const auto blank = [&](std::size_t from, std::size_t to) {
    for (auto index = from; index < to && index < out.size(); ++index) {
      if (out[index] != '\n') {
        out[index] = ' ';
      }
    }
  };
  if (xml) {
    for (auto open = out.find("<!--"); open != std::string::npos; open = out.find("<!--", open)) {
      const auto close = out.find("-->", open + 4);
      const auto end = close == std::string::npos ? out.size() : close + 3;
      blank(open, end);
      open = end;
    }
    return out;
  }
  char quote = 0;
  for (std::size_t index = 0; index < out.size(); ++index) {
    const char ch = out[index];
    if (quote != 0) {
      if (ch == '\\') {
        ++index;
      } else if (ch == quote || ch == '\n') {
        quote = 0;
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '/' && index + 1 < out.size() && out[index + 1] == '/') {
      const auto end = out.find('\n', index);
      blank(index, end == std::string::npos ? out.size() : end);
      index = end == std::string::npos ? out.size() : end;
    } else if (ch == '/' && index + 1 < out.size() && out[index + 1] == '*') {
      const auto close = out.find("*/", index + 2);
      const auto end = close == std::string::npos ? out.size() : close + 2;
      blank(index, end);
      index = end - 1;
    }
  }
  return out;
}

// Bound on the backward scan over a block's `(...)` arguments, so adversarial
// parentheses stay linear.
constexpr std::size_t kMaxBlockHeader = 512;

// The names of the Gradle blocks enclosing each of `offsets` (ascending):
// `dependencies` inside `subprojects` is {"subprojects", "dependencies"}, the
// identifier before each unclosed `{`. One pass over the text; strings are
// skipped.
[[nodiscard]] std::vector<std::vector<std::string>> enclosing_blocks(std::string_view text,
                                                                     std::span<const std::size_t> offsets) {
  std::vector<std::vector<std::string>> result;
  result.reserve(offsets.size());
  std::vector<std::string> blocks;
  char quote = 0;
  std::size_t next = 0;
  for (std::size_t index = 0; index <= text.size() && next < offsets.size(); ++index) {
    while (next < offsets.size() && offsets[next] <= index) {
      result.push_back(blocks);
      ++next;
    }
    if (index == text.size()) {
      break;
    }
    const char ch = text[index];
    if (quote != 0) {
      if (ch == '\\') {
        ++index;
      } else if (ch == quote || ch == '\n') {
        quote = 0;
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '{') {
      // `name {`, `name(...) {` and `name("x") {` all name the block `name`.
      const auto floor = index > kMaxBlockHeader ? index - kMaxBlockHeader : 0;
      auto end = index;
      while (end > floor && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' || text[end - 1] == '\r')) {
        --end;
      }
      if (end > floor && text[end - 1] == ')') {
        int depth = 0;
        while (end > floor) {
          --end;
          if (text[end] == ')') {
            ++depth;
          } else if (text[end] == '(' && --depth == 0) {
            break;
          }
        }
      }
      auto begin = end;
      while (begin > floor && (std::isalnum(static_cast<unsigned char>(text[begin - 1])) != 0 || text[begin - 1] == '_')) {
        --begin;
      }
      blocks.emplace_back(text.substr(begin, end - begin));
    } else if (ch == '}' && !blocks.empty()) {
      blocks.pop_back();
    }
  }
  while (result.size() < offsets.size()) {
    result.push_back(blocks);
  }
  return result;
}

struct BuildFacts {
  std::optional<std::size_t> actuator;  // offset of the actuator dependency
  bool boot = false;                    // the Spring Boot plugin applied / the Boot parent: an application
  int boot_major = 0;                   // the Boot version the build declares, 0 when it names none
  bool servlet = false;                 // spring-boot-starter-web / -webmvc
  bool reactive = false;                // spring-boot-starter-webflux
  bool prometheus = false;              // io.micrometer:micrometer-registry-prometheus
  bool cache = false;                   // spring-boot-starter-cache / spring-boot-cache (the caches endpoint, Boot 4)
};

void note_dependency(BuildFacts& facts, std::string_view group, std::string_view artifact, std::size_t offset) {
  if (group == "org.springframework.boot") {
    if (artifact == "spring-boot-starter-actuator" && !facts.actuator) {
      facts.actuator = offset;
    } else if (artifact == "spring-boot-starter-web" || artifact == "spring-boot-starter-webmvc") {
      facts.servlet = true;
    } else if (artifact == "spring-boot-starter-webflux") {
      facts.reactive = true;
    } else if (artifact == "spring-boot-starter-cache" || artifact == "spring-boot-cache") {
      facts.cache = true;
    }
  } else if (group == "io.micrometer" && artifact == "micrometer-registry-prometheus") {
    facts.prometheus = true;  // not `-simpleclient`, which Boot 4.1 does not support
  }
}

// The major version a `4.1.1`-style version names, 0 when it is not a literal.
[[nodiscard]] int major_of(std::string_view version) {
  int major = 0;
  std::size_t index = 0;
  for (; index < version.size() && index < 4 && version[index] >= '0' && version[index] <= '9'; ++index) {
    major = major * 10 + (version[index] - '0');
  }
  return index > 0 && (index == version.size() || version[index] == '.') ? major : 0;
}

// Gradle (Kotlin or Groovy DSL): dependencies on the runtime classpath, in the
// project's own `dependencies` block (not `subprojects`/`allprojects`), and the
// `org.springframework.boot` plugin applied in its `plugins` block (not `apply
// false`) or by `apply plugin:`, with the version the plugin declares.
[[nodiscard]] BuildFacts read_gradle(std::string_view source) {
  const auto text = blank_comments(source, false);
  BuildFacts facts;
  enum class Kind { Dependency, Plugin, Applied };
  struct Match {
    std::size_t offset;
    Kind kind;
    std::string first;   // group, or the plugin's version
    std::string second;  // artifact, or "false" for `apply false`
  };
  std::vector<Match> matches;
  static const std::regex coordinate{
      R"re(\b(implementation|api|runtimeOnly|compile|runtime)\s*\(?\s*["']([\w.\-]+):([\w.\-]+)(?::[^"'\n]{0,128})?["'])re"};
  static const std::regex named{
      R"re(\b(implementation|api|runtimeOnly|compile|runtime)\s*\(?\s*group\s*[:=]\s*["']([\w.\-]+)["']\s*,\s*name\s*[:=]\s*["']([\w.\-]+)["'])re"};
  for (const auto* pattern : {&coordinate, &named}) {
    for (auto it = std::sregex_iterator(text.begin(), text.end(), *pattern); it != std::sregex_iterator(); ++it) {
      matches.push_back(Match{static_cast<std::size_t>(it->position(0)), Kind::Dependency, (*it)[2].str(), (*it)[3].str()});
    }
  }
  // The plugin id, then the rest of its statement (to the end of the line or a
  // `;`): `version "3.4.0"`, `version bootVersion`, `version(libs...)`,
  // `.version("x")`, and `apply false` / `apply(false)` / `.apply(false)` in
  // any of those spellings. Only a literal version is read.
  static const std::regex plugin{R"re(\bid\s*\(?\s*["']org\.springframework\.boot["']\s*\)?([^;\n]{0,256}))re"};
  static const std::regex literal_version{R"re(\bversion\s*\(?\s*["']([^"'\n]{1,32})["'])re"};
  static const std::regex apply_false{R"re(\bapply\s*\(?\s*false\b)re"};
  for (auto it = std::sregex_iterator(text.begin(), text.end(), plugin); it != std::sregex_iterator(); ++it) {
    const auto rest = (*it)[1].str();
    std::smatch version;
    const bool literal = std::regex_search(rest, version, literal_version);
    matches.push_back(Match{static_cast<std::size_t>(it->position(0)), Kind::Plugin, literal ? version[1].str() : "",
                            std::regex_search(rest, apply_false) ? "false" : ""});
  }
  static const std::regex applied{R"re(\bapply\s*\(?\s*plugin\s*[:=]\s*["']org\.springframework\.boot["'])re"};
  for (auto it = std::sregex_iterator(text.begin(), text.end(), applied); it != std::sregex_iterator(); ++it) {
    matches.push_back(Match{static_cast<std::size_t>(it->position(0)), Kind::Applied, {}, {}});
  }
  std::ranges::sort(matches, {}, &Match::offset);
  std::vector<std::size_t> offsets;
  offsets.reserve(matches.size());
  for (const auto& match : matches) {
    offsets.push_back(match.offset);
  }
  const auto blocks = enclosing_blocks(text, offsets);
  for (std::size_t index = 0; index < matches.size(); ++index) {
    const auto& match = matches[index];
    const auto own = [&](std::string_view block) { return blocks[index].size() == 1 && blocks[index].front() == block; };
    if (match.kind == Kind::Dependency && own("dependencies")) {
      note_dependency(facts, match.first, match.second, match.offset);
    } else if (match.kind == Kind::Plugin && own("plugins") && match.second.empty()) {
      facts.boot = true;
      facts.boot_major = std::max(facts.boot_major, major_of(match.first));
    } else if (match.kind == Kind::Applied && blocks[index].empty()) {
      facts.boot = true;  // the version comes from the buildscript classpath: not read
    }
  }
  return facts;
}

// Maven: `<dependency>` elements outside `<dependencyManagement>`, `<plugins>`
// and `<profiles>` whose scope keeps them at runtime, and the Boot parent or
// a `spring-boot-maven-plugin` in `<plugins>` (not only `<pluginManagement>`).
// A `<packaging>pom</packaging>` project is an aggregator, not an application.
[[nodiscard]] BuildFacts read_pom(std::string_view source) {
  const auto text = blank_comments(source, true);
  BuildFacts facts;
  const auto ranges_of = [&](std::string_view tag) {
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    const auto open_tag = "<" + std::string(tag) + ">";
    const auto close_tag = "</" + std::string(tag) + ">";
    for (auto open = text.find(open_tag); open != std::string::npos; open = text.find(open_tag, open + 1)) {
      const auto close = text.find(close_tag, open);
      ranges.emplace_back(open, close == std::string::npos ? text.size() : close);
      if (close == std::string::npos) {
        break;  // unclosed: it runs to the end, and so would every later one
      }
    }
    return ranges;
  };
  const auto inside = [](const auto& ranges, std::size_t offset) {
    return std::ranges::any_of(ranges, [&](const auto& range) { return range.first < offset && offset < range.second; });
  };
  const auto managed = ranges_of("dependencyManagement");
  const auto plugins = ranges_of("plugins");
  const auto profiles = ranges_of("profiles");
  const auto plugin_management = ranges_of("pluginManagement");
  const auto element = [](std::string_view block, std::string_view tag) {
    const auto open_tag = "<" + std::string(tag) + ">";
    const auto open = block.find(open_tag);
    if (open == std::string_view::npos) {
      return std::string{};
    }
    const auto from = open + open_tag.size();
    const auto close = block.find("</", from);
    return std::string(trim(block.substr(from, close == std::string_view::npos ? std::string_view::npos : close - from)));
  };
  for (auto open = text.find("<dependency>"); open != std::string::npos; open = text.find("<dependency>", open + 1)) {
    const auto close = text.find("</dependency>", open);
    const auto block = std::string_view(text).substr(open, close == std::string::npos ? std::string::npos : close - open);
    const auto scope = element(block, "scope");
    if (inside(managed, open) || inside(plugins, open) || inside(profiles, open) || scope == "test" ||
        scope == "provided" || scope == "import") {
      continue;
    }
    note_dependency(facts, element(block, "groupId"), element(block, "artifactId"), open);
  }
  for (const auto& [open, close] : ranges_of("parent")) {
    const auto block = std::string_view(text).substr(open, close - open);
    if (block.find("<artifactId>spring-boot-starter-parent</artifactId>") != std::string_view::npos) {
      facts.boot = true;
      facts.boot_major = std::max(facts.boot_major, major_of(element(block, "version")));
    }
  }
  for (const auto& [open, close] : plugins) {
    if (inside(plugin_management, open) || inside(profiles, open)) {
      continue;
    }
    const auto block = std::string_view(text).substr(open, close - open);
    for (auto plugin = block.find("<plugin>"); plugin != std::string_view::npos; plugin = block.find("<plugin>", plugin + 1)) {
      const auto end = block.find("</plugin>", plugin);
      const auto body = block.substr(plugin, end == std::string_view::npos ? std::string_view::npos : end - plugin);
      if (element(body, "artifactId") == "spring-boot-maven-plugin") {
        facts.boot = true;
        facts.boot_major = std::max(facts.boot_major, major_of(element(body, "version")));
      }
    }
  }
  // The project's own packaging: the first <packaging> outside the parent.
  const auto parents = ranges_of("parent");
  for (auto open = text.find("<packaging>"); open != std::string::npos; open = text.find("<packaging>", open + 1)) {
    if (!inside(parents, open)) {
      facts.boot = facts.boot && element(std::string_view(text).substr(open), "packaging") != "pom";
      break;
    }
  }
  return facts;
}

[[nodiscard]] std::string directory_of(std::string_view relative_path) {
  const auto slash = relative_path.rfind('/');
  return slash == std::string_view::npos ? std::string{} : std::string(relative_path.substr(0, slash));
}

// --- resolution ---------------------------------------------------------------

enum class Access { None, ReadOnly, Unrestricted };

struct Operation {
  std::string_view method;  // lowercase, as a `file_route` context spells it
  std::string_view path;    // beneath the endpoint's own path
};

enum class Needs { Nothing, Prometheus, Cache };

struct BuiltinEndpoint {
  std::string_view id;
  std::array<Operation, 4> operations;
  std::size_t operation_count;
  Access default_access;
  Needs needs;
};

// The built-in web endpoints whose availability needs nothing beyond a web
// application with the actuator, or one more dependency: `prometheus` its
// registry, `caches` the spring-boot-cache module (Boot 4.1 auto-configures the
// endpoint there, and neither the actuator nor the web starter pulls it in).
// With the web operations Spring Boot maps for them; `health` answers its
// groups and components through `{*path}`.
constexpr std::array<BuiltinEndpoint, 15> kEndpoints = {{
    {"health", {{{"get", ""}, {"get", "/{*path}"}}}, 2, Access::Unrestricted, Needs::Nothing},
    {"info", {{{"get", ""}}}, 1, Access::Unrestricted, Needs::Nothing},
    {"beans", {{{"get", ""}}}, 1, Access::Unrestricted, Needs::Nothing},
    {"caches", {{{"get", ""}, {"delete", ""}, {"get", "/{cache}"}, {"delete", "/{cache}"}}}, 4, Access::Unrestricted, Needs::Cache},
    {"conditions", {{{"get", ""}}}, 1, Access::Unrestricted, Needs::Nothing},
    {"configprops", {{{"get", ""}, {"get", "/{prefix}"}}}, 2, Access::Unrestricted, Needs::Nothing},
    {"env", {{{"get", ""}, {"get", "/{toMatch}"}}}, 2, Access::Unrestricted, Needs::Nothing},
    {"loggers", {{{"get", ""}, {"get", "/{name}"}, {"post", "/{name}"}}}, 3, Access::Unrestricted, Needs::Nothing},
    {"mappings", {{{"get", ""}}}, 1, Access::Unrestricted, Needs::Nothing},
    {"metrics", {{{"get", ""}, {"get", "/{requiredMetricName}"}}}, 2, Access::Unrestricted, Needs::Nothing},
    {"scheduledtasks", {{{"get", ""}}}, 1, Access::Unrestricted, Needs::Nothing},
    {"threaddump", {{{"get", ""}}}, 1, Access::Unrestricted, Needs::Nothing},
    {"heapdump", {{{"get", ""}}}, 1, Access::None, Needs::Nothing},
    {"shutdown", {{{"post", ""}}}, 1, Access::None, Needs::Nothing},
    {"prometheus", {{{"get", ""}}}, 1, Access::Unrestricted, Needs::Prometheus},
}};

struct App {
  std::string node_id;
  std::string source_file;
  bool servlet = false;  // spring-boot-starter-web / -webmvc
  bool webflux = false;  // spring-boot-starter-webflux
  bool prometheus = false;
  bool cache = false;
  int boot_major = 0;  // 0: the build names no Boot version
};

struct Document {
  std::string profile;                   // the file's profile, "" for the base file
  std::vector<std::string> on_profiles;  // `spring.config.activate.on-profile`, empty when unset
  bool unmatchable = false;              // a profile expression: applies to no configuration modeled
  bool unreadable = false;               // a structure that may hold a deciding key could not be read
  std::array<int, 3> rank{};             // file tier, format, document index: later wins
  std::string exposure_node;
  std::string source_file;
  nlohmann::json props;                  // relaxed key -> string, or null when unknowable
  std::vector<std::string> groups;       // health group names as written
};

struct Value {
  std::optional<std::string> text;  // nullopt: unknowable
  const Document* from;
};

[[nodiscard]] std::optional<Access> access_of(std::string_view value) {
  const auto spelled = relaxed_key(value);
  if (spelled == "none" || spelled == "false") {
    return Access::None;
  }
  if (spelled == "readonly") {
    return Access::ReadOnly;
  }
  if (spelled == "unrestricted" || spelled == "true") {
    return Access::Unrestricted;
  }
  return std::nullopt;
}

// Spring's cleanBasePath: one trailing `/` dropped, `/` alone the root.
[[nodiscard]] std::optional<std::string> base_path_of(std::string value) {
  if (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  if (!value.empty() && value.front() != '/') {
    return std::nullopt;  // "Base path must start with '/' or be empty": the app refuses to start
  }
  return value;
}

// Spring's String-to-Boolean conversion: true/on/yes/1 and false/off/no/0.
[[nodiscard]] std::optional<bool> boolean_of(std::string_view value) {
  const auto spelled = relaxed_key(value);
  if (spelled == "true" || spelled == "on" || spelled == "yes" || spelled == "1") {
    return true;
  }
  if (spelled == "false" || spelled == "off" || spelled == "no" || spelled == "0") {
    return false;
  }
  return std::nullopt;
}

struct Route {
  std::string method;
  std::string path;
  std::string handler;
};

// The routes one configuration serves, or nullopt when a deciding value is
// unknowable here.
[[nodiscard]] std::optional<std::vector<Route>> configuration_routes(const std::map<std::string, Value>& values,
                                                                     const std::set<std::string>& groups,
                                                                     const App& app) {
  bool unknown = false;
  const auto get = [&](const std::string& key) -> std::optional<std::string> {
    const auto slot = values.find(key);
    if (slot == values.end()) {
      return std::nullopt;
    }
    if (!slot->second.text) {
      unknown = true;
      return std::nullopt;
    }
    return std::string(trim(*slot->second.text));
  };
  const auto ids_of = [&](const std::string& key, std::string_view fallback) {
    std::set<std::string> ids;
    const auto value = get(key).value_or(std::string(fallback));
    for (std::size_t start = 0; start <= value.size();) {
      const auto comma = value.find(',', start);
      const auto id = endpoint_id(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (!id.empty()) {
        ids.insert(id);
      }
      start = comma == std::string::npos ? value.size() + 1 : comma + 1;
    }
    return ids;
  };
  const auto include = ids_of("management.endpoints.web.exposure.include", "health");
  const auto exclude = ids_of("management.endpoints.web.exposure.exclude", "");
  const auto access_key = [&](const std::string& key) -> std::optional<Access> {
    const auto value = get(key);
    if (!value) {
      return std::nullopt;
    }
    const auto access = access_of(*value);
    unknown = unknown || !access;
    return access;
  };
  auto default_access = access_key("management.endpoints.access.default");
  if (!default_access) {
    default_access = access_key("management.endpoints.enabledbydefault");
  }
  const auto max_permitted = access_key("management.endpoints.access.maxpermitted").value_or(Access::Unrestricted);

  // `spring.main.web-application-type` overrides the stack the classpath picks
  // (servlet when both starters are there); `none` runs no web server, and a
  // stack whose starter is missing does not start.
  bool reactive = !app.servlet;
  if (const auto type = get("spring.main.webapplicationtype")) {
    const auto spelled = relaxed_key(*type);
    if (spelled == "none" || (spelled == "servlet" && !app.servlet) || (spelled == "reactive" && !app.webflux)) {
      return std::vector<Route>{};
    }
    if (spelled != "servlet" && spelled != "reactive") {
      return std::nullopt;
    }
    reactive = spelled == "reactive";
  }

  // Where the endpoints sit, as ManagementPortType.get (Boot 4.1.1) decides: a
  // negative management port disables them; it is the main server when unset,
  // when it is 8080 and the server port is unset, or when it is not 0 and equals
  // the server port; otherwise it is a separate server under its own base path.
  // A port that is not an integer fails startup.
  const auto port_of = [&](const std::string& key) -> std::optional<long long> {
    const auto value = get(key);
    if (!value) {
      return std::nullopt;
    }
    long long port = 0;
    const auto* end = value->data() + value->size();
    const auto [last, error] = std::from_chars(value->data(), end, port);
    if (error != std::errc{} || last != end) {
      unknown = true;
      return std::nullopt;
    }
    return port;
  };
  const auto management_port = port_of("management.server.port");
  const auto server_port = port_of("server.port");
  if (unknown) {
    return std::nullopt;
  }
  if (management_port && *management_port < 0) {
    return std::vector<Route>{};
  }
  const bool same_server = !management_port || (!server_port && *management_port == 8080) ||
                           (*management_port != 0 && management_port == server_port);
  std::string prefix;
  if (!same_server) {
    prefix = get("management.server.basepath").value_or("");
  } else if (reactive) {
    prefix = get("spring.webflux.basepath").value_or("");
  } else {
    const auto servlet_path = get("spring.mvc.servlet.path").value_or("/");
    if (servlet_path != "/" && !servlet_path.empty()) {
      return std::nullopt;  // the dispatcher servlet's own path: not modeled
    }
    prefix = get("server.servlet.contextpath").value_or("");
  }
  const auto base_path = base_path_of(get("management.endpoints.web.basepath").value_or("/actuator"));
  if (!base_path) {
    return std::vector<Route>{};
  }
  const auto exposure = values.find("management.endpoints.web.exposure.include");
  const auto handler_of = [&]() -> std::string {
    if (exposure != values.end() && exposure->second.from != nullptr && !exposure->second.from->exposure_node.empty()) {
      return exposure->second.from->exposure_node;
    }
    return app.node_id;
  };
  const auto handler = handler_of();
  std::vector<Route> routes;
  const auto root = join_route_path(prefix, *base_path);
  bool any = false;
  for (const auto& endpoint : kEndpoints) {
    const std::string id(endpoint.id);
    if ((endpoint.needs == Needs::Prometheus && !app.prometheus) || (endpoint.needs == Needs::Cache && !app.cache)) {
      continue;
    }
    if (!(include.contains("*") || include.contains(id)) || exclude.contains("*") || exclude.contains(id)) {
      continue;
    }
    auto access = access_key("management.endpoint." + id + ".access");
    if (!access) {
      access = access_key("management.endpoint." + id + ".enabled");
    }
    auto effective = access.value_or(default_access.value_or(endpoint.default_access));
    effective = std::min(effective, max_permitted);
    if (effective == Access::None) {
      continue;
    }
    const auto path = join_route_path(root, get("management.endpoints.web.pathmapping." + id).value_or(id));
    any = true;
    for (std::size_t index = 0; index < endpoint.operation_count; ++index) {
      const auto& operation = endpoint.operations[index];
      if (effective == Access::ReadOnly && operation.method != "get") {
        continue;
      }
      routes.push_back(Route{std::string(operation.method), join_route_path(path, operation.path), handler});
    }
    if (id == "health") {
      // The liveness and readiness groups: Boot 4 enables them unless
      // `probes.enabled` is false (AvailabilityProbesAutoConfiguration,
      // matchIfMissing = true); Boot 3 only when it is true (or on Kubernetes,
      // which config cannot tell). An unknown Boot version counts as off.
      std::set<std::string> named(groups);
      const auto probes = get("management.endpoint.health.probes.enabled");
      const auto enabled = probes ? boolean_of(*probes) : std::optional<bool>(app.boot_major >= 4);
      if (!enabled) {
        unknown = true;
      } else if (*enabled) {
        named.insert("liveness");
        named.insert("readiness");
      }
      for (const auto& group : named) {
        routes.push_back(Route{"get", join_route_path(path, group), handler});
      }
    }
  }
  // The discovery page lists the exposed endpoints at the base path.
  if (any && !base_path->empty() &&
      relaxed_key(get("management.endpoints.web.discovery.enabled").value_or("true")) != "false") {
    routes.insert(routes.begin(), Route{"get", root, handler});
  }
  if (unknown) {
    return std::nullopt;
  }
  return routes;
}

}  // namespace

// The text of a line before a `#` comment (YAML: at the start or after
// whitespace, outside quotes).
std::string_view strip_yaml_comment(std::string_view line) {
  char quote = 0;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char ch = line[index];
    if (quote != 0) {
      if (ch == quote) {
        quote = 0;
      }
      continue;
    }
    if ((ch == '"' || ch == '\'') && (index == 0 || line[index - 1] == ' ' || line[index - 1] == ':' ||
                                      line[index - 1] == '[' || line[index - 1] == ',' || line[index - 1] == '-')) {
      quote = ch;
    } else if (ch == '#' && (index == 0 || line[index - 1] == ' ' || line[index - 1] == '\t')) {
      return line.substr(0, index);
    }
  }
  return line;
}

SpringPlaceholder find_spring_placeholder(std::string_view value, std::size_t from) {
  const auto open = value.find("${", from);
  if (open == std::string_view::npos) {
    return {};
  }
  const auto close = value.find('}', open);
  return SpringPlaceholder{
      .open = open,
      .close = close,
      .inner = close == std::string_view::npos ? std::string_view{} : value.substr(open + 2, close - open - 2),
  };
}

bool is_spring_application_config(const std::filesystem::path& path) {
  const auto name = path.filename().generic_string();
  const auto parent = path.parent_path().generic_string();
  if (!(parent == kResources || parent.ends_with("/" + std::string(kResources)))) {
    return false;
  }
  for (const std::string_view extension : {".yml", ".yaml", ".properties"}) {
    if (!name.ends_with(extension)) {
      continue;
    }
    const auto stem = std::string_view(name).substr(0, name.size() - extension.size());
    return stem == "application" || (stem.starts_with("application-") && stem.size() > 12);
  }
  return false;
}

bool is_spring_build_file(std::string_view relative_path) {
  const auto slash = relative_path.rfind('/');
  const auto name = slash == std::string_view::npos ? relative_path : relative_path.substr(slash + 1);
  return name == "build.gradle.kts" || name == "build.gradle" || name == "pom.xml";
}

void append_spring_actuator_facts(const ExtractionContext& context, ExtractionResult& result) {
  const bool maven = context.relative_path.ends_with("pom.xml");
  const auto facts = maven ? read_pom(context.source) : read_gradle(context.source);
  // A Spring Boot application with the actuator and a servlet or reactive web
  // server. Without the web starter the endpoints are JMX only.
  if (!facts.actuator || !facts.boot || !(facts.servlet || facts.reactive)) {
    return;
  }
  auto& fragment = result.fragment;
  const auto file_id = make_id(context.relative_path);
  if (std::ranges::none_of(fragment.nodes, [&](const Node& node) { return node.id == file_id; })) {
    fragment.nodes.push_back(Node{
        .id = file_id,
        .label = std::filesystem::path(context.relative_path).filename().string(),
        .source_file = context.source_file,
        .source_location = SourceLocation{.start_line = 1, .end_line = 1},
        .kind = "file",
        .confidence = Confidence::Extracted,
    });
  }
  const auto line = line_at(context.source, *facts.actuator);
  const auto node_id = make_id(context.relative_path + ":spring_actuator");
  fragment.nodes.push_back(Node{
      .id = node_id,
      .label = "Spring Boot Actuator",
      .source_file = context.source_file,
      .source_location = SourceLocation{.start_line = line, .end_line = line},
      .kind = "spring_actuator",
      .confidence = Confidence::Extracted,
  });
  fragment.edges.push_back(Edge{.source = file_id, .target = node_id, .relation = "contains", .confidence = Confidence::Extracted});
  result.raw_relations.push_back(RawRelation{
      .source_id = node_id,
      .target_label = {},
      .relation = std::string(kAppRelation),
      .context = nlohmann::json{{"module", directory_of(context.relative_path)},
                                {"servlet", facts.servlet},
                                {"webflux", facts.reactive},
                                {"prometheus", facts.prometheus},
                                {"cache", facts.cache},
                                {"boot_major", facts.boot_major}}
                     .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),  // a path may not be UTF-8
      .source_file = context.source_file,
  });
}

ExtractionResult extract_spring_application_config(const ExtractionContext& context) {
  ExtractionResult result;
  auto& fragment = result.fragment;
  const std::filesystem::path path(context.relative_path);
  const auto file_id = make_id(context.relative_path);
  std::string file_label = path.filename().string();
  if (path.has_parent_path() && path.parent_path().has_filename()) {
    file_label = path.parent_path().filename().string() + "/" + file_label;
  }
  fragment.nodes.push_back(Node{
      .id = file_id,
      .label = std::move(file_label),
      .source_file = context.source_file,
      .source_location = SourceLocation{.start_line = 1, .end_line = 1},
      .kind = "file",
      .confidence = Confidence::Extracted,
  });

  const auto name = path.filename().string();
  const bool properties = name.ends_with(".properties");
  const auto stem = path.stem().string();
  const auto profile = stem.size() > 12 ? stem.substr(12) : std::string{};  // after "application-"
  const auto directory = path.parent_path().generic_string();
  const auto module = directory.size() > kResources.size() ? directory.substr(0, directory.size() - kResources.size() - 1)
                                                           : std::string{};
  const auto documents = properties ? parse_properties(context.source) : parse_yaml(context.source);
  for (std::size_t index = 0; index < documents.size(); ++index) {
    nlohmann::json props = nlohmann::json::object();
    std::vector<std::string> groups;
    const ConfigEntry* include = nullptr;
    for (const auto& entry : documents[index].entries) {
      const auto key = relaxed_key(entry.key);
      if (!relevant_key(key)) {
        continue;
      }
      const auto resolved = entry.known ? resolve_placeholders(entry.value) : std::nullopt;
      props[key] = resolved ? nlohmann::json(*resolved) : nlohmann::json(nullptr);
      if (key == "management.endpoints.web.exposure.include") {
        include = &entry;
      }
      // `management.endpoint.health.group.<name>.<property>`: the group as written.
      constexpr std::string_view kGroup = "management.endpoint.health.group.";
      if (key.starts_with(kGroup)) {
        auto written = std::string_view(entry.key);
        for (int dots = 0; dots < 4; ++dots) {
          written.remove_prefix(written.find('.') + 1);
        }
        const auto group = std::string(written.substr(0, written.find('.')));
        if (!group.empty() && std::ranges::find(groups, group) == groups.end()) {
          groups.push_back(group);
        }
      }
    }
    const bool unreadable = documents[index].unreadable;
    if (props.empty() && !unreadable) {
      continue;
    }
    std::string exposure_node;
    if (include != nullptr) {
      exposure_node = make_id(context.relative_path + ":actuator_exposure:" + std::to_string(index));
      fragment.nodes.push_back(Node{
          .id = exposure_node,
          .label = "management.endpoints.web.exposure.include",
          .source_file = context.source_file,
          .source_location = SourceLocation{.start_line = include->line, .end_line = include->line},
          .kind = "actuator_exposure",
          .confidence = Confidence::Extracted,
      });
      fragment.edges.push_back(
          Edge{.source = file_id, .target = exposure_node, .relation = "contains", .confidence = Confidence::Extracted});
    }
    result.raw_relations.push_back(RawRelation{
        .source_id = file_id,
        .target_label = {},
        .relation = std::string(kConfigRelation),
        .context = nlohmann::json{{"module", module},
                                  {"profile", profile},
                                  {"rank", {profile.empty() ? 0 : 1, properties ? 1 : 0, index}},
                                  {"exposure_node", exposure_node},
                                  {"groups", groups},
                                  {"unreadable", unreadable},
                                  {"props", props}}
                       .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),  // config bytes may not be UTF-8
        .source_file = context.source_file,
    });
  }
  return result;
}

std::vector<RawRelation> spring_actuator_routes(std::span<const RawRelation> raw_relations) {
  std::map<std::string, std::vector<App>> apps;
  std::map<std::string, std::vector<Document>> documents;
  for (const auto& relation : raw_relations) {
    if (relation.relation != kAppRelation && relation.relation != kConfigRelation) {
      continue;
    }
    const auto fact = nlohmann::json::parse(relation.context, nullptr, false);
    if (!fact.is_object() || !fact.contains("module") || !fact["module"].is_string()) {
      continue;
    }
    const auto module = fact["module"].get<std::string>();
    if (relation.relation == kAppRelation) {
      apps[module].push_back(App{.node_id = relation.source_id,
                                 .source_file = relation.source_file,
                                 .servlet = fact.value("servlet", false),
                                 .webflux = fact.value("webflux", false),
                                 .prometheus = fact.value("prometheus", false),
                                 .cache = fact.value("cache", false),
                                 .boot_major = fact.value("boot_major", 0)});
      continue;
    }
    Document document{
        .profile = fact.value("profile", std::string{}),
        .unreadable = fact.value("unreadable", false),
        .exposure_node = fact.value("exposure_node", std::string{}),
        .source_file = relation.source_file,
        .props = fact.value("props", nlohmann::json::object()),
        .groups = fact.value("groups", std::vector<std::string>{}),
    };
    const auto rank = fact.value("rank", std::vector<int>{});
    for (std::size_t index = 0; index < rank.size() && index < document.rank.size(); ++index) {
      document.rank[index] = rank[index];
    }
    // `spring.config.activate.on-profile` (or the legacy `spring.profiles`): a
    // list of profile names; an expression is not modeled.
    for (const auto* key : {"spring.config.activate.onprofile", "spring.profiles"}) {
      if (!document.props.contains(key)) {
        continue;
      }
      const auto& value = document.props[key];
      if (!value.is_string() || value.get_ref<const std::string&>().find_first_of("!&|()") != std::string::npos) {
        document.unmatchable = true;
        continue;
      }
      const auto& names = value.get_ref<const std::string&>();
      for (std::size_t start = 0; start <= names.size();) {
        const auto comma = names.find(',', start);
        const auto name = trim(std::string_view(names).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!name.empty()) {
          document.on_profiles.emplace_back(name);
        }
        start = comma == std::string::npos ? names.size() + 1 : comma + 1;
      }
    }
    documents[module].push_back(std::move(document));
  }

  std::vector<RawRelation> routes;
  std::set<std::tuple<std::string, std::string, std::string>> seen;
  for (const auto& [module, module_apps] : apps) {
    auto& docs = documents[module];
    std::ranges::stable_sort(docs, [](const Document& a, const Document& b) { return a.rank < b.rank; });
    std::set<std::string> profiles;
    for (const auto& document : docs) {
      if (!document.profile.empty()) {
        profiles.insert(document.profile);
      }
      profiles.insert(document.on_profiles.begin(), document.on_profiles.end());
    }
    std::vector<std::string> configurations{""};
    configurations.insert(configurations.end(), profiles.begin(), profiles.end());
    for (const auto& configuration : configurations) {
      std::map<std::string, Value> values;
      std::set<std::string> groups;
      // A document the reader could not open may apply to this configuration
      // (its own `on-profile` may be what it could not read): serve nothing.
      const bool unreadable = std::ranges::any_of(docs, [&](const Document& document) {
        return document.unreadable && (document.profile.empty() || document.profile == configuration);
      });
      if (unreadable) {
        continue;
      }
      for (const auto& document : docs) {
        const bool applies =
            !document.unmatchable && (document.profile.empty() || document.profile == configuration) &&
            (document.on_profiles.empty() || std::ranges::find(document.on_profiles, configuration) != document.on_profiles.end());
        if (!applies) {
          continue;
        }
        for (const auto& [key, value] : document.props.items()) {
          values[key] = Value{value.is_string() ? std::optional<std::string>(value.get<std::string>()) : std::nullopt, &document};
        }
        groups.insert(document.groups.begin(), document.groups.end());
      }
      for (const auto& app : module_apps) {
        const auto served = configuration_routes(values, groups, app);
        if (!served) {
          continue;
        }
        for (const auto& route : *served) {
          if (!seen.emplace(route.handler, route.method, route.path).second) {
            continue;
          }
          routes.push_back(RawRelation{
              .source_id = route.handler,
              .target_label = {},
              .relation = "file_route",  // the path is absolute: prefix, base path, endpoint, operation
              .context = route.method + " " + route.path,
              .source_file = app.source_file,
          });
        }
      }
    }
  }
  return routes;
}

}  // namespace cgraph
