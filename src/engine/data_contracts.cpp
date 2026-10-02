#include "cgraph/data_contracts.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

constexpr std::string_view kProvides = "provides_contract";
constexpr std::string_view kUses = "uses_contract";
constexpr std::string_view kOrmTableUse = "orm_table_use";
// Two opaque bytes stand for code the reader cannot see, with no spaces around
// them so text the code glues to them stays glued:
//   kOpaque   an interpolation inside one literal (`{x}`, `${x}`, `$x`);
//   kBoundary a non-literal operand of a `+` concatenation, which sits between
//             two literals and so always ends (or starts) a token there.
// The SQL reader decides per name touching one (sql_tokens): `users{where}`,
// `users$filter` and `"... FROM users" + where` read `users`, while
// `measurements_{year}`, `events_%s`, `"FROM events_" + year`, `t_{y}x` and
// `{prefix}_events` are only parts of a name and read nothing.
constexpr char kOpaque = '\x01';
constexpr char kBoundary = '\x02';
constexpr std::string_view kPlaceholder = "\x01";
constexpr std::string_view kBoundaryText = "\x02";

[[nodiscard]] bool is_ident_start(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}
[[nodiscard]] bool is_ident_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$';
}
[[nodiscard]] std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return out;
}
void push_unique(std::vector<std::string>& out, std::string value) {
  if (std::ranges::find(out, value) == out.end()) {
    out.push_back(std::move(value));
  }
}

// ---------------------------------------------------------------- SQL text

struct SqlToken {
  enum Kind { Ident, Quoted, Punct, Other } kind;
  std::string text;
};

