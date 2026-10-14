#include "cgraph/messaging_contracts.hpp"

#include "cgraph/data_contracts.hpp"
#include "cgraph/javascript_syntax.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cgraph {
namespace {

constexpr std::string_view kProvides = "provides_contract";
constexpr std::string_view kUses = "uses_contract";

// ---------------------------------------------------------------------------
// Tree helpers

[[nodiscard]] std::string_view type_of(const TSNode& node) {
  return ts_node_is_null(node) ? std::string_view{} : std::string_view(ts_node_type(node));
}
[[nodiscard]] TSNode field(const TSNode& node, std::string_view name) {
  return ts_node_is_null(node) ? TSNode{}
                               : ts_node_child_by_field_name(node, name.data(), static_cast<std::uint32_t>(name.size()));
}
[[nodiscard]] std::string text(const TSNode& node, std::string_view source) {
  if (ts_node_is_null(node)) {
    return {};
  }
  const auto start = ts_node_start_byte(node);
  const auto end = ts_node_end_byte(node);
  return end <= source.size() && start <= end ? std::string(source.substr(start, end - start)) : std::string{};
}
[[nodiscard]] std::vector<TSNode> named_children(const TSNode& node) {
  std::vector<TSNode> children;
  if (ts_node_is_null(node)) {
    return children;
  }
  const auto count = ts_node_named_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index) {
    children.push_back(ts_node_named_child(node, index));
  }
  return children;
}
[[nodiscard]] TSNode parent(const TSNode& node) { return ts_node_is_null(node) ? TSNode{} : ts_node_parent(node); }
[[nodiscard]] TSNode root_of(TSNode node) {
  for (auto up = ts_node_parent(node); !ts_node_is_null(up); up = ts_node_parent(up)) {
    node = up;
  }
  return node;
}
[[nodiscard]] TSNode unwrap(TSNode node) {
  while (type_of(node) == "parenthesized_expression" || type_of(node) == "argument" ||
         type_of(node) == "value_argument" || type_of(node) == "literal_element" || type_of(node) == "await_expression") {
    const auto children = named_children(node);
    if (children.empty()) {
      break;
    }
    node = children.back();
  }
  return node;
}
[[nodiscard]] std::string_view last_segment(std::string_view dotted) {
  const auto cut = dotted.find_last_of(".:");
  return cut == std::string_view::npos ? dotted : dotted.substr(cut + 1);
}
[[nodiscard]] bool is_upper_initial(std::string_view name) {
  return !name.empty() && std::isupper(static_cast<unsigned char>(name.front())) != 0;
}
[[nodiscard]] bool is_identifier(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
  }) && std::isdigit(static_cast<unsigned char>(name.front())) == 0;
}
[[nodiscard]] std::string strip_suffix(std::string_view name, std::string_view suffix) {
  return name.size() > suffix.size() && name.ends_with(suffix) ? std::string(name.substr(0, name.size() - suffix.size()))
                                                                : std::string{};
}
[[nodiscard]] bool contains_lower(std::string_view haystack, std::string_view needle) {
  std::string lower(haystack);
  std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lower.find(needle) != std::string::npos;
}

// A string literal's value in any of the grammars (Go, Java, Kotlin, C#, C++,
// Python, JavaScript/TypeScript); nullopt for anything else, an interpolated
// string included. An escape sequence is not decoded: such a name is refused.
[[nodiscard]] std::optional<std::string> string_value(TSNode node, std::string_view source) {
  node = unwrap(node);
  static constexpr std::array<std::string_view, 7> kStrings = {
      "string",          "string_literal",     "interpreted_string_literal", "raw_string_literal",
      "template_string", "verbatim_string_literal", "line_string_literal"};
  if (std::ranges::find(kStrings, type_of(node)) == kStrings.end()) {
    return std::nullopt;
  }
  if (type_of(node) == "raw_string_literal" && ts_node_named_child_count(node) == 0) {
    auto raw = text(node, source);
    return raw.size() >= 2 ? std::optional<std::string>(raw.substr(1, raw.size() - 2)) : std::nullopt;
  }
  std::string value;
  for (const auto& child : named_children(node)) {
    const auto type = type_of(child);
    if (type == "string_start" || type == "string_end") {
      continue;
    }
    if (type.ends_with("_content") || type == "string_fragment") {
      value += text(child, source);
      continue;
    }
    return std::nullopt;  // interpolation, substitution or escape sequence
  }
  return value;
}