// The index just past an interpolation starting at `at` (kOpaque, a Python
// `%s` / `%(name)s` directive, a `{field}`), or nullopt when none starts there.
[[nodiscard]] std::optional<std::size_t> interpolation_end(std::string_view text, std::size_t at) {
  if (text[at] == kOpaque) {
    return at + 1;
  }
  if (text[at] == '{') {
    const auto close = text.find('}', at + 1);
    return close == std::string_view::npos ? std::nullopt : std::optional<std::size_t>{close + 1};
  }
  if (text[at] == '%' && at + 1 < text.size()) {
    if (text[at + 1] == '(') {
      const auto close = text.find(')', at + 2);
      return close == std::string_view::npos || close + 1 >= text.size() ? std::nullopt
                                                                           : std::optional<std::size_t>{close + 2};
    }
    if (std::isalpha(static_cast<unsigned char>(text[at + 1])) != 0) {
      return at + 2;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<SqlToken> sql_tokens(std::string_view text) {
  std::vector<SqlToken> tokens;
  std::size_t i = 0;
  const auto n = text.size();
  while (i < n) {
    const char ch = text[i];
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      ++i;
    } else if (ch == '-' && i + 1 < n && text[i + 1] == '-') {
      while (i < n && text[i] != '\n') {
        ++i;
      }
    } else if (ch == '/' && i + 1 < n && text[i + 1] == '*') {
      const auto close = text.find("*/", i + 2);
      i = close == std::string_view::npos ? n : close + 2;
    } else if (ch == '\'') {
      ++i;
      while (i < n) {
        if (text[i] == '\'' && i + 1 < n && text[i + 1] == '\'') {
          i += 2;
        } else if (text[i] == '\'') {
          ++i;
          break;
        } else {
          ++i;
        }
      }
      tokens.push_back({SqlToken::Other, "'"});
    } else if (ch == '"') {
      const auto close = text.find('"', i + 1);
      if (close == std::string_view::npos) {
        tokens.push_back({SqlToken::Other, "\""});
        i = n;
      } else {
        tokens.push_back({SqlToken::Quoted, std::string(text.substr(i + 1, close - i - 1))});
        i = close + 1;
      }
    } else if (is_ident_start(ch)) {
      const auto start = i;
      while (i < n && is_ident_char(text[i])) {
        ++i;
      }
      // A name touching code the reader cannot see may be only part of a name.
      // After it: an interpolation (kOpaque, a `%s` / `%(x)s` directive, a
      // `{x}` format field) glues when the name ends in `_` or more name
      // follows the interpolation (`t_{y}`, `t{y}_x`); a concatenation
      // boundary glues only a name ending in `_`. Before it: a name right after
      // an interpolation or a `{x}` field is a suffix (`{prefix}_events`); right
      // after a boundary only when it starts with `_` (`schema + "_events"`).
      const auto name = text.substr(start, i - start);
      bool glued = false;
      if (start > 0) {
        const char before = text[start - 1];
        glued = before == kOpaque || before == '}' || (before == kBoundary && name.front() == '_');
      }
      if (!glued && i < n) {
        if (text[i] == kBoundary) {
          glued = name.back() == '_';
        } else if (const auto end = interpolation_end(text, i)) {
          glued = name.back() == '_' || (*end < n && is_ident_char(text[*end]));
        }
      }
      tokens.push_back({glued ? SqlToken::Other : SqlToken::Ident, std::string(name)});
    } else if (ch == ':' || ch == '$' || ch == '@') {
      // `:name`, `$1`, `@p`: a bind parameter, one opaque token.
      const auto start = i++;
      while (i < n && is_ident_char(text[i])) {
        ++i;
      }
      tokens.push_back({SqlToken::Other, std::string(text.substr(start, i - start))});
    } else if (ch == '(' || ch == ')' || ch == ',' || ch == '.' || ch == ';') {
      tokens.push_back({SqlToken::Punct, std::string(1, ch)});
      ++i;
    } else {
      tokens.push_back({SqlToken::Other, std::string(1, ch)});
      ++i;
    }
  }
  return tokens;
}

// Upper-case SQL words that are never a table name and, before `(`, open a
// subquery or a list rather than a function call.
[[nodiscard]] bool is_sql_keyword(const SqlToken& token) {
  static const std::unordered_set<std::string_view> kKeywords = {
      "ALL",       "AND",     "ANY",    "ARRAY",   "AS",       "ASC",      "BETWEEN", "BY",       "CASE",
      "CONFLICT",  "CROSS",   "DEFAULT", "DELETE", "DESC",     "DISTINCT", "DO",      "ELSE",     "END",
      "EXCEPT",    "EXISTS",  "FALSE",  "FETCH",   "FILTER",   "FOR",      "FROM",    "FULL",     "GROUP",
      "HAVING",    "ILIKE",   "IN",     "INNER",   "INSERT",   "INTERSECT", "INTO",   "IS",       "JOIN",
      "LATERAL",   "LEFT",    "LIKE",   "LIMIT",   "MATERIALIZED", "NATURAL", "NOT",   "NOTHING",  "NULL",
      "OF",
      "OFFSET",    "ON",      "ONLY",   "OR",      "ORDER",    "OUTER",    "OVER",    "PARTITION", "RECURSIVE",
      "RETURNING", "RIGHT",   "ROW",    "SELECT",  "SET",      "SOME",     "THEN",    "TRUE",     "UNION",
      "UPDATE",    "USING",   "VALUES", "WHEN",    "WHERE",    "WINDOW",   "WITH",    "WITHIN",
  };
  return token.kind == SqlToken::Ident && kKeywords.contains(token.text);
}

[[nodiscard]] bool is_word(const std::vector<SqlToken>& tokens, std::size_t index, std::string_view word) {
  return index < tokens.size() && tokens[index].kind == SqlToken::Ident && tokens[index].text == word;
}
[[nodiscard]] bool is_punct(const std::vector<SqlToken>& tokens, std::size_t index, char ch) {
  return index < tokens.size() && tokens[index].kind == SqlToken::Punct && tokens[index].text[0] == ch;
}

// A table reference at `index`: `[ONLY] [schema.]name`. The normalized name and
// the index after it, or nullopt when no table name stands there.
struct TableRef {
  std::string name;
  std::string schema;  // the qualifier as written, lower-cased unless quoted; empty when none
  std::size_t next;
};
[[nodiscard]] std::optional<TableRef> table_ref(const std::vector<SqlToken>& tokens, std::size_t index) {
  if (is_word(tokens, index, "ONLY")) {
    ++index;
  }
  const auto name_token = [&](std::size_t at) {
    return at < tokens.size() &&
           ((tokens[at].kind == SqlToken::Ident && !is_sql_keyword(tokens[at])) || tokens[at].kind == SqlToken::Quoted);
  };
  if (!name_token(index)) {
    return std::nullopt;
  }
  const auto spelled = [&](std::size_t at) {
    return tokens[at].kind == SqlToken::Quoted ? tokens[at].text : lower(tokens[at].text);
  };
  std::string schema;
  std::string name = spelled(index);
  ++index;
  if (is_punct(tokens, index, '.') && name_token(index + 1)) {
    schema = name;
    name = spelled(index + 1);
    index += 2;
  }
  if (name.empty() || schema == "information_schema" || schema == "pg_catalog" || name.starts_with("pg_")) {
    return std::nullopt;  // the database's own catalog, not a schema anybody owns
  }
  return TableRef{std::move(name), std::move(schema), index};
}

// Whether the text's first word (after spaces and `(`) is an upper-case SQL
// statement verb: the cheap test every string literal takes first.
[[nodiscard]] bool opens_with_sql_verb(std::string_view text) {
  std::size_t i = 0;
  for (;;) {  // spaces, `(`, and leading `-- ...` / `/* ... */` comments
    while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) != 0 || text[i] == '(')) {
      ++i;
    }
    if (text.substr(i).starts_with("--")) {
      const auto end = text.find('\n', i);
      i = end == std::string_view::npos ? text.size() : end + 1;
    } else if (text.substr(i).starts_with("/*")) {
      const auto end = text.find("*/", i + 2);
      i = end == std::string_view::npos ? text.size() : end + 2;
    } else {
      break;
    }
  }
  const auto start = i;
  while (i < text.size() && std::isupper(static_cast<unsigned char>(text[i])) != 0) {
    ++i;
  }
  if (i < text.size() && is_ident_char(text[i])) {
    return false;
  }
  const auto word = text.substr(start, i - start);
  return word == "SELECT" || word == "INSERT" || word == "UPDATE" || word == "DELETE" || word == "WITH";
}

// ---------------------------------------------------------------- Cypher text

// The text with comments blanked and string literal contents blanked (quotes
// kept), every offset and newline preserved.
[[nodiscard]] std::string cypher_code(std::string_view text) {
  std::string out(text);
  std::size_t i = 0;
  const auto n = out.size();
  const auto blank = [&](std::size_t from, std::size_t to) {
    for (auto k = from; k < to && k < n; ++k) {
      if (out[k] != '\n') {
        out[k] = ' ';
      }
    }
  };
  while (i < n) {
    const char ch = out[i];
    if (ch == '/' && i + 1 < n && out[i + 1] == '/') {
      const auto end = out.find('\n', i);
      const auto stop = end == std::string::npos ? n : end;
      blank(i, stop);
      i = stop;
    } else if (ch == '/' && i + 1 < n && out[i + 1] == '*') {
      const auto close = out.find("*/", i + 2);
      const auto stop = close == std::string::npos ? n : close + 2;
      blank(i, stop);
      i = stop;
    } else if (ch == '\'' || ch == '"') {
      auto k = i + 1;
      while (k < n && out[k] != ch) {
        k += out[k] == '\\' ? 2 : 1;
      }
      blank(i + 1, std::min(k, n));
      i = k + 1;
    } else {
      ++i;
    }
  }
  return out;
}