// The literal default of an env read (`os.getenv("X", "d")`,
// `process.env.X || 'd'`, `Environment.GetEnvironmentVariable("X") ?? "d"`,
// `System.getenv("X") ?: "d"`, `System.getenv().getOrDefault("X", "d")`).
[[nodiscard]] std::optional<std::string> env_default(TSNode node, std::string_view source) {
  node = unwrap(node);
  const auto type = type_of(node);
  const auto children = named_children(node);
  const auto is_env = [&](const TSNode& read) {
    const auto spelled = text(read, source);
    return contains_lower(spelled, "getenv") || contains_lower(spelled, "process.env") ||
           contains_lower(spelled, "environ") || contains_lower(spelled, "getenvironmentvariable");
  };
  if ((type == "binary_expression" || type == "boolean_operator" || type == "elvis_expression") && children.size() >= 2) {
    const auto op = field(node, "operator");
    const auto spelled_op = ts_node_is_null(op) ? std::string{} : text(op, source);
    const bool defaulting = type == "elvis_expression" || type == "boolean_operator" || spelled_op == "||" ||
                            spelled_op == "??" || spelled_op.empty();
    if (defaulting && is_env(children.front())) {
      if (auto value = string_value(children.back(), source)) {
        return value;
      }
    }
    return std::nullopt;
  }
  if (type == "call" || type == "method_invocation" || type == "call_expression" || type == "invocation_expression") {
    auto callee = field(node, "function");
    if (ts_node_is_null(callee)) {
      callee = field(node, "name");
    }
    const auto args = ts_node_is_null(field(node, "arguments")) ? TSNode{} : field(node, "arguments");
    if (ts_node_is_null(args) || !is_env(node)) {
      return std::nullopt;
    }
    const auto spelled = text(ts_node_is_null(callee) ? node : callee, source);
    const auto method = last_segment(spelled);
    if (method != "getenv" && method != "get" && method != "getOrDefault") {
      return std::nullopt;
    }
    std::vector<TSNode> positional;
    for (const auto& arg : named_children(args)) {
      if (type_of(arg) != "keyword_argument" && type_of(arg) != "comment") {
        positional.push_back(arg);
      }
    }
    if (positional.size() == 2) {
      return string_value(positional[1], source);
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// The per-file index: what each name in the file is bound to.

enum class Lang { Go, Java, Kotlin, CSharp, Cpp, Python, Js };

struct FileIndex {
  bool built = false;
  // name -> the literal values it is bound to, or names it is bound to (one hop)
  std::unordered_map<std::string, std::set<std::string>> values;
  std::unordered_map<std::string, std::set<std::string>> aliases;
  // name -> the gRPC services of the clients it is bound to
  std::unordered_map<std::string, std::set<std::string>> clients;
  // name -> the declared (or constructing) simple type name
  std::unordered_map<std::string, std::set<std::string>> types;
  // Python class -> the gRPC service its servicer base names
  std::unordered_map<std::string, std::string> servicers;
  std::string imports;  // the file's import texts, newline-joined
  // C# `using A = X.<S>.<S>Client;`: A -> S
  std::unordered_map<std::string, std::string> using_aliases;
  bool grpc_import = false;
  // Go package alias / JavaScript imported name -> the path it is imported from
  std::unordered_map<std::string, std::string> import_paths;
  // names also bound to a declared type or a constructed object that is no
  // client (`HttpClient _client` beside a `CurrencyServiceClient _client`)
  std::set<std::string> non_clients;
};

// The path segment an import path names a package by: its last, or the one
// before a `/vN` major-version suffix (`github.com/redis/go-redis/v9`).
[[nodiscard]] std::string go_package_name(std::string_view path) {
  auto cut = path.rfind('/');
  auto last = cut == std::string_view::npos ? path : path.substr(cut + 1);
  if (last.size() >= 2 && last.front() == 'v' &&
      std::ranges::all_of(last.substr(1), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; }) &&
      cut != std::string_view::npos) {
    const auto head = path.substr(0, cut);
    const auto previous = head.rfind('/');
    last = previous == std::string_view::npos ? head : head.substr(previous + 1);
  }
  return std::string(last);
}

// True for an import path generated protobuf code lives under: a segment
// naming proto / grpc (`genproto`, `proto-gen`, `grpc_health_v1`) or a `pb`
// package (`pb`, `oteldemopb`), or (JavaScript) a relative path.
[[nodiscard]] bool is_generated_stub_path(std::string_view path) {
  if (path.starts_with('.')) {
    return true;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    auto end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    const auto segment = path.substr(start, end - start);
    if (contains_lower(segment, "proto") || contains_lower(segment, "grpc") || segment.ends_with("pb") ||
        segment.starts_with("pb")) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

// The name an expression binds or reads: an identifier, or the last member of
// `this.x`, `self.x`, `cs.cartSvcClient`, `_x`.
[[nodiscard]] std::string bound_name(TSNode node, std::string_view source) {
  node = unwrap(node);
  const auto type = type_of(node);
  if (type == "identifier" || type == "simple_identifier" || type == "field_identifier" ||
      type == "property_identifier" || type == "shorthand_property_identifier_pattern") {
    return text(node, source);
  }
  for (const char* name : {"field", "attribute", "property", "name"}) {
    if (type == "selector_expression" || type == "attribute" || type == "member_expression" ||
        type == "field_access" || type == "member_access_expression") {
      const auto member = field(node, name);
      if (!ts_node_is_null(member)) {
        return text(member, source);
      }
    }
  }
  if (type == "navigation_expression") {
    const auto children = named_children(node);
    if (!children.empty()) {
      return std::string(last_segment(text(children.back(), source)));
    }
  }
  return {};
}

// The simple name of a type node: `pb.CartServiceClient` is
// `CartServiceClient`, `KafkaTemplate<K, V>` is `KafkaTemplate`, `*T` is `T`.
[[nodiscard]] std::string simple_type(TSNode node, std::string_view source) {
  node = unwrap(node);
  const auto type = type_of(node);
  if (type == "generic_type" || type == "generic_name") {
    const auto children = named_children(node);
    return children.empty() ? std::string{} : simple_type(children.front(), source);
  }
  if (type == "pointer_type" || type == "type_annotation" || type == "nullable_type" || type == "user_type") {
    const auto children = named_children(node);
    return children.empty() ? std::string{} : simple_type(children.back(), source);
  }
  auto spelled = text(node, source);
  const auto angle = spelled.find('<');
  if (angle != std::string::npos) {
    spelled.resize(angle);
  }
  return std::string(last_segment(spelled));
}

// The service a gRPC client constructor names, empty when `node` is none.
[[nodiscard]] std::string client_constructor_service(TSNode node, std::string_view source, Lang lang,
                                                     const FileIndex* index) {
  node = unwrap(node);
  const auto type = type_of(node);
  if (lang == Lang::Go && type == "call_expression") {
    const auto callee = field(node, "function");
    const auto args = named_children(field(node, "arguments"));
    if (type_of(callee) != "selector_expression" || args.size() != 1) {
      return {};
    }
    const auto name = text(field(callee, "field"), source);
    if (!name.starts_with("New") || index == nullptr) {
      return {};
    }
    const auto package = index->import_paths.find(text(field(callee, "operand"), source));
    if (package == index->import_paths.end() || !is_generated_stub_path(package->second)) {
      return {};
    }
    return strip_suffix(std::string_view(name).substr(3), "Client");
  }
  if (lang == Lang::Java && type == "method_invocation") {
    const auto name = text(field(node, "name"), source);
    const auto object = field(node, "object");
    if (name.starts_with("with") && !ts_node_is_null(object)) {
      return client_constructor_service(object, source, lang, index);
    }
    if (!name.starts_with("new") || !name.ends_with("Stub") || ts_node_is_null(object)) {
      return {};
    }
    return strip_suffix(last_segment(text(object, source)), "Grpc");
  }
  if (lang == Lang::CSharp && type == "object_creation_expression") {
    const auto created = field(node, "type");
    if (type_of(created) != "qualified_name") {
      return {};
    }
    const auto qualifier = std::string(last_segment(text(field(created, "qualifier"), source)));
    const auto name = text(field(created, "name"), source);
    return name == qualifier + "Client" ? qualifier : std::string{};
  }
  if (lang == Lang::Python && type == "call") {
    if (index == nullptr || index->imports.find("grpc") == std::string::npos) {
      return {};
    }
    const auto callee = std::string(last_segment(text(field(node, "function"), source)));
    return strip_suffix(callee, "Stub");
  }
  if (lang == Lang::Js && type == "new_expression" && index != nullptr && index->grpc_import) {
    const auto constructor = field(node, "constructor");
    if (type_of(constructor) == "identifier") {
      const auto imported = index->import_paths.find(text(constructor, source));
      if (imported == index->import_paths.end() || !is_generated_stub_path(imported->second)) {
        return {};
      }
      return strip_suffix(text(constructor, source), "Client");
    }
    if (type_of(constructor) == "member_expression" && named_children(field(node, "arguments")).size() >= 2) {
      const auto name = text(field(constructor, "property"), source);
      return is_upper_initial(name) ? name : std::string{};
    }
  }
  return {};
}

// The service a declared client type names: Go `pb.<S>Client`, C#
// `<S>.<S>Client`, Java `<S>[Blocking|Future|BlockingV2]Stub` in a file that
// names `<S>Grpc`, TypeScript `<S>Client` in a file importing grpc.
[[nodiscard]] std::string client_type_service(TSNode node, std::string_view source, Lang lang,
                                              const FileIndex& index, std::string_view file_source) {
  node = unwrap(node);
  if (lang == Lang::Go) {
    // A generated client is an interface, held by value: `*pkg.XClient` and
    // `pkg.XClient{...}` are some other library's struct.
    if (type_of(node) != "qualified_type") {
      return {};
    }
    const auto package = index.import_paths.find(text(field(node, "package"), source));
    if (package == index.import_paths.end() || !is_generated_stub_path(package->second)) {
      return {};
    }
    return strip_suffix(text(field(node, "name"), source), "Client");
  }
  if (lang == Lang::CSharp) {
    if (type_of(node) == "identifier") {
      const auto alias = index.using_aliases.find(text(node, source));
      return alias == index.using_aliases.end() ? std::string{} : alias->second;
    }
    if (type_of(node) != "qualified_name") {
      return {};
    }
    const auto qualifier = std::string(last_segment(text(field(node, "qualifier"), source)));
    return text(field(node, "name"), source) == qualifier + "Client" ? qualifier : std::string{};
  }
  if (lang == Lang::Java) {
    const auto name = simple_type(node, source);
    for (const std::string_view suffix : {"BlockingV2Stub", "BlockingStub", "FutureStub", "Stub"}) {
      auto service = strip_suffix(name, suffix);
      if (!service.empty() && file_source.find(service + "Grpc") != std::string_view::npos) {
        return service;
      }
    }
    return {};
  }
  if (lang == Lang::Js && index.grpc_import) {
    const auto name = simple_type(node, source);
    const auto imported = index.import_paths.find(name);
    if (imported == index.import_paths.end() || !is_generated_stub_path(imported->second)) {
      return {};
    }
    return strip_suffix(name, "Client");
  }
  return {};
}

void bind_value(FileIndex& index, const std::string& name, TSNode value, std::string_view source) {
  if (name.empty() || ts_node_is_null(value)) {
    return;
  }
  if (auto literal = string_value(value, source)) {
    index.values[name].insert(*literal);
  } else if (auto fallback = env_default(value, source)) {
    index.values[name].insert(*fallback);
  } else if (auto alias = bound_name(value, source); !alias.empty() && alias != name) {
    index.aliases[name].insert(alias);
  }
}

void bind_name(FileIndex& index, const std::string& name, TSNode type_node, TSNode value, std::string_view source,
          Lang lang, std::string_view file_source) {
  if (name.empty()) {
    return;
  }
  if (!ts_node_is_null(type_node)) {
    auto service = client_type_service(type_node, source, lang, index, file_source);
    if (!service.empty()) {
      index.clients[name].insert(service);
    }
    if (auto simple = simple_type(type_node, source); !simple.empty()) {
      index.types[name].insert(simple);
      if (service.empty() && simple != "var" && simple != "auto") {
        index.non_clients.insert(name);
      }
    }
  }
  if (!ts_node_is_null(value)) {
    const auto service = client_constructor_service(value, source, lang, &index);
    if (!service.empty()) {
      index.clients[name].insert(service);
    }
    const auto unwrapped = unwrap(value);
    if (service.empty() &&
        (type_of(unwrapped) == "object_creation_expression" || type_of(unwrapped) == "new_expression")) {
      index.non_clients.insert(name);
    }
    // The constructor a name is assigned from types it (Python, Kotlin and
    // JavaScript have no declared type): `producer = KafkaProducer(...)`.
    if (type_of(unwrapped) == "call" || type_of(unwrapped) == "object_creation_expression" ||
        type_of(unwrapped) == "new_expression") {
      auto callee = field(unwrapped, "function");
      if (ts_node_is_null(callee)) callee = field(unwrapped, "type");
      if (ts_node_is_null(callee)) callee = field(unwrapped, "constructor");
      if (auto simple = simple_type(callee, source); !simple.empty()) {
        index.types[name].insert(simple);
      }
    }
    bind_value(index, name, value, source);
  }
}

// Go `import pb "x/genproto"` (alias or package name -> path) and JavaScript
// `import { CartServiceClient } from '../protos/demo'` (local name -> path).
void record_import_paths(const TSNode& node, FileIndex& index, std::string_view source, Lang lang) {
  std::vector<TSNode> stack{node};
  while (!stack.empty()) {
    const auto current = stack.back();
    stack.pop_back();
    const auto type = type_of(current);
    if (lang == Lang::Go && type == "import_spec") {
      const auto path = string_value(field(current, "path"), source).value_or("");
      const auto alias = field(current, "name");
      index.import_paths[ts_node_is_null(alias) ? go_package_name(path) : text(alias, source)] = path;
      continue;
    }
    if (lang == Lang::Js && type == "import_statement") {
      const auto path = string_value(field(current, "source"), source).value_or("");
      std::vector<TSNode> names{current};
      while (!names.empty()) {
        const auto name_node = names.back();
        names.pop_back();
        if (type_of(name_node) == "import_specifier") {
          const auto alias = field(name_node, "alias");
          index.import_paths[text(ts_node_is_null(alias) ? field(name_node, "name") : alias, source)] = path;
          continue;
        }
        if (type_of(name_node) == "import_clause") {
          for (const auto& child : named_children(name_node)) {
            if (type_of(child) == "identifier") index.import_paths[text(child, source)] = path;
          }
        }
        for (const auto& child : named_children(name_node)) names.push_back(child);
      }
      continue;
    }
    for (const auto& child : named_children(current)) stack.push_back(child);
  }
}

// Reads one node into the index; false when its children hold nothing more to
// read (an import).
bool collect_node(const TSNode& node, FileIndex& index, std::string_view source, Lang lang) {
  const auto type = type_of(node);
  if (type.find("import") != std::string_view::npos || type == "using_directive") {
    index.imports += text(node, source);
    index.imports += '\n';
    record_import_paths(node, index, source, lang);
    if (lang == Lang::CSharp && type == "using_directive") {
      const auto alias = field(node, "name");
      for (const auto& target : named_children(node)) {
        if (!ts_node_is_null(alias) && type_of(target) == "qualified_name") {
          const auto qualifier = std::string(last_segment(text(field(target, "qualifier"), source)));
          if (text(field(target, "name"), source) == qualifier + "Client") {
            index.using_aliases[text(alias, source)] = qualifier;
          }
        }
      }
    }
    return false;
  }
  switch (lang) {
    case Lang::Go:
      if (type == "const_spec" || type == "var_spec") {
        const auto names = named_children(node);
        const auto value = field(node, "value");
        const auto values = named_children(value);
        const auto type_node = field(node, "type");
        for (const auto& child : names) {
          if (type_of(child) == "identifier") {
            bind_name(index, text(child, source), type_node, values.size() == 1 ? values.front() : TSNode{}, source, lang,
                 source);
          }
        }
      } else if (type == "short_var_declaration" || type == "assignment_statement") {
        const auto left = named_children(field(node, "left"));
        const auto right = named_children(field(node, "right"));
        if (left.size() == right.size()) {
          for (std::size_t i = 0; i < left.size(); ++i) {
            bind_name(index, bound_name(left[i], source), {}, right[i], source, lang, source);
          }
        }
      } else if (type == "keyed_element") {
        const auto key = field(node, "key");
        bind_name(index, bound_name(ts_node_is_null(key) ? TSNode{} : named_children(key).empty() ? key : named_children(key).front(),
                               source),
             {}, field(node, "value"), source, lang, source);
      } else if (type == "field_declaration" || type == "parameter_declaration") {
        const auto type_node = field(node, "type");
        for (const auto& child : named_children(node)) {
          if (type_of(child) == "field_identifier" || type_of(child) == "identifier") {
            bind_name(index, text(child, source), type_node, {}, source, lang, source);
          }
        }
      }
      break;
    case Lang::Java:
      if (type == "field_declaration" || type == "local_variable_declaration") {
        const auto type_node = field(node, "type");
        for (const auto& child : named_children(node)) {
          if (type_of(child) == "variable_declarator") {
            bind_name(index, text(field(child, "name"), source), type_node, field(child, "value"), source, lang, source);
          }
        }
      } else if (type == "formal_parameter") {
        bind_name(index, text(field(node, "name"), source), field(node, "type"), {}, source, lang, source);
      } else if (type == "assignment_expression") {
        bind_name(index, bound_name(field(node, "left"), source), {}, field(node, "right"), source, lang, source);
      }
      break;
    case Lang::Kotlin:
      if (type == "property_declaration") {
        std::string name;
        TSNode value{};
        for (const auto& child : named_children(node)) {
          const auto child_type = type_of(child);
          if (child_type == "variable_declaration") {
            const auto parts = named_children(child);
            if (!parts.empty()) name = text(parts.front(), source);
          } else if (child_type != "binding_pattern_kind" && child_type != "modifiers" && child_type != "type_constraints") {
            value = child;
          }
        }
        bind_name(index, name, {}, value, source, lang, source);
      }
      break;
    case Lang::CSharp:
      if (type == "variable_declaration") {
        const auto type_node = field(node, "type");
        for (const auto& child : named_children(node)) {
          if (type_of(child) != "variable_declarator") continue;
          const auto parts = named_children(child);
          TSNode value{};
          for (const auto& part : parts) {
            if (!ts_node_eq(part, field(child, "name")) && type_of(part) != "bracketed_argument_list") value = part;
          }
          bind_name(index, text(field(child, "name"), source), type_node, value, source, lang, source);
        }
      } else if (type == "parameter") {
        bind_name(index, text(field(node, "name"), source), field(node, "type"), {}, source, lang, source);
      } else if (type == "property_declaration") {
        bind_name(index, text(field(node, "name"), source), field(node, "type"), field(node, "value"), source, lang, source);
      } else if (type == "assignment_expression") {
        bind_name(index, bound_name(field(node, "left"), source), {}, field(node, "right"), source, lang, source);
      }
      break;
    case Lang::Python:
      if (type == "assignment") {
        bind_name(index, bound_name(field(node, "left"), source), field(node, "type"), field(node, "right"), source, lang,
             source);
      } else if (type == "class_definition") {
        const auto name = text(field(node, "name"), source);
        for (const auto& base : named_children(field(node, "superclasses"))) {
          if (auto service = strip_suffix(last_segment(text(base, source)), "Servicer"); !service.empty()) {
            index.servicers[name] = service;
          }
        }
      }
      break;
    case Lang::Js:
      if (type == "variable_declarator" || type == "public_field_definition" || type == "field_definition") {
        bind_name(index, text(field(node, "name"), source), field(node, "type"), field(node, "value"), source, lang, source);
      } else if (type == "required_parameter" || type == "optional_parameter") {
        bind_name(index, text(field(node, "pattern"), source), field(node, "type"), {}, source, lang, source);
      } else if (type == "assignment_expression") {
        bind_name(index, bound_name(field(node, "left"), source), {}, field(node, "right"), source, lang, source);
      } else if (type == "call_expression" && text(field(node, "function"), source) == "require") {
        index.imports += text(node, source);
        index.imports += '\n';
      }
      break;
    case Lang::Cpp:
      break;
  }
  return true;
}

// Walks the whole tree with an explicit stack: a file's tree can be deeper
// than a worker thread's stack allows a recursion to go (extractor.cpp caps
// its own walk for the same reason).
void collect(const TSNode& root, FileIndex& index, std::string_view source, Lang lang) {
  std::vector<TSNode> stack{root};
  while (!stack.empty()) {
    const auto node = stack.back();
    stack.pop_back();
    if (!collect_node(node, index, source, lang)) {
      continue;
    }
    auto children = named_children(node);
    stack.insert(stack.end(), children.rbegin(), children.rend());
  }
}

thread_local FileIndex* current_index = nullptr;

// The index of the file `node` is in: built once per MessagingContractsFileScope
// (one per extracted file); without a scope, built afresh for each call.
FileIndex& file_index(const TSNode& node, const ExtractionContext& context, Lang lang) {
  thread_local FileIndex unscoped;
  FileIndex& index = current_index != nullptr ? *current_index : unscoped;
  if (current_index != nullptr && index.built) {
    return index;
  }
  index = FileIndex{};
  index.built = true;
  const auto root = root_of(node);
  // The grpc import is needed while collecting (a JavaScript client
  // constructor counts only with it), so read the imports first.
  for (const auto& child : named_children(root)) {
    const auto type = type_of(child);
    if (type.find("import") != std::string_view::npos || type == "lexical_declaration" ||
        type == "variable_declaration") {
      const auto spelled = text(child, context.source);
      if (spelled.find("@grpc/grpc-js") != std::string::npos || spelled.find("'grpc'") != std::string::npos ||
          spelled.find("\"grpc\"") != std::string::npos) {
        index.grpc_import = true;
      }
    }
  }
  collect(root, index, context.source, lang);
  // One hop through names bound to names: `const TOPIC = DEFAULT_TOPIC`.
  for (const auto& [name, targets] : index.aliases) {
    for (const auto& target : targets) {
      if (const auto found = index.values.find(target); found != index.values.end()) {
        auto copy = found->second;
        index.values[name].insert(copy.begin(), copy.end());
      }
      if (const auto found = index.clients.find(target); found != index.clients.end()) {
        auto copy = found->second;
        index.clients[name].insert(copy.begin(), copy.end());
      }
    }
  }
  // A Python class deriving a same-file servicer class serves its service too.
  for (int pass = 0; pass < 3 && lang == Lang::Python; ++pass) {
    std::vector<std::pair<std::string, std::string>> found;
    std::vector<TSNode> stack{root};
    while (!stack.empty()) {
      const auto current = stack.back();
      stack.pop_back();
      if (type_of(current) == "class_definition") {
        const auto name = text(field(current, "name"), context.source);
        for (const auto& base : named_children(field(current, "superclasses"))) {
          const auto base_name = text(base, context.source);
          if (const auto service = index.servicers.find(base_name);
              service != index.servicers.end() && !index.servicers.contains(name)) {
            found.emplace_back(name, service->second);
          }
        }
      }
      for (const auto& child : named_children(current)) stack.push_back(child);
    }
    for (auto& [name, service] : found) index.servicers.emplace(std::move(name), std::move(service));
  }
  return index;
}

[[nodiscard]] bool imports_any(const FileIndex& index, std::initializer_list<std::string_view> modules) {
  return std::ranges::any_of(modules, [&](std::string_view module) { return index.imports.find(module) != std::string::npos; });
}

// True when `node` is a plain name that a parameter of an enclosing function
// declares: `def forward(topic): producer.send(topic, ...)` reads its argument,
// not a same-named binding elsewhere in the file.
[[nodiscard]] bool shadowed_by_parameter(TSNode node, const std::string& name, std::string_view source) {
  node = unwrap(node);
  const auto type = type_of(node);
  if (type != "identifier" && type != "simple_identifier") {
    return false;
  }
  for (auto up = parent(node); !ts_node_is_null(up); up = parent(up)) {
    auto parameters = field(up, "parameters");
    if (ts_node_is_null(parameters)) {
      for (const auto& child : named_children(up)) {
        if (type_of(child) == "function_value_parameters") parameters = child;
      }
    }
    if (ts_node_is_null(parameters)) {
      continue;
    }
    std::vector<TSNode> stack{parameters};
    while (!stack.empty()) {
      const auto current = stack.back();
      stack.pop_back();
      const auto current_type = type_of(current);
      if ((current_type == "identifier" || current_type == "simple_identifier") && text(current, source) == name) {
        return true;
      }
      for (const auto& child : named_children(current)) stack.push_back(child);
    }
  }
  return false;
}

// The one value a name argument reads: a literal, an env default, or a name
// bound to exactly one value in the file.
[[nodiscard]] std::optional<std::string> name_value(TSNode node, std::string_view source, const FileIndex& index) {
  if (auto literal = string_value(node, source)) {
    return literal;
  }
  if (auto fallback = env_default(node, source)) {
    return fallback;
  }
  const auto name = bound_name(node, source);
  if (const auto found = index.values.find(name); !name.empty() && found != index.values.end() &&
                                                  found->second.size() == 1 && !shadowed_by_parameter(node, name, source)) {
    return *found->second.begin();
  }
  return std::nullopt;
}

// The service the client `receiver` is, inline or through a bound name.
[[nodiscard]] std::string receiver_service(TSNode receiver, std::string_view source, Lang lang, const FileIndex& index) {
  if (auto service = client_constructor_service(receiver, source, lang, &index); !service.empty()) {
    return service;
  }
  const auto name = bound_name(receiver, source);
  if (const auto found = index.clients.find(name); !name.empty() && found != index.clients.end() &&
                                                   found->second.size() == 1 && !index.non_clients.contains(name)) {
    return *found->second.begin();
  }
  return {};
}

[[nodiscard]] bool receiver_typed(TSNode receiver, std::string_view source, const FileIndex& index,
                                  std::initializer_list<std::string_view> types) {
  const auto name = bound_name(receiver, source);
  const auto found = index.types.find(name);
  if (name.empty() || found == index.types.end()) {
    return false;
  }
  return std::ranges::any_of(found->second, [&](const std::string& type) {
    return std::ranges::find(types, std::string_view(type)) != types.end();
  });
}

void emit(std::vector<RawRelation>& out, std::string_view relation, const std::string& source_id,
          const std::string& contract, const ExtractionContext& context) {
  if (source_id.empty()) {
    return;
  }
  for (const auto& existing : out) {
    if (existing.relation == relation && existing.source_id == source_id && existing.context == contract) {
      return;
    }
  }
  out.push_back(RawRelation{
      .source_id = source_id,
      .relation = std::string(relation),
      .context = contract,
      .source_file = context.source_file,
  });
}

void emit_grpc(std::vector<RawRelation>& out, std::string_view relation, const std::string& source_id,
               std::string_view service, std::string_view method, const ExtractionContext& context) {
  const auto name = grpc_method_name(method);
  if (!is_identifier(service) || !is_identifier(name) || is_standard_grpc_service(service)) {
    return;
  }
  emit(out, relation, source_id, "grpc:" + std::string(service) + "/" + name, context);
}

void emit_topic(std::vector<RawRelation>& out, std::string_view relation, const std::string& source_id,
                const std::optional<std::string>& name, const ExtractionContext& context) {
  if (!name) {
    return;
  }
  const auto topic = topic_name_from_address(*name);
  // `amq.*` is RabbitMQ's reserved space (`amq.rabbitmq.reply-to`): the
  // broker's own queues and exchanges, never one service's contract with another.
  if (is_topic_name(topic) && !topic.starts_with("amq.")) {
    emit(out, relation, source_id, "topic:" + topic, context);
  }
}

[[nodiscard]] std::vector<TSNode> arguments_of(const TSNode& call) {
  auto args = field(call, "arguments");
  if (ts_node_is_null(args)) {
    // Kotlin: call_expression -> call_suffix -> value_arguments
    for (const auto& child : named_children(call)) {
      if (type_of(child) == "call_suffix") {
        for (const auto& part : named_children(child)) {
          if (type_of(part) == "value_arguments") args = part;
        }
      }
    }
  }
  std::vector<TSNode> out;
  if (ts_node_is_null(args)) {
    return out;
  }
  for (const auto& arg : named_children(args)) {
    if (type_of(arg) != "comment") out.push_back(arg);
  }
  return out;
}

// A keyword argument's value (Python `routing_key=...`, C# `routingKey: ...`).
[[nodiscard]] TSNode keyword(const std::vector<TSNode>& args, std::string_view name, std::string_view source) {
  for (const auto& arg : args) {
    if (type_of(arg) == "keyword_argument" && text(field(arg, "name"), source) == name) {
      return field(arg, "value");
    }
    if (type_of(arg) == "argument") {
      const auto label = field(arg, "name");
      if (!ts_node_is_null(label) && text(label, source) == name) {
        const auto children = named_children(arg);
        return children.back();
      }
    }
  }
  return {};
}
[[nodiscard]] std::vector<TSNode> positional(const std::vector<TSNode>& args, std::string_view source) {
  std::vector<TSNode> out;
  for (const auto& arg : args) {
    if (type_of(arg) == "keyword_argument") continue;
    if (type_of(arg) == "argument" && !ts_node_is_null(field(arg, "name"))) continue;
    (void)source;
    out.push_back(arg);
  }
  return out;
}

// The string elements of a list expression (`List.of("a")`, `listOf("a")`,
// `["a"]`, `[]string{"a"}`, `new[] { "a" }`) or the one name argument itself.
[[nodiscard]] std::vector<std::optional<std::string>> name_list(TSNode node, std::string_view source,
                                                                const FileIndex& index) {
  node = unwrap(node);
  const auto type = type_of(node);
  std::vector<std::optional<std::string>> names;
  if (type == "list" || type == "array" || type == "element_value_array_initializer" ||
      type == "array_initializer" || type == "collection_expression") {
    for (const auto& item : named_children(node)) names.push_back(name_value(item, source, index));
    return names;
  }
  if (type == "composite_literal") {
    for (const auto& item : named_children(field(node, "body"))) names.push_back(name_value(item, source, index));
    return names;
  }
  if (type == "implicit_array_creation_expression" || type == "array_creation_expression" ||
      type == "object_creation_expression") {
    for (const auto& child : named_children(node)) {
      if (type_of(child) == "initializer_expression") {
        for (const auto& item : named_children(child)) names.push_back(name_value(item, source, index));
      }
    }
    return names;
  }
  if (type == "method_invocation" || type == "call_expression" || type == "call") {
    const auto children = named_children(node);
    const auto callee = text(ts_node_is_null(field(node, "name")) ? (children.empty() ? TSNode{} : children.front())
                                                                   : field(node, "name"),
                             source);
    static constexpr std::array<std::string_view, 9> kListCalls = {
        "asList", "of", "singletonList", "singleton", "listOf", "setOf", "arrayListOf", "mutableListOf", "list"};
    if (std::ranges::find(kListCalls, last_segment(callee)) != kListCalls.end()) {
      for (const auto& item : arguments_of(node)) names.push_back(name_value(item, source, index));
    }
    return names;
  }
  names.push_back(name_value(node, source, index));
  return names;
}

// ---------------------------------------------------------------------------
// Go

[[nodiscard]] TSNode enclosing(TSNode node, std::initializer_list<std::string_view> types) {
  for (auto up = parent(node); !ts_node_is_null(up); up = parent(up)) {
    if (std::ranges::find(types, type_of(up)) != types.end()) return up;
  }
  return {};
}

// The type a Go server value is: `&T{}`, `T{}`, `new(T)`, or a local of the
// enclosing function assigned one.
[[nodiscard]] std::string go_impl_type(TSNode value, const TSNode& function, std::string_view source, int depth = 0) {
  value = unwrap(value);
  const auto type = type_of(value);
  if (type == "unary_expression") {
    return go_impl_type(field(value, "operand"), function, source, depth);
  }
  if (type == "composite_literal") {
    return simple_type(field(value, "type"), source);
  }
  if (type == "call_expression" && text(field(value, "function"), source) == "new") {
    const auto args = named_children(field(value, "arguments"));
    return args.size() == 1 ? simple_type(args.front(), source) : std::string{};
  }
  if (type == "identifier" && depth == 0 && !ts_node_is_null(function)) {
    const auto name = text(value, source);
    std::string found;
    std::vector<TSNode> stack{function};
    while (!stack.empty()) {
      const auto current = stack.back();
      stack.pop_back();
      const auto current_type = type_of(current);
      if (current_type == "short_var_declaration" || current_type == "assignment_statement") {
        const auto left = named_children(field(current, "left"));
        const auto right = named_children(field(current, "right"));
        for (std::size_t i = 0; i < left.size() && i < right.size(); ++i) {
          if (text(left[i], source) == name) {
            auto bound = go_impl_type(right[i], function, source, depth + 1);
            if (!bound.empty()) found = bound;
          }
        }
      }
      for (const auto& child : named_children(current)) stack.push_back(child);
    }
    return found;
  }
  return {};
}

// A generated Go client method takes a context first, then the request (a
// client-streaming method takes the context alone), then only `grpc.X(...)`
// call options: `client.GetCart(ctx, req)`, `client.Watch(ctx)`,
// `client.Get(ctx, req, grpc.WaitForReady(true))`.
[[nodiscard]] bool go_rpc_call_shape(const std::vector<TSNode>& args, std::string_view source) {
  if (args.empty() || args.size() > 2 + 4) {
    return false;
  }
  const auto first = text(args[0], source);
  if (!contains_lower(first, "ctx") && !contains_lower(first, "context")) {
    return false;
  }
  for (std::size_t index = 2; index < args.size(); ++index) {
    if (type_of(args[index]) != "call_expression" || !text(field(args[index], "function"), source).starts_with("grpc.")) {
      return false;
    }
  }
  return true;
}

// `(context.Context, *P.Req) (*P.Resp, error)`: the package alias P, empty
// when the method has another shape.
[[nodiscard]] std::string go_unary_handler_package(const TSNode& method, std::string_view source) {
  const auto params = named_children(field(method, "parameters"));
  const auto results = named_children(field(method, "result"));
  if (params.size() != 2 || results.size() != 2) {
    return {};
  }
  if (text(field(params[0], "type"), source) != "context.Context" || text(field(results[1], "type"), source) != "error") {
    return {};
  }
  const auto message_package = [&](const TSNode& declaration) -> std::string {
    const auto declared = field(declaration, "type");
    if (type_of(declared) != "pointer_type") return {};
    const auto pointee = named_children(declared);
    if (pointee.empty() || type_of(pointee.front()) != "qualified_type") return {};
    return text(field(pointee.front(), "package"), source);
  };
  const auto request = message_package(params[1]);
  return !request.empty() && request == message_package(results[0]) ? request : std::string{};
}

}  // namespace

struct MessagingContractsFileScope::Index {
  FileIndex index;
  FileIndex* previous = nullptr;
};

MessagingContractsFileScope::MessagingContractsFileScope() : index_(std::make_unique<Index>()) {
  index_->previous = current_index;
  current_index = &index_->index;
}

MessagingContractsFileScope::~MessagingContractsFileScope() { current_index = index_->previous; }

bool is_standard_grpc_service(std::string_view service) {
  return service == "Health" || service == "ServerReflection";
}

std::string grpc_method_name(std::string_view name) {
  std::string spelled(name);
  if (!spelled.empty()) {
    spelled.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(spelled.front())));
  }
  return spelled;
}

bool is_topic_name(std::string_view name) {
  return !name.empty() && name.size() <= 249 && std::ranges::all_of(name, [](char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '.' || ch == '_' || ch == '-';
  });
}

std::string topic_name_from_address(std::string_view address) {
  if (address.starts_with("arn:")) {
    return std::string(address.substr(address.rfind(':') + 1));
  }
  if (address.starts_with("https://") || address.starts_with("http://")) {
    auto trimmed = address;
    while (trimmed.ends_with('/')) trimmed.remove_suffix(1);
    return std::string(trimmed.substr(trimmed.rfind('/') + 1));
  }
  return std::string(address);
}

void go_messaging_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                            std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  if ((type != "call_expression" && type != "method_declaration" && type != "composite_literal") ||
      function_scope_id.empty() || is_test_source_path(context.relative_path)) {
    return;
  }
  const auto source = context.source;
  if (type == "method_declaration") {
    const auto receiver = named_children(field(node, "receiver"));
    if (receiver.size() != 1) return;
    const auto receiver_type = simple_type(field(receiver.front(), "type"), source);
    const auto method = text(field(node, "name"), source);
    if (!is_upper_initial(method)) return;
    if (auto package = go_unary_handler_package(node, source); !package.empty()) {
      out.push_back(RawRelation{.source_id = function_scope_id,
                                .target_label = receiver_type,
                                .relation = std::string(kGrpcHandlerRelation),
                                .context = method + "\n" + package,
                                .source_file = context.source_file});
      return;
    }
    // Streaming: a `<P>.<S>_<M>Server` parameter names the service and method.
    for (const auto& param : named_children(field(node, "parameters"))) {
      const auto declared = field(param, "type");
      if (type_of(declared) != "qualified_type") continue;
      const auto name = text(field(declared, "name"), source);
      const auto underscore = name.find('_');
      if (underscore == std::string::npos || !name.ends_with("Server")) continue;
      const auto service = name.substr(0, underscore);
      const auto rpc = name.substr(underscore + 1, name.size() - underscore - 1 - 6);
      if (rpc == method) emit_grpc(out, kProvides, function_scope_id, service, rpc, context);
    }
    return;
  }
  auto& index = file_index(node, context, Lang::Go);
  if (type == "composite_literal") {
    const auto literal_type = field(node, "type");
    if (type_of(literal_type) != "qualified_type") return;
    const auto package = text(field(literal_type, "package"), source);
    const auto name = text(field(literal_type, "name"), source);
    const bool sarama = package == "sarama" && imports_any(index, {"sarama"});
    const bool kafka_go = package == "kafka" && imports_any(index, {"segmentio/kafka-go", "confluent-kafka-go"});
    if (!sarama && !kafka_go) return;
    const bool produces = name == "ProducerMessage" || name == "Message" || name == "Writer" || name == "WriterConfig";
    const bool consumes = name == "ReaderConfig";
    if (!produces && !consumes) return;
    for (const auto& element : named_children(field(node, "body"))) {
      if (type_of(element) != "keyed_element") continue;
      const auto key = text(field(element, "key"), source);
      if (key == "Topic") {
        emit_topic(out, produces ? kProvides : kUses, function_scope_id, name_value(field(element, "value"), source, index),
                   context);
      } else if (key == "GroupTopics" && consumes) {
        for (const auto& topic : name_list(field(element, "value"), source, index)) {
          emit_topic(out, kUses, function_scope_id, topic, context);
        }
      }
    }
    return;
  }
  // call_expression
  const auto callee = field(node, "function");
  if (type_of(callee) != "selector_expression") return;
  const auto receiver = field(callee, "operand");
  const auto method = text(field(callee, "field"), source);
  const auto args = arguments_of(node);
  if (method.starts_with("Register") && method.ends_with("Server") && args.size() == 2 &&
      type_of(receiver) == "identifier") {
    const auto service = method.substr(8, method.size() - 8 - 6);
    const auto function = enclosing(node, {"function_declaration", "method_declaration", "func_literal"});
    const auto impl = go_impl_type(args[1], function, source);
    if (!service.empty() && !impl.empty() && !is_standard_grpc_service(service)) {
      out.push_back(RawRelation{.source_id = function_scope_id,
                                .target_label = impl,
                                .relation = std::string(kGrpcServerRelation),
                                .context = service + "\n" + text(receiver, source),
                                .source_file = context.source_file});
    }
    return;
  }
  if (is_upper_initial(method) && !method.starts_with("New") && go_rpc_call_shape(args, source)) {
    if (auto service = receiver_service(receiver, source, Lang::Go, index); !service.empty()) {
      emit_grpc(out, kUses, function_scope_id, service, method, context);
      return;
    }
  }
  const bool kafka = imports_any(index, {"sarama", "segmentio/kafka-go", "confluent-kafka-go"});
  const bool amqp = imports_any(index, {"amqp091-go", "streadway/amqp"});
  if (kafka && method == "ConsumePartition" && !args.empty()) {
    emit_topic(out, kUses, function_scope_id, name_value(args[0], source, index), context);
  } else if (kafka && method == "Consume" && args.size() == 3) {
    for (const auto& topic : name_list(args[1], source, index)) emit_topic(out, kUses, function_scope_id, topic, context);
  } else if (kafka && method == "SubscribeTopics" && !args.empty()) {
    for (const auto& topic : name_list(args[0], source, index)) emit_topic(out, kUses, function_scope_id, topic, context);
  } else if (amqp && (method == "Publish" || method == "PublishWithContext")) {
    const std::size_t at = method == "Publish" ? 0 : 1;
    if (args.size() > at + 1 && string_value(args[at], source) == std::optional<std::string>("")) {
      emit_topic(out, kProvides, function_scope_id, name_value(args[at + 1], source, index), context);
    }
  } else if (amqp && method == "Consume" && args.size() == 7) {
    emit_topic(out, kUses, function_scope_id, name_value(args[0], source, index), context);
  }
}