struct CypherElement {
  bool node = true;
  std::size_t start = 0;  // offset of `(` / `[`
  std::size_t end = 0;    // offset just past `)` / `]`
  std::string variable;
  std::vector<std::string> names;  // labels, or relationship types
  bool points_left = false;        // `<-[`
  bool points_right = false;       // `]->`
};

void skip_space(std::string_view text, std::size_t& i) {
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
    ++i;
  }
}

[[nodiscard]] std::string cypher_name(std::string_view text, std::size_t& i) {
  if (i < text.size() && text[i] == '`') {
    const auto close = text.find('`', i + 1);
    if (close == std::string_view::npos) {
      return {};
    }
    auto name = std::string(text.substr(i + 1, close - i - 1));
    i = close + 1;
    return name;
  }
  const auto start = i;
  if (i < text.size() && is_ident_start(text[i])) {
    while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) != 0 || text[i] == '_')) {
      ++i;
    }
  }
  return std::string(text.substr(start, i - start));
}

// Skips a balanced `{...}` property map at `i`, if any.
[[nodiscard]] bool skip_properties(std::string_view text, std::size_t& i) {
  if (i >= text.size() || text[i] != '{') {
    return true;
  }
  int depth = 0;
  for (; i < text.size(); ++i) {
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}' && --depth == 0) {
      ++i;
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::optional<CypherElement> cypher_node(std::string_view text, std::size_t open) {
  CypherElement element;
  element.start = open;
  auto i = open + 1;
  skip_space(text, i);
  element.variable = cypher_name(text, i);
  skip_space(text, i);
  while (i < text.size() && text[i] == ':') {
    ++i;
    skip_space(text, i);
    auto label = cypher_name(text, i);
    if (label.empty()) {
      return std::nullopt;
    }
    element.names.push_back(std::move(label));
    skip_space(text, i);
  }
  if (!skip_properties(text, i)) {
    return std::nullopt;
  }
  skip_space(text, i);
  if (i >= text.size() || text[i] != ')') {
    return std::nullopt;
  }
  element.end = i + 1;
  return element;
}

[[nodiscard]] std::optional<CypherElement> cypher_relationship(std::string_view text, std::size_t open) {
  // `-[`, or `<-[`, with only spaces between.
  auto before = open;
  while (before > 0 && std::isspace(static_cast<unsigned char>(text[before - 1])) != 0) {
    --before;
  }
  if (before == 0 || text[before - 1] != '-') {
    return std::nullopt;
  }
  CypherElement element;
  element.node = false;
  element.start = open;
  element.points_left = before >= 2 && text[before - 2] == '<';
  auto i = open + 1;
  skip_space(text, i);
  element.variable = cypher_name(text, i);
  skip_space(text, i);
  if (i < text.size() && text[i] == ':') {
    ++i;
    for (;;) {
      skip_space(text, i);
      auto type = cypher_name(text, i);
      if (type.empty()) {
        return std::nullopt;
      }
      element.names.push_back(std::move(type));
      skip_space(text, i);
      if (i < text.size() && text[i] == '|') {
        ++i;
        skip_space(text, i);
        if (i < text.size() && text[i] == ':') {
          ++i;
        }
        continue;
      }
      break;
    }
  }
  if (i < text.size() && text[i] == '*') {  // variable length `*1..3`
    ++i;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0 || text[i] == '.' ||
                               std::isspace(static_cast<unsigned char>(text[i])) != 0)) {
      ++i;
    }
  }
  if (!skip_properties(text, i)) {
    return std::nullopt;
  }
  skip_space(text, i);
  if (i >= text.size() || text[i] != ']') {
    return std::nullopt;
  }
  element.end = i + 1;
  auto after = element.end;
  skip_space(text, after);
  if (after >= text.size() || text[after] != '-') {
    return std::nullopt;
  }
  element.points_right = after + 1 < text.size() && text[after + 1] == '>';
  return element;
}

// Whether the code text opens with an upper-case Cypher clause.
[[nodiscard]] bool opens_with_cypher_clause(std::string_view code) {
  std::size_t i = 0;
  skip_space(code, i);
  const auto start = i;
  while (i < code.size() && std::isupper(static_cast<unsigned char>(code[i])) != 0) {
    ++i;
  }
  const auto word = code.substr(start, i - start);
  if (i < code.size() && is_ident_char(code[i])) {
    return false;  // `Matches`, `CREATED_AT`
  }
  if (word == "MATCH" || word == "MERGE" || word == "UNWIND") {
    return true;
  }
  if (word == "OPTIONAL") {
    skip_space(code, i);
    return code.substr(i).starts_with("MATCH");
  }
  if (word == "CREATE") {  // `CREATE (n:...)`, never `CREATE TABLE` / `CREATE INDEX`
    skip_space(code, i);
    return i < code.size() && code[i] == '(';
  }
  return false;
}

// ---------------------------------------------------------------- source trees

enum class Family { None, Python, JavaScript, Kotlin, Java };

[[nodiscard]] Family family_of(std::string_view language) {
  if (language == "python") {
    return Family::Python;
  }
  if (language == "javascript" || language == "typescript" || language == "tsx") {
    return Family::JavaScript;
  }
  if (language == "kotlin") {
    return Family::Kotlin;
  }
  if (language == "java") {
    return Family::Java;
  }
  return Family::None;
}

[[nodiscard]] std::string_view text_of(const TSNode& node, std::string_view source) {
  const auto start = ts_node_start_byte(node);
  const auto end = ts_node_end_byte(node);
  if (start >= end || end > source.size()) {
    return {};
  }
  return source.substr(start, end - start);
}