std::vector<RawRelation> grpc_server_contract_facts(std::span<const RawRelation> raw_relations) {
  const auto directory = [](const std::string& file) {
    const auto slash = file.rfind('/');
    return slash == std::string::npos ? std::string{} : file.substr(0, slash);
  };
  // (directory, impl type, package alias) -> services registered with it
  std::map<std::tuple<std::string, std::string, std::string>, std::set<std::string>> servers;
  for (const auto& relation : raw_relations) {
    if (relation.relation != kGrpcServerRelation) continue;
    const auto cut = relation.context.find('\n');
    if (cut == std::string::npos) continue;
    servers[{directory(relation.source_file), relation.target_label, relation.context.substr(cut + 1)}].insert(
        relation.context.substr(0, cut));
  }
  std::vector<RawRelation> facts;
  // A type registered for two services: which handler serves which is in the
  // .proto, not here, so it provides nothing rather than every method twice.
  std::erase_if(servers, [](const auto& entry) { return entry.second.size() != 1; });
  if (servers.empty()) {
    return facts;
  }
  for (const auto& relation : raw_relations) {
    if (relation.relation != kGrpcHandlerRelation) continue;
    const auto cut = relation.context.find('\n');
    if (cut == std::string::npos) continue;
    const auto found = servers.find({directory(relation.source_file), relation.target_label, relation.context.substr(cut + 1)});
    if (found == servers.end()) continue;
    for (const auto& service : found->second) {
      facts.push_back(RawRelation{.source_id = relation.source_id,
                                  .relation = std::string(kProvides),
                                  .context = "grpc:" + service + "/" + relation.context.substr(0, cut),
                                  .source_file = relation.source_file});
    }
  }
  return facts;
}