[[nodiscard]] bool is_string_node(Family family, std::string_view type) {
  switch (family) {
    case Family::Python:
      return type == "string";
    case Family::JavaScript:
      return type == "string" || type == "template_string";
    case Family::Kotlin:
    case Family::Java:
      return type == "string_literal" || type == "text_block";
    case Family::None:
      break;
  }
  return false;
}

// A `+` concatenation (or Python's adjacent literals).
[[nodiscard]] bool is_concatenation(Family family, const TSNode& node, std::string_view source) {
  const std::string_view type = ts_node_type(node);
  if (family == Family::Python && type == "concatenated_string") {
    return true;
  }
  const bool binary = family == Family::Python   ? type == "binary_operator"
                      : family == Family::Kotlin ? type == "additive_expression"
                                                 : type == "binary_expression";
  if (!binary || ts_node_child_count(node) != 3) {
    return false;
  }
  return text_of(ts_node_child(node, 1), source) == "+";
}

// Backslash escapes as the language reads them; a line break or tab is a space.
[[nodiscard]] std::string decode_escapes(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '\\' || i + 1 >= raw.size()) {
      out.push_back(raw[i]);
      continue;
    }
    const char next = raw[++i];
    if (next == '"' || next == '\'' || next == '\\' || next == '`' || next == '$') {
      out.push_back(next);
    } else {
      out.push_back(' ');
    }
  }
  return out;
}

// The characters a string literal stands for, interpolations as kPlaceholder.
[[nodiscard]] std::string literal_text(const TSNode& node, std::string_view source) {
  std::string raw;
  const auto count = ts_node_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto child = ts_node_child(node, i);
    const std::string_view type = ts_node_type(child);
    if (type == "interpolation" || type == "template_substitution" || type == "string_interpolation" ||
        type == "interpolated_expression" || type == "interpolated_identifier") {
      raw += kPlaceholder;
    } else if (type == "string_content" || type == "string_fragment" || type == "multiline_string_fragment" ||
               type == "escape_sequence" || type == "character_escape_seq") {
      raw += text_of(child, source);
    }
  }
  return decode_escapes(raw);
}

// A concatenation's text: literal operands in order, anything else a placeholder.
void concatenation_text(Family family, const TSNode& node, std::string_view source, std::string& out,
                        bool& has_literal) {
  const std::string_view type = ts_node_type(node);
  if (is_string_node(family, type)) {
    out += literal_text(node, source);
    has_literal = true;
    return;
  }
  if (type == "parenthesized_expression" && ts_node_named_child_count(node) == 1) {
    concatenation_text(family, ts_node_named_child(node, 0), source, out, has_literal);
    return;
  }
  if (is_concatenation(family, node, source)) {
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < count; ++i) {
      concatenation_text(family, ts_node_named_child(node, i), source, out, has_literal);
    }
    return;
  }
  out += kBoundaryText;
}

struct Scope {
  std::string id;
  SourceLocation location;
};

[[nodiscard]] bool covers(const SourceLocation& location, std::uint32_t line, std::uint32_t column) {
  const auto after_start =
      line > location.start_line || (line == location.start_line && column >= location.start_column);
  const auto before_end = line < location.end_line || (line == location.end_line && column <= location.end_column);
  return after_start && before_end;
}

// Scopes that both cover a point nest: the one starting later (or, starting
// together, ending sooner) is the inner one.
[[nodiscard]] bool narrower(const SourceLocation& a, const SourceLocation& b) {
  const auto a_start = std::pair(a.start_line, a.start_column);
  const auto b_start = std::pair(b.start_line, b.start_column);
  if (a_start != b_start) {
    return a_start > b_start;
  }
  return std::pair(a.end_line, a.end_column) < std::pair(b.end_line, b.end_column);
}

// The code a fact at a position hangs off: the innermost function, else
// module-level variable, else class containing it, else the file.
class Scopes {
 public:
  Scopes(const Fragment& fragment, const ExtractionContext& context) : file_id_(make_id(context.relative_path)) {
    for (const auto& node : fragment.nodes) {
      if (node.source_file != context.source_file || !node.source_location || node.source_location->start_line == 0) {
        continue;
      }
      if (node.kind == "function") {
        functions_.push_back({node.id, *node.source_location});
      } else if (node.kind == "variable") {
        variables_.push_back({node.id, *node.source_location});
      } else if (node.kind == "class") {
        classes_.push_back({node.id, *node.source_location});
      }
    }
  }

  [[nodiscard]] std::string at(const TSNode& node) const {
    for (const auto* scopes : {&functions_, &variables_, &classes_}) {
      if (auto id = innermost(*scopes, node); !id.empty()) {
        return id;
      }
    }
    return file_id_;
  }
  [[nodiscard]] std::string function_at(const TSNode& node) const { return innermost(functions_, node); }
  [[nodiscard]] std::string class_at(const TSNode& node) const { return innermost(classes_, node); }

 private:
  [[nodiscard]] static std::string innermost(const std::vector<Scope>& scopes, const TSNode& node) {
    const auto point = ts_node_start_point(node);
    const Scope* best = nullptr;
    for (const auto& scope : scopes) {
      if (covers(scope.location, point.row + 1, point.column) &&
          (best == nullptr || narrower(scope.location, best->location))) {
        best = &scope;
      }
    }
    return best == nullptr ? std::string{} : best->id;
  }

  std::string file_id_;
  std::vector<Scope> functions_;
  std::vector<Scope> variables_;
  std::vector<Scope> classes_;
};

// An annotation's simple name and its argument text: `@Node("User")` is
// ("Node", "\"User\""), `@org.x.Node` is ("Node", "").
[[nodiscard]] std::pair<std::string, std::string> annotation_parts(std::string_view text) {
  std::size_t i = 0;
  if (i < text.size() && text[i] == '@') {
    ++i;
  }
  const auto start = i;
  while (i < text.size() && (is_ident_char(text[i]) || text[i] == '.')) {
    ++i;
  }
  auto qualified = text.substr(start, i - start);
  if (const auto dot = qualified.rfind('.'); dot != std::string_view::npos) {
    qualified = qualified.substr(dot + 1);
  }
  std::string arguments;
  if (const auto open = text.find('(', i); open != std::string_view::npos) {
    const auto close = text.rfind(')');
    if (close != std::string_view::npos && close > open) {
      arguments = std::string(text.substr(open + 1, close - open - 1));
    }
  }
  return {std::string(qualified), std::move(arguments)};
}

[[nodiscard]] std::vector<std::string> string_literals(std::string_view text) {
  static const std::regex kLiteral{R"re("([^"\\]*)")re"};
  std::vector<std::string> out;
  const std::string owned(text);
  for (auto it = std::sregex_iterator(owned.begin(), owned.end(), kLiteral); it != std::sregex_iterator(); ++it) {
    out.push_back((*it)[1].str());
  }
  return out;
}

[[nodiscard]] bool is_annotation(std::string_view type) {
  return type == "annotation" || type == "marker_annotation";
}

// The annotations directly on a declaration (its `modifiers`).
[[nodiscard]] std::vector<TSNode> declaration_annotations(const TSNode& declaration) {
  std::vector<TSNode> out;
  const auto count = ts_node_named_child_count(declaration);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto child = ts_node_named_child(declaration, i);
    if (std::string_view(ts_node_type(child)) != "modifiers") {
      continue;
    }
    const auto modifiers = ts_node_named_child_count(child);
    for (std::uint32_t j = 0; j < modifiers; ++j) {
      const auto modifier = ts_node_named_child(child, j);
      if (is_annotation(ts_node_type(modifier))) {
        out.push_back(modifier);
      }
    }
  }
  return out;
}

[[nodiscard]] std::string class_name(const TSNode& declaration, std::string_view source) {
  const auto name = ts_node_child_by_field_name(declaration, "name", 4);
  if (!ts_node_is_null(name)) {
    return std::string(text_of(name, source));
  }
  const auto count = ts_node_named_child_count(declaration);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto child = ts_node_named_child(declaration, i);
    if (std::string_view(ts_node_type(child)) == "type_identifier") {
      return std::string(text_of(child, source));
    }
  }
  return {};
}

[[nodiscard]] bool is_type_declaration(std::string_view type) {
  return type == "class_declaration" || type == "object_declaration" || type == "interface_declaration" ||
         type == "enum_declaration" || type == "record_declaration" || type == "companion_object";
}

class Emitter {
 public:
  Emitter(const ExtractionContext& context, std::vector<RawRelation>& out) : context_(context), out_(out) {}

  void fact(std::string_view relation, const std::string& source_id, std::string_view kind, const std::string& name,
            std::string target_label = {}) {
    if (source_id.empty() || name.empty()) {
      return;
    }
    auto context = std::string(kind) + ":" + name;
    if (!seen_.insert(std::string(relation) + "\n" + source_id + "\n" + context + "\n" + target_label).second) {
      return;
    }
    out_.push_back(RawRelation{
        .source_id = source_id,
        .target_label = std::move(target_label),
        .relation = std::string(relation),
        .context = relation == kOrmTableUse ? std::string{} : std::move(context),
        .source_file = context_.source_file,
    });
  }

 private:
  const ExtractionContext& context_;
  std::vector<RawRelation>& out_;
  std::unordered_set<std::string> seen_;
};