// ---------------------------------------------------------------------------
// Java

void java_messaging_contracts(const TSNode& node, const ExtractionContext& context,
                              const std::string& function_scope_id, std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  if ((type != "method_invocation" && type != "method_declaration" && type != "annotation" &&
       type != "object_creation_expression") ||
      function_scope_id.empty() || is_test_source_path(context.relative_path)) {
    return;
  }
  const auto source = context.source;
  if (type == "method_declaration") {
    const auto body = parent(node);
    const auto owner = parent(body);
    if (type_of(owner) != "class_declaration") return;
    const auto superclass = named_children(field(owner, "superclass"));
    if (superclass.empty()) return;
    const auto service = strip_suffix(simple_type(superclass.front(), source), "ImplBase");
    if (service.empty()) return;
    for (const auto& param : named_children(field(node, "parameters"))) {
      if (simple_type(field(param, "type"), source) == "StreamObserver") {
        emit_grpc(out, kProvides, function_scope_id, service, text(field(node, "name"), source), context);
        return;
      }
    }
    return;
  }
  auto& index = file_index(node, context, Lang::Java);
  if (type == "annotation") {
    const auto name = text(field(node, "name"), source);
    const bool kafka = name == "KafkaListener";
    const bool rabbit = name == "RabbitListener";
    const bool sqs = name == "SqsListener";
    const bool jms = name == "JmsListener";
    if (!kafka && !rabbit && !sqs && !jms) return;
    for (const auto& argument : named_children(field(node, "arguments"))) {
      std::string key;
      TSNode value = argument;
      if (type_of(argument) == "element_value_pair") {
        key = text(field(argument, "key"), source);
        value = field(argument, "value");
      }
      const bool topics = (kafka && key == "topics") || (rabbit && key == "queues") ||
                          (sqs && (key.empty() || key == "value" || key == "queueNames")) ||
                          (jms && key == "destination");
      if (!topics) continue;
      for (const auto& topic : name_list(value, source, index)) emit_topic(out, kUses, function_scope_id, topic, context);
    }
    return;
  }
  const auto args = arguments_of(node);
  if (type == "object_creation_expression") {
    if (simple_type(field(node, "type"), source) == "ProducerRecord" && !args.empty() &&
        imports_any(index, {"org.apache.kafka"})) {
      emit_topic(out, kProvides, function_scope_id, name_value(args[0], source, index), context);
    }
    return;
  }
  const auto receiver = field(node, "object");
  const auto method = text(field(node, "name"), source);
  if (!ts_node_is_null(receiver) && !method.starts_with("with") && method != "getChannel" &&
      method != "getCallOptions" && !method.starts_with("new")) {
    if (auto service = receiver_service(receiver, source, Lang::Java, index); !service.empty()) {
      emit_grpc(out, kUses, function_scope_id, service, method, context);
      return;
    }
  }
  if (ts_node_is_null(receiver) || args.empty()) return;
  if (method == "send" && receiver_typed(receiver, source, index, {"KafkaTemplate", "ReactiveKafkaProducerTemplate", "SqsTemplate"})) {
    emit_topic(out, kProvides, function_scope_id, name_value(args[0], source, index), context);
  } else if ((method == "convertAndSend" || method == "send") && args.size() == 2 &&
             receiver_typed(receiver, source, index, {"RabbitTemplate", "AmqpTemplate", "RabbitMessagingTemplate"})) {
    emit_topic(out, kProvides, function_scope_id, name_value(args[0], source, index), context);
  } else if (method == "subscribe" && receiver_typed(receiver, source, index, {"KafkaConsumer", "Consumer"})) {
    for (const auto& topic : name_list(args[0], source, index)) emit_topic(out, kUses, function_scope_id, topic, context);
  } else if ((method == "stream" || method == "table" || method == "globalTable") &&
             receiver_typed(receiver, source, index, {"StreamsBuilder"})) {
    emit_topic(out, kUses, function_scope_id, name_value(args[0], source, index), context);
  } else if (method == "setQueueNames" && imports_any(index, {"org.springframework.amqp"})) {
    for (const auto& arg : args) emit_topic(out, kUses, function_scope_id, name_value(arg, source, index), context);
  }
}