// Spring Data Neo4j: `@Node` on a class provides its labels; an outgoing
// `@Relationship` on its members provides `<label>.<TYPE>`.
void neo4j_entity(const TSNode& declaration, std::string_view source, const Scopes& scopes, Emitter& emit) {
  std::vector<std::string> labels;
  bool entity = false;
  for (const auto& annotation : declaration_annotations(declaration)) {
    const auto [name, arguments] = annotation_parts(text_of(annotation, source));
    if (name != "Node") {
      continue;
    }
    entity = true;
    labels = string_literals(arguments);
  }
  if (!entity) {
    return;
  }
  if (labels.empty()) {
    labels.push_back(class_name(declaration, source));  // `@Node` alone: the class's simple name
  }
  const auto class_id = scopes.class_at(declaration);
  for (const auto& label : labels) {
    emit.fact(kProvides, class_id, "label", label);
  }
  // Relationship annotations in this class's own body, not a nested type's.
  std::vector<TSNode> stack;
  const auto count = ts_node_named_child_count(declaration);
  for (std::uint32_t i = 0; i < count; ++i) {
    stack.push_back(ts_node_named_child(declaration, i));
  }
  while (!stack.empty()) {
    const auto node = stack.back();
    stack.pop_back();
    const std::string_view type = ts_node_type(node);
    if (is_type_declaration(type)) {
      continue;
    }
    if (is_annotation(type)) {
      const auto [name, arguments] = annotation_parts(text_of(node, source));
      if (name != "Relationship") {
        continue;
      }
      // INCOMING / UNDIRECTED start at the other entity, whose label this class
      // does not know.
      if (arguments.find("INCOMING") != std::string::npos || arguments.find("UNDIRECTED") != std::string::npos) {
        continue;
      }
      static const std::regex kType{R"re(^\s*(?:(?:type|value)\s*=\s*)?"([^"]+)")re"};
      static const std::regex kNamedType{R"re((?:type|value)\s*=\s*"([^"]+)")re"};
      std::smatch match;
      std::string relationship;
      if (std::regex_search(arguments, match, kType) || std::regex_search(arguments, match, kNamedType)) {
        relationship = match[1].str();
      }
      if (!relationship.empty()) {
        emit.fact(kProvides, class_id, "label", labels.front() + "." + relationship);
      }
      continue;
    }
    const auto children = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < children; ++i) {
      stack.push_back(ts_node_named_child(node, i));
    }
  }
}

// `db.update(compiqJobs)`, `.from(t)`, `.innerJoin(t, ...)`: the identifier a
// Drizzle query builder is handed, resolved against the repo's models later.
void orm_table_use(const TSNode& call, std::string_view source, const Scopes& scopes, Emitter& emit) {
  static const std::unordered_set<std::string_view> kBuilders = {
      "from", "update", "insert", "delete", "join", "innerJoin", "leftJoin", "rightJoin", "fullJoin",
  };
  const auto callee = ts_node_child_by_field_name(call, "function", 8);
  if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "member_expression") {
    return;
  }
  const auto property = ts_node_child_by_field_name(callee, "property", 8);
  if (ts_node_is_null(property) || !kBuilders.contains(text_of(property, source))) {
    return;
  }
  const auto arguments = ts_node_child_by_field_name(call, "arguments", 9);
  if (ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0) {
    return;
  }
  const auto first = ts_node_named_child(arguments, 0);
  if (std::string_view(ts_node_type(first)) != "identifier") {
    return;
  }
  emit.fact(kOrmTableUse, scopes.function_at(call), "", std::string(text_of(first, source)),
            std::string(text_of(first, source)));
}

// Whether a string sits in a JPA query annotation whose text is JPQL / HQL,
// which names entities and paths, not tables: `@Query` without
// `nativeQuery = true`, `@NamedQuery`. (A Spring Data Neo4j `@Query` holds
// Cypher, which is still read.)
[[nodiscard]] bool in_jpql_annotation(const TSNode& node, std::string_view source) {
  for (auto parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    const std::string_view type = ts_node_type(parent);
    if (is_annotation(type)) {
      const auto [name, arguments] = annotation_parts(text_of(parent, source));
      static const std::regex kNative{R"(nativeQuery\s*=\s*true)"};
      return (name == "Query" && !std::regex_search(arguments, kNative)) || name == "NamedQuery";
    }
    if (type.ends_with("declaration") || type == "class_body" || type == "block" || type == "function_body") {
      return false;
    }
  }
  return false;
}

void read_text(const std::string& text, bool sql, const TSNode& at, const Scopes& scopes, Emitter& emit) {
  const auto tables = sql ? sql_text_tables(text) : std::vector<std::string>{};
  const auto labels = cypher_text_labels(text, true);
  if (tables.empty() && labels.empty()) {
    return;
  }
  const auto source_id = scopes.at(at);
  for (const auto& table : tables) {
    emit.fact(kUses, source_id, "table", table);
  }
  for (const auto& label : labels) {
    emit.fact(kUses, source_id, "label", label);
  }
}

}  // namespace

std::vector<std::string> sql_text_tables(std::string_view text) {
  std::vector<std::string> tables;
  // Most string literals are no SQL: decide on the first word before tokenizing.
  if (!opens_with_sql_verb(text)) {
    return tables;
  }
  const auto tokens = sql_tokens(text);
  std::size_t first = 0;
  while (is_punct(tokens, first, '(')) {
    ++first;
  }
  if (!(is_word(tokens, first, "SELECT") || is_word(tokens, first, "INSERT") || is_word(tokens, first, "UPDATE") ||
        is_word(tokens, first, "DELETE") || is_word(tokens, first, "WITH"))) {
    return tables;
  }
  // CTE names are no tables: `WITH recent AS (`, `, older AS (`,
  // `WITH x(a, b) AS (`, `WITH x AS [NOT] MATERIALIZED (`.
  std::unordered_set<std::string> ctes;
  for (std::size_t i = 1; i + 2 < tokens.size(); ++i) {
    if ((tokens[i].kind != SqlToken::Ident && tokens[i].kind != SqlToken::Quoted) ||
        !(is_word(tokens, i - 1, "WITH") || is_word(tokens, i - 1, "RECURSIVE") || is_punct(tokens, i - 1, ','))) {
      continue;
    }
    auto at = i + 1;
    if (is_punct(tokens, at, '(')) {  // a column list
      int depth = 0;
      for (; at < tokens.size(); ++at) {
        if (is_punct(tokens, at, '(')) {
          ++depth;
        } else if (is_punct(tokens, at, ')') && --depth == 0) {
          ++at;
          break;
        }
      }
    }
    if (!is_word(tokens, at, "AS")) {
      continue;
    }
    ++at;
    if (is_word(tokens, at, "NOT")) {
      ++at;
    }
    if (is_word(tokens, at, "MATERIALIZED")) {
      ++at;
    }
    if (is_punct(tokens, at, '(')) {
      ctes.insert(tokens[i].kind == SqlToken::Quoted ? tokens[i].text : lower(tokens[i].text));
    }
  }
  // Aliases bound by an earlier table reference (`FROM users u`): `JOIN u.roles`
  // after one is a path through an entity (JPQL / HQL), not a table.
  std::unordered_set<std::string> aliases;
  // Inside a function call's parentheses (`EXTRACT(YEAR FROM d)`,
  // `SUBSTRING(s FROM 2)`) FROM names no table.
  std::vector<bool> in_function;
  const auto add = [&](const std::optional<TableRef>& ref) {
    if (!ref || ctes.contains(ref->name) || (!ref->schema.empty() && aliases.contains(ref->schema))) {
      return;
    }
    push_unique(tables, ref->name);
    auto alias = ref->next;
    if (is_word(tokens, alias, "AS")) {
      ++alias;
    }
    if (alias < tokens.size() && tokens[alias].kind == SqlToken::Ident && !is_sql_keyword(tokens[alias])) {
      aliases.insert(lower(tokens[alias].text));
    }
  };
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (is_punct(tokens, i, '(')) {
      in_function.push_back(i > 0 && tokens[i - 1].kind == SqlToken::Ident && !is_sql_keyword(tokens[i - 1]));
      continue;
    }
    if (is_punct(tokens, i, ')')) {
      if (!in_function.empty()) {
        in_function.pop_back();
      }
      continue;
    }
    if (!in_function.empty() && in_function.back()) {
      continue;
    }
    if (is_word(tokens, i, "FROM") || is_word(tokens, i, "JOIN")) {
      if (is_word(tokens, i, "FROM") && i > 0 && is_word(tokens, i - 1, "DISTINCT")) {
        continue;  // `IS DISTINCT FROM x`: a value
      }
      auto ref = table_ref(tokens, i + 1);
      if (ref && is_punct(tokens, ref->next, '(')) {
        continue;  // `FROM unnest(...)`: a set-returning function
      }
      add(ref);
    } else if (is_word(tokens, i, "INTO") && i > 0 && is_word(tokens, i - 1, "INSERT")) {
      add(table_ref(tokens, i + 1));
    } else if (is_word(tokens, i, "UPDATE")) {
      // `UPDATE t [AS] [alias] SET`; never `DO UPDATE SET` or `FOR UPDATE`.
      const auto ref = table_ref(tokens, i + 1);
      if (!ref) {
        continue;
      }
      auto next = ref->next;
      for (int skipped = 0; skipped < 2 && next < tokens.size() && !is_word(tokens, next, "SET") &&
                            tokens[next].kind == SqlToken::Ident;
           ++skipped) {
        ++next;
      }
      if (is_word(tokens, next, "SET")) {
        add(ref);
      }
    }
  }
  return tables;
}

std::vector<std::string> cypher_text_labels(std::string_view text, bool require_clause) {
  std::vector<std::string> out;
  if (require_clause) {
    // The cheap test first: a string that opens with neither a clause nor a
    // comment is no Cypher, and is never copied.
    std::size_t first = 0;
    skip_space(text, first);
    if ((first >= text.size() || text[first] != '/') && !opens_with_cypher_clause(text)) {
      return out;
    }
  }
  const auto code = cypher_code(text);
  if (require_clause && !opens_with_cypher_clause(code)) {
    return out;
  }
  std::size_t statement_start = 0;
  while (statement_start < code.size()) {
    auto statement_end = code.find(';', statement_start);
    if (statement_end == std::string::npos) {
      statement_end = code.size();
    }
    const std::string_view statement(code.data() + statement_start, statement_end - statement_start);
    std::vector<CypherElement> elements;
    for (std::size_t i = 0; i < statement.size(); ++i) {
      std::optional<CypherElement> element;
      if (statement[i] == '(') {
        element = cypher_node(statement, i);
      } else if (statement[i] == '[') {
        element = cypher_relationship(statement, i);
      }
      if (element) {
        i = element->end - 1;
        elements.push_back(std::move(*element));
      }
    }
    // A variable bound with a label anywhere in the statement keeps it.
    std::unordered_map<std::string, std::string> bound;
    for (const auto& element : elements) {
      if (element.node && !element.variable.empty() && !element.names.empty()) {
        bound.emplace(element.variable, element.names.front());
      }
    }
    const auto label_of = [&](const CypherElement& node) -> std::string {
      if (!node.names.empty()) {
        return node.names.front();
      }
      const auto it = bound.find(node.variable);
      return it == bound.end() ? std::string{} : it->second;
    };
    // Only `-`, `<` and spaces between a node and its relationship.
    const auto adjacent = [&](std::size_t from, std::size_t to) {
      for (auto k = from; k < to; ++k) {
        if (statement[k] != '-' && statement[k] != '<' && statement[k] != '>' &&
            std::isspace(static_cast<unsigned char>(statement[k])) == 0) {
          return false;
        }
      }
      return true;
    };
    for (std::size_t k = 0; k < elements.size(); ++k) {
      const auto& element = elements[k];
      if (element.node) {
        for (const auto& label : element.names) {
          push_unique(out, label);
        }
        continue;
      }
      if (element.names.empty() || element.points_left == element.points_right) {
        continue;  // no type, or undirected / both ways: no start node to qualify by
      }
      const bool left_ok = k > 0 && elements[k - 1].node && adjacent(elements[k - 1].end, element.start);
      const bool right_ok =
          k + 1 < elements.size() && elements[k + 1].node && adjacent(element.end, elements[k + 1].start);
      const CypherElement* start = nullptr;
      if (element.points_right && left_ok) {
        start = &elements[k - 1];
      } else if (element.points_left && right_ok) {
        start = &elements[k + 1];
      }
      if (start == nullptr) {
        continue;
      }
      const auto label = label_of(*start);
      if (label.empty()) {
        continue;  // the start node's label is unknown here: no contract rather than a guess
      }
      for (const auto& type : element.names) {
        push_unique(out, label + "." + type);
      }
    }
    statement_start = statement_end + 1;
  }
  return out;
}