// ---------------------------------------------------------------------------
// Kotlin

void kotlin_messaging_contracts(const TSNode& node, const ExtractionContext& context,
                                const std::string& function_scope_id, std::vector<RawRelation>& out) {
  if (type_of(node) != "call_expression" || function_scope_id.empty() || is_test_source_path(context.relative_path)) {
    return;
  }
  const auto source = context.source;
  const auto children = named_children(node);
  if (children.empty()) return;
  const auto callee = text(children.front(), source);
  const auto name = std::string(last_segment(callee));
  if (name != "subscribe" && name != "ProducerRecord") return;
  auto& index = file_index(node, context, Lang::Kotlin);
  if (!imports_any(index, {"org.apache.kafka"})) return;
  const auto args = arguments_of(node);
  if (args.empty()) return;
  if (name == "subscribe") {
    for (const auto& topic : name_list(args[0], source, index)) emit_topic(out, kUses, function_scope_id, topic, context);
  } else {
    emit_topic(out, kProvides, function_scope_id, name_value(args[0], source, index), context);
  }
}

// ---------------------------------------------------------------------------
// C#

void csharp_messaging_contracts(const TSNode& node, const ExtractionContext& context,
                                const std::string& function_scope_id, std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  if ((type != "invocation_expression" && type != "method_declaration") || is_test_source_path(context.relative_path) ||
      (type == "method_declaration" && function_scope_id.empty())) {
    return;
  }
  // A top-level statement (`Program.cs`) belongs to its file.
  const auto scope = function_scope_id.empty() ? make_id(context.relative_path) : function_scope_id;
  const auto source = context.source;
  if (type == "method_declaration") {
    bool overrides = false;
    for (const auto& child : named_children(node)) {
      if (type_of(child) == "modifier" && text(child, source) == "override") overrides = true;
    }
    const auto owner = parent(parent(node));
    if (!overrides || type_of(owner) != "class_declaration") return;
    for (const auto& child : named_children(owner)) {
      if (type_of(child) != "base_list") continue;
      for (const auto& base : named_children(child)) {
        if (type_of(base) != "qualified_name") continue;
        const auto service = std::string(last_segment(text(field(base, "qualifier"), source)));
        if (text(field(base, "name"), source) == service + "Base") {
          emit_grpc(out, kProvides, scope, service, text(field(node, "name"), source), context);
        }
      }
    }
    return;
  }
  const auto callee = field(node, "function");
  if (type_of(callee) != "member_access_expression") return;
  const auto receiver = field(callee, "expression");
  auto method = text(field(callee, "name"), source);
  auto& index = file_index(node, context, Lang::CSharp);
  const auto args = arguments_of(node);
  if (is_upper_initial(method)) {
    if (auto service = receiver_service(receiver, source, Lang::CSharp, index); !service.empty()) {
      if (auto stripped = strip_suffix(method, "Async"); !stripped.empty()) method = stripped;
      emit_grpc(out, kUses, scope, service, method, context);
      return;
    }
  }
  if (args.empty()) return;
  if (imports_any(index, {"Confluent.Kafka"})) {
    if (method == "Subscribe") {
      for (const auto& topic : name_list(args[0], source, index)) emit_topic(out, kUses, scope, topic, context);
    } else if (method == "Produce" || method == "ProduceAsync") {
      emit_topic(out, kProvides, scope, name_value(args[0], source, index), context);
    }
  }
  if (imports_any(index, {"RabbitMQ.Client"})) {
    const auto plain = positional(args, source);
    if (method == "BasicPublish" || method == "BasicPublishAsync") {
      auto exchange = keyword(args, "exchange", source);
      auto key = keyword(args, "routingKey", source);
      if (ts_node_is_null(exchange) && !plain.empty()) exchange = plain[0];
      if (ts_node_is_null(key) && plain.size() > 1) key = plain[1];
      const auto spelled = text(unwrap(exchange), source);
      const bool default_exchange = string_value(exchange, source) == std::optional<std::string>("") ||
                                    spelled == "string.Empty" || spelled == "String.Empty";
      if (!ts_node_is_null(exchange) && !ts_node_is_null(key) && default_exchange) {
        emit_topic(out, kProvides, scope, name_value(key, source, index), context);
      }
    } else if (method == "BasicConsume" || method == "BasicConsumeAsync") {
      auto queue = keyword(args, "queue", source);
      if (ts_node_is_null(queue) && !plain.empty()) queue = plain[0];
      if (!ts_node_is_null(queue)) emit_topic(out, kUses, scope, name_value(queue, source, index), context);
    }
  }
}

// ---------------------------------------------------------------------------
// C++ (servers only)

void cpp_messaging_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                             std::vector<RawRelation>& out) {
  if (type_of(node) != "function_definition" || function_scope_id.empty() ||
      is_test_source_path(context.relative_path)) {
    return;
  }
  const auto source = context.source;
  const auto owner = parent(parent(node));
  if (type_of(owner) != "class_specifier") return;
  std::string service;
  for (const auto& child : named_children(owner)) {
    if (type_of(child) != "base_class_clause") continue;
    for (const auto& base : named_children(child)) {
      const auto spelled = text(base, source);
      if (spelled.ends_with("::Service")) {
        const auto head = std::string_view(spelled).substr(0, spelled.size() - 9);
        const auto cut = head.rfind("::");
        service = std::string(cut == std::string_view::npos ? head : head.substr(cut + 2));
      }
    }
  }
  const auto declarator = field(node, "declarator");
  if (service.empty() || type_of(declarator) != "function_declarator") return;
  bool overrides = false;
  for (const auto& child : named_children(declarator)) {
    if (type_of(child) == "virtual_specifier" && text(child, source) == "override") overrides = true;
  }
  if (overrides) {
    emit_grpc(out, kProvides, function_scope_id, service, text(field(declarator, "declarator"), source), context);
  }
}