bool is_test_source_path(std::string_view relative_path) {
  const std::filesystem::path path{std::string(relative_path)};
  for (const auto& part : path.parent_path()) {
    const auto segment = part.generic_string();
    if (segment == "test" || segment == "tests" || segment == "__tests__" || segment == "__mocks__" ||
        segment == "spec" || segment == "specs" || segment == "testdata" || segment == "fixtures" ||
        segment == "testFixtures" || segment == "integrationTest" || segment == "e2e" || segment == "cypress") {
      return true;
    }
  }
  const auto file = path.filename().generic_string();
  const auto stem = path.stem().generic_string();
  const auto extension = path.extension().generic_string();
  if (file.find(".test.") != std::string::npos || file.find(".spec.") != std::string::npos) {
    return true;
  }
  if (extension == ".py") {
    return file.starts_with("test_") || stem.ends_with("_test") || file == "conftest.py";
  }
  if (extension == ".kt" || extension == ".java") {
    // `UserServiceTest`, `UserServiceTests`, `UserRepositoryIT`: the suffix
    // follows a lower-case letter or digit, so `AUDIT` and `ABTest` are not tests.
    for (const std::string_view suffix : {"Tests", "Test", "IT"}) {
      if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
        const auto before = static_cast<unsigned char>(stem[stem.size() - suffix.size() - 1]);
        return std::islower(before) != 0 || std::isdigit(before) != 0;
      }
    }
  }
  return false;
}

void extract_code_data_contracts(const TSNode& root, std::string_view language, const ExtractionContext& context,
                                 const Fragment& fragment, std::vector<RawRelation>& raw_relations) {
  const auto family = family_of(language);
  if (family == Family::None) {
    return;
  }
  if (is_test_source_path(context.relative_path)) {
    return;  // a test's strings, entities and queries are fixtures, not the service's schema
  }
  const Scopes scopes(fragment, context);
  Emitter emit(context, raw_relations);
  std::vector<TSNode> stack{root};
  while (!stack.empty()) {
    const auto node = stack.back();
    stack.pop_back();
    const std::string_view type = ts_node_type(node);
    if ((family == Family::Kotlin || family == Family::Java) && type == "class_declaration") {
      neo4j_entity(node, context.source, scopes, emit);
    }
    if (family == Family::JavaScript && type == "call_expression") {
      orm_table_use(node, context.source, scopes, emit);
    }
    const bool literal = is_string_node(family, type);
    if (literal || is_concatenation(family, node, context.source)) {
      std::string text;
      bool has_literal = false;
      concatenation_text(family, node, context.source, text, has_literal);
      if (has_literal) {
        const bool jpql = (family == Family::Kotlin || family == Family::Java) && opens_with_sql_verb(text) &&
                          in_jpql_annotation(node, context.source);
        read_text(text, !jpql, node, scopes, emit);
        continue;  // its literals are read; a nested query inside an interpolation is not
      }
    }
    const auto count = ts_node_child_count(node);
    for (std::uint32_t i = count; i > 0; --i) {
      stack.push_back(ts_node_child(node, i - 1));
    }
  }
}

std::vector<RawRelation> sql_table_contract_facts(const Fragment& fragment, const ExtractionContext& context) {
  std::vector<RawRelation> facts;
  std::unordered_set<std::string> seen;
  for (const auto& node : fragment.nodes) {
    if (node.kind != "sql_table" || node.label.empty() || !seen.insert(node.id).second) {
      continue;
    }
    facts.push_back(RawRelation{
        .source_id = node.id,
        .target_label = {},
        .relation = std::string(kProvides),
        .context = "table:" + node.label,
        .source_file = context.source_file,
    });
  }
  return facts;
}

ExtractionResult extract_cypher(const ExtractionContext& context) {
  ExtractionResult result;
  const std::filesystem::path source_path(context.relative_path);
  std::string file_label = source_path.filename().string();
  if (source_path.has_parent_path() && source_path.parent_path().has_filename()) {
    file_label = source_path.parent_path().filename().string() + "/" + file_label;
  }
  const auto file_id = make_id(context.relative_path);
  result.fragment.nodes.push_back(Node{
      .id = file_id,
      .label = file_label.empty() ? context.relative_path : std::move(file_label),
      .source_file = context.source_file,
      .source_location = SourceLocation{.start_line = 1, .end_line = 1},
      .kind = "file",
      .confidence = Confidence::Extracted,
  });
  if (is_test_source_path(context.relative_path)) {
    return result;
  }
  Emitter emit(context, result.raw_relations);
  for (const auto& label : cypher_text_labels(context.source, false)) {
    emit.fact(kUses, file_id, "label", label);
  }
  return result;
}

std::vector<RawRelation> orm_table_contract_facts(std::span<const RawRelation> raw_relations,
                                                  const std::function<bool(const std::string&)>& has_sql_table,
                                                  const std::function<std::string(const RawRelation&)>& resolve_name) {
  std::vector<RawRelation> facts;
  std::unordered_map<std::string, std::string> table_of;  // model variable -> its table
  for (const auto& relation : raw_relations) {
    if (relation.relation != "maps_table" || relation.source_id.empty() || relation.target_label.empty()) {
      continue;
    }
    table_of.emplace(relation.source_id, relation.target_label);
    facts.push_back(RawRelation{
        .source_id = relation.source_id,
        .target_label = {},
        .relation = std::string(has_sql_table(relation.target_label) ? kProvides : kUses),
        .context = "table:" + relation.target_label,
        .source_file = relation.source_file,
    });
  }
  for (const auto& relation : raw_relations) {
    if (relation.relation != kOrmTableUse || relation.source_id.empty()) {
      continue;
    }
    const auto model = table_of.find(resolve_name(relation));
    if (model == table_of.end() || has_sql_table(model->second)) {
      continue;  // not a model, or one of the repo's own tables: its users are reached in-repo
    }
    facts.push_back(RawRelation{
        .source_id = relation.source_id,
        .target_label = {},
        .relation = std::string(kUses),
        .context = "table:" + model->second,
        .source_file = relation.source_file,
    });
  }
  return facts;
}

}  // namespace cgraph