// ---------------------------------------------------------------------------
// Python

void python_messaging_contracts(const TSNode& node, const ExtractionContext& context,
                                const std::string& function_scope_id, std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  if ((type != "call" && type != "function_definition") || is_test_source_path(context.relative_path) ||
      (type == "function_definition" && function_scope_id.empty())) {
    return;
  }
  // A script's top-level call (`channel.basic_publish(...)` in `send.py`)
  // belongs to its file.
  const auto scope = function_scope_id.empty() ? make_id(context.relative_path) : function_scope_id;
  const auto source = context.source;
  auto& index = file_index(node, context, Lang::Python);
  if (type == "function_definition") {
    const auto owner = parent(parent(node));
    if (type_of(owner) != "class_definition") return;
    const auto service = index.servicers.find(text(field(owner, "name"), source));
    const auto params = named_children(field(node, "parameters"));
    const auto name = text(field(node, "name"), source);
    if (service == index.servicers.end() || params.size() != 3 || text(params[0], source) != "self" ||
        name.starts_with("_")) {
      return;
    }
    emit_grpc(out, kProvides, scope, service->second, name, context);
    return;
  }
  const auto callee = field(node, "function");
  const auto args = arguments_of(node);
  const auto plain = positional(args, source);
  const auto callee_name = std::string(last_segment(text(callee, source)));
  const bool kafka = imports_any(index, {"kafka", "aiokafka", "confluent_kafka"});
  if (kafka && (callee_name == "KafkaConsumer" || callee_name == "AIOKafkaConsumer")) {
    for (const auto& arg : plain) emit_topic(out, kUses, scope, name_value(arg, source, index), context);
    return;
  }
  if (type_of(callee) != "attribute") return;
  auto receiver = field(callee, "object");
  auto method = text(field(callee, "attribute"), source);
  if ((method == "future" || method == "with_call") && type_of(receiver) == "attribute") {
    method = text(field(receiver, "attribute"), source);
    receiver = field(receiver, "object");
  }
  if (!method.starts_with("_")) {
    if (auto service = receiver_service(receiver, source, Lang::Python, index); !service.empty()) {
      emit_grpc(out, kUses, scope, service, method, context);
      return;
    }
  }
  if (kafka && method == "subscribe") {
    auto topics = keyword(args, "topics", source);
    if (ts_node_is_null(topics) && !plain.empty()) topics = plain[0];
    if (!ts_node_is_null(topics)) {
      for (const auto& topic : name_list(topics, source, index)) emit_topic(out, kUses, scope, topic, context);
    }
  } else if (kafka && (method == "send" || method == "send_and_wait" || method == "produce") && !plain.empty() &&
             receiver_typed(receiver, source, index, {"KafkaProducer", "AIOKafkaProducer", "Producer"})) {
    emit_topic(out, kProvides, scope, name_value(plain[0], source, index), context);
  } else if (imports_any(index, {"pika"}) && method == "basic_publish") {
    auto exchange = keyword(args, "exchange", source);
    auto key = keyword(args, "routing_key", source);
    if (ts_node_is_null(exchange) && !plain.empty()) exchange = plain[0];
    if (ts_node_is_null(key) && plain.size() > 1) key = plain[1];
    if (!ts_node_is_null(exchange) && !ts_node_is_null(key) &&
        string_value(exchange, source) == std::optional<std::string>("")) {
      emit_topic(out, kProvides, scope, name_value(key, source, index), context);
    }
  } else if (imports_any(index, {"pika"}) && method == "basic_consume") {
    auto queue = keyword(args, "queue", source);
    if (ts_node_is_null(queue) && !plain.empty()) queue = plain[0];
    if (!ts_node_is_null(queue)) emit_topic(out, kUses, scope, name_value(queue, source, index), context);
  } else if (imports_any(index, {"boto3"}) && (method == "send_message" || method == "send_message_batch" ||
                                                method == "receive_message")) {
    const auto url = keyword(args, "QueueUrl", source);
    if (!ts_node_is_null(url)) {
      emit_topic(out, method == "receive_message" ? kUses : kProvides, scope, name_value(url, source, index),
                 context);
    }
  } else if (imports_any(index, {"boto3"}) && method == "publish" && !ts_node_is_null(keyword(args, "TopicArn", source))) {
    emit_topic(out, kProvides, scope, name_value(keyword(args, "TopicArn", source), source, index), context);
  } else if (imports_any(index, {"pubsub"}) && (method == "publish" || method == "create_subscription")) {
    auto topic = method == "publish" ? (plain.empty() ? TSNode{} : plain[0]) : keyword(args, "topic", source);
    topic = unwrap(topic);
    if (type_of(topic) == "call" && last_segment(text(field(topic, "function"), source)) == "topic_path") {
      const auto parts = positional(arguments_of(topic), source);
      if (parts.size() == 2) {
        emit_topic(out, method == "publish" ? kProvides : kUses, scope, name_value(parts[1], source, index),
                   context);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// JavaScript / TypeScript

void js_messaging_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                            const Fragment& fragment, std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  if ((type != "call_expression" && type != "new_expression") || is_test_source_path(context.relative_path)) {
    return;
  }
  const auto source = context.source;
  auto& index = file_index(node, context, Lang::Js);
  const auto scope = js_syntax::reading_scope_id(node, context, function_scope_id, fragment);
  const auto args = arguments_of(node);
  const auto object_member = [&](const TSNode& object, std::string_view key) -> TSNode {
    if (type_of(unwrap(object)) != "object") return {};
    for (const auto& pair : named_children(unwrap(object))) {
      if (type_of(pair) == "pair" && js_syntax::strip_string_quotes(text(field(pair, "key"), source)) == key) {
        return field(pair, "value");
      }
    }
    return {};
  };
  if (type == "new_expression") {
    const auto constructor = text(field(node, "constructor"), source);
    if (args.empty()) return;
    if (imports_any(index, {"@aws-sdk/client-sqs"}) &&
        (constructor == "SendMessageCommand" || constructor == "SendMessageBatchCommand" ||
         constructor == "ReceiveMessageCommand")) {
      const auto url = object_member(args[0], "QueueUrl");
      if (!ts_node_is_null(url)) {
        emit_topic(out, constructor == "ReceiveMessageCommand" ? kUses : kProvides, scope,
                   name_value(url, source, index), context);
      }
    } else if (imports_any(index, {"@aws-sdk/client-sns"}) && constructor == "PublishCommand") {
      const auto arn = object_member(args[0], "TopicArn");
      if (!ts_node_is_null(arn)) emit_topic(out, kProvides, scope, name_value(arn, source, index), context);
    }
    return;
  }
  const auto callee = field(node, "function");
  if (type_of(callee) != "member_expression") return;
  const auto receiver = field(callee, "object");
  const auto method = text(field(callee, "property"), source);
  static constexpr std::array<std::string_view, 8> kStubMachinery = {
      "close", "getChannel", "waitForReady", "makeUnaryRequest", "makeClientStreamRequest",
      "makeServerStreamRequest", "makeBidiStreamRequest", "addService"};
  if (index.grpc_import && method == "addService" && args.size() == 2 && type_of(unwrap(args[1])) == "object") {
    const auto definition = unwrap(args[0]);
    std::string service;
    if (type_of(definition) == "member_expression" && text(field(definition, "property"), source) == "service") {
      service = bound_name(field(definition, "object"), source);
    } else if (type_of(definition) == "identifier") {
      service = strip_suffix(text(definition, source), "Service");
    }
    if (service.empty()) return;
    for (const auto& member : named_children(unwrap(args[1]))) {
      const auto member_type = type_of(member);
      std::string key;
      if (member_type == "shorthand_property_identifier") key = text(member, source);
      if (member_type == "pair" || member_type == "method_definition") {
        key = js_syntax::strip_string_quotes(text(field(member, member_type == "pair" ? "key" : "name"), source));
      }
      if (!key.empty()) emit_grpc(out, kProvides, scope, service, key, context);
    }
    return;
  }
  if (index.grpc_import && std::ranges::find(kStubMachinery, method) == kStubMachinery.end()) {
    if (auto service = receiver_service(receiver, source, Lang::Js, index); !service.empty()) {
      emit_grpc(out, kUses, scope, service, method, context);
      return;
    }
  }
  if (args.empty()) return;
  if (imports_any(index, {"kafkajs"}) && (method == "send" || method == "subscribe")) {
    const auto topic = object_member(args[0], "topic");
    if (!ts_node_is_null(topic)) {
      emit_topic(out, method == "send" ? kProvides : kUses, scope, name_value(topic, source, index), context);
    }
    const auto topics = object_member(args[0], "topics");
    if (method == "subscribe" && !ts_node_is_null(topics)) {
      for (const auto& name : name_list(topics, source, index)) emit_topic(out, kUses, scope, name, context);
    }
  } else if (imports_any(index, {"amqplib", "amqp-connection-manager"})) {
    if (method == "sendToQueue") {
      emit_topic(out, kProvides, scope, name_value(args[0], source, index), context);
    } else if (method == "consume") {
      emit_topic(out, kUses, scope, name_value(args[0], source, index), context);
    } else if (method == "publish" && args.size() >= 2 && string_value(args[0], source) == std::optional<std::string>("")) {
      emit_topic(out, kProvides, scope, name_value(args[1], source, index), context);
    }
  } else if (imports_any(index, {"@google-cloud/pubsub"}) && type_of(unwrap(receiver)) == "call_expression") {
    const auto inner = unwrap(receiver);
    const auto inner_callee = field(inner, "function");
    const auto inner_args = arguments_of(inner);
    if (type_of(inner_callee) != "member_expression" || text(field(inner_callee, "property"), source) != "topic" ||
        inner_args.empty()) {
      return;
    }
    if (method == "publish" || method == "publishMessage" || method == "publishJSON") {
      emit_topic(out, kProvides, scope, name_value(inner_args[0], source, index), context);
    } else if (method == "subscription" || method == "createSubscription") {
      emit_topic(out, kUses, scope, name_value(inner_args[0], source, index), context);
    }
  }
}

}  // namespace cgraph
