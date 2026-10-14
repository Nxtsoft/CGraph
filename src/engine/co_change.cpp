#include "cgraph/co_change.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

using Ids = std::unordered_set<std::string_view>;

struct Range {
  std::uint32_t first = 0;
  std::uint32_t last = 0;
};

// One candidate file in one ranked list: why it is there, and the keys that
// order the list (smaller first).
struct Candidate {
  std::string why;
  int depth = 0;
  std::size_t edges = 0;
  int why_rank = 99;  // verb_rank of the relation `why` names; a stronger one replaces it
};

[[nodiscard]] std::string_view directory_of(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

[[nodiscard]] std::string_view basename_of(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The file name without its last extension: `a.test.ts` -> `a.test`.
[[nodiscard]] std::string_view stem_of(std::string_view path) {
  const auto base = basename_of(path);
  const auto dot = base.rfind('.');
  return dot == std::string_view::npos || dot == 0 ? base : base.substr(0, dot);
}

// The stem with one test marker removed (`test_x`, `x_test`, `x_tests`,
// `x.test`, `x.spec`, `x_spec`, `XTest`, `XTests`), or the stem itself.
[[nodiscard]] std::string_view test_core(std::string_view stem) {
  if (stem.starts_with("test_") && stem.size() > 5) return stem.substr(5);
  static constexpr std::array<std::string_view, 7> kSuffixes{"_tests", "_test", ".test", ".spec", "_spec", "Tests", "Test"};
  for (const auto suffix : kSuffixes) {
    if (stem.ends_with(suffix) && stem.size() > suffix.size()) return stem.substr(0, stem.size() - suffix.size());
  }
  return stem;
}

[[nodiscard]] std::string ticked(std::string_view label) {
  return "`" + std::string(label) + "`";
}

// How a dependent relates to what it depends on, as a verb.
[[nodiscard]] std::string_view verb_for(std::string_view relation) {
  if (relation == "CALLS") return "calls";
  if (relation == "imports" || relation == "imports_from") return "imports";
  if (relation == "implements") return "implements";
  if (relation == "inherits" || relation == "extends") return "extends";
  return "uses";
}

// Which relation names a dependent best: a call says more than an import.
[[nodiscard]] int verb_rank(std::string_view relation) {
  const auto verb = verb_for(relation);
  if (verb == "calls") return 0;
  if (verb == "implements" || verb == "extends") return 1;
  if (verb == "uses") return 2;
  return 3;
}

[[nodiscard]] std::vector<Range> ranges_of(const nlohmann::json& params) {
  std::vector<Range> out;
  const auto lines = params.find("lines");
  if (lines == params.end() || !lines->is_array()) return out;
  for (const auto& pair : *lines) {
    if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number_integer() || !pair[1].is_number_integer()) continue;
    const auto first = pair[0].get<long long>();
    const auto last = pair[1].get<long long>();
    if (first < 1 || last < first) continue;
    out.push_back({static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(last)});
  }
  return out;
}

// The innermost symbols of the file overlapping any range: a method edited
// inside a class is the method, not the class around it.
[[nodiscard]] std::vector<const Node*> edited_symbols(const std::vector<const Node*>& symbols,
                                                      const std::vector<Range>& ranges) {
  std::vector<const Node*> hits;
  for (const auto* node : symbols) {
    const auto& loc = *node->source_location;
    for (const auto& range : ranges) {
      if (loc.start_line <= range.last && loc.end_line >= range.first) {
        hits.push_back(node);
        break;
      }
    }
  }
  std::vector<const Node*> inner;
  for (const auto* node : hits) {
    const auto& outer = *node->source_location;
    const bool holds_another = std::ranges::any_of(hits, [&](const Node* other) {
      if (other == node) return false;
      const auto& in = *other->source_location;
      return outer.start_line <= in.start_line && in.end_line <= outer.end_line &&
             (outer.start_line != in.start_line || outer.end_line != in.end_line);
    });
    if (!holds_another) inner.push_back(node);
  }
  return inner;
}

// Orders one list's candidates: shallower first, then more edges, then path.
[[nodiscard]] std::vector<std::pair<std::string, Candidate>> ranked(std::map<std::string, Candidate> candidates) {
  std::vector<std::pair<std::string, Candidate>> out(std::make_move_iterator(candidates.begin()),
                                                     std::make_move_iterator(candidates.end()));
  std::ranges::stable_sort(out, [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.second.depth, rhs.second.edges) < std::tie(rhs.second.depth, lhs.second.edges);
  });
  return out;
}

}  // namespace

nlohmann::json co_change(const GraphSnapshot& graph, const nlohmann::json& params) {
  const auto id = params.value("id", std::string{});
  const auto limit = params.value("limit", kCoChangeDefaultLimit);
  nlohmann::json result{{"id", id}, {"found", false}, {"edited", nlohmann::json::array()},
                        {"files", nlohmann::json::array()}, {"candidates", 0}};

  const Node* file = nullptr;
  for (const auto& node : graph.nodes) {
    if (node.id == id && node.kind == "file" && !node.source_file.empty()) {
      file = &node;
      break;
    }
  }
  if (file == nullptr) return result;
  result["found"] = true;
  const std::string_view path = file->source_file;

  // The file's own nodes, and the symbols among them that carry a span.
  Ids own;
  std::vector<const Node*> symbols;
  std::unordered_map<std::string_view, const Node*> by_id;
  for (const auto& node : graph.nodes) {
    if (node.source_file == path) {
      own.insert(node.id);
      if (&node != file && node.source_location && node.source_location->start_line > 0) symbols.push_back(&node);
    }
  }
  const auto ranges = ranges_of(params);
  // A whole-file edit (no ranges) changes every symbol and the file itself.
  std::vector<const Node*> edited = ranges.empty() ? symbols : edited_symbols(symbols, ranges);
  if (ranges.empty()) edited.push_back(file);
  Ids edited_ids;
  for (const auto* node : edited) {
    edited_ids.insert(node->id);
    if (node != file) result["edited"].push_back(node->label);
  }

  // One pass: direct dependents of the edited symbols, the same-file code that
  // reaches them (a route into the file for a second hop), and every edge that
  // joins the file to another.
  struct Hop {
    std::string_view other;     // node in another file
    std::string_view here;      // node in this file (or the intermediate, for depth 2)
    std::string_view relation;
  };
  std::vector<Hop> direct;
  std::vector<Hop> linked_in;
  std::vector<Hop> linked_out;
  Ids intermediate;  // depth-1 dependents, in any file, to walk one more hop from
  for (const auto& edge : graph.edges) {
    const bool source_own = own.contains(edge.source);
    const bool target_own = own.contains(edge.target);
    if (edited_ids.contains(edge.target) && edge.source != edge.target && !edited_ids.contains(edge.source)) {
      intermediate.insert(edge.source);
      if (!source_own) direct.push_back({edge.source, edge.target, edge.relation});
    }
    if (target_own && !source_own) linked_in.push_back({edge.source, edge.target, edge.relation});
    if (source_own && !target_own) linked_out.push_back({edge.target, edge.source, edge.relation});
  }
  std::vector<Hop> second;
  if (!intermediate.empty()) {
    for (const auto& edge : graph.edges) {
      if (intermediate.contains(edge.target) && !own.contains(edge.source) && !intermediate.contains(edge.source)) {
        second.push_back({edge.source, edge.target, edge.relation});
      }
    }
  }

  // Resolve every node the hops name, in one pass.
  Ids wanted;
  for (const auto* hops : {&direct, &linked_in, &linked_out, &second}) {
    for (const auto& hop : *hops) {
      wanted.insert(hop.other);
      wanted.insert(hop.here);
    }
  }
  for (const auto& node : graph.nodes) {
    if (wanted.contains(node.id)) by_id.emplace(node.id, &node);
  }
  const auto code_file_of = [&](std::string_view node_id) -> const Node* {
    const auto it = by_id.find(node_id);
    if (it == by_id.end() || it->second->source_file.empty() || it->second->source_file == path ||
        is_memory_node_id(it->second->id) || is_enrichment_node_id(it->second->id)) {
      return nullptr;
    }
    return it->second;
  };
  const auto label_of = [&](std::string_view node_id) -> std::string_view {
    if (const auto it = by_id.find(node_id); it != by_id.end()) return it->second->label;
    return node_id;
  };

  std::map<std::string, Candidate> callers;
  for (const auto& hop : direct) {
    const auto* other = code_file_of(hop.other);
    if (other == nullptr) continue;
    auto& candidate = callers[other->source_file];
    candidate.depth = 1;
    ++candidate.edges;
    if (verb_rank(hop.relation) < candidate.why_rank) {
      candidate.why_rank = verb_rank(hop.relation);
      candidate.why = std::string(verb_for(hop.relation)) + " " +
                      (hop.here == file->id ? std::string("this file") : ticked(label_of(hop.here)));
    }
  }
  for (const auto& hop : second) {
    const auto* other = code_file_of(hop.other);
    if (other == nullptr) continue;
    auto [it, fresh] = callers.try_emplace(other->source_file);
    if (fresh) it->second.depth = 2;
    if (it->second.depth != 2) continue;
    ++it->second.edges;
    if (verb_rank(hop.relation) < it->second.why_rank) {
      it->second.why_rank = verb_rank(hop.relation);
      it->second.why = std::string(verb_for(hop.relation)) + " " + ticked(label_of(hop.here)) +
                       ", which depends on the edited code";
    }
  }

  // The source/test counterpart by name.
  std::map<std::string, Candidate> tests;
  const auto stem = stem_of(path);
  const auto core = test_core(stem);
  const auto dir = directory_of(path);
  for (const auto& node : graph.nodes) {
    if (node.kind != "file" || node.source_file.empty() || node.source_file == path) continue;
    const auto other_stem = stem_of(node.source_file);
    if (other_stem == stem || test_core(other_stem) != core) continue;
    auto& candidate = tests[node.source_file];
    candidate.depth = directory_of(node.source_file) == dir ? 0 : 1;
    candidate.edges = 0;
    candidate.why = test_core(other_stem) != other_stem ? "test for this file" : "the code this test covers";
  }

  std::map<std::string, Candidate> linked;
  for (const auto& hop : linked_in) {
    const auto* other = code_file_of(hop.other);
    if (other == nullptr) continue;
    auto& candidate = linked[other->source_file];
    ++candidate.edges;
    if (verb_rank(hop.relation) < candidate.why_rank) {
      candidate.why_rank = verb_rank(hop.relation);
      candidate.why = std::string(verb_for(hop.relation)) + " " +
                      (hop.here == file->id ? std::string("this file") : ticked(label_of(hop.here)));
    }
  }
  for (const auto& hop : linked_out) {
    const auto* other = code_file_of(hop.other);
    if (other == nullptr) continue;
    auto& candidate = linked[other->source_file];
    ++candidate.edges;
    if (verb_rank(hop.relation) < candidate.why_rank) {
      candidate.why_rank = verb_rank(hop.relation);
      candidate.why = other->kind == "file"
                          ? "this file " + std::string(verb_for(hop.relation)) + " it"
                          : "defines " + ticked(other->label) + ", which this file " + std::string(verb_for(hop.relation));
    }
  }

  const std::array<std::pair<const char*, std::vector<std::pair<std::string, Candidate>>>, 3> lists{{
      {"callers", ranked(std::move(callers))},
      {"tests", ranked(std::move(tests))},
      {"linked", ranked(std::move(linked))},
  }};
  std::unordered_set<std::string> seen;
  std::size_t candidates = 0;
  for (const auto& [_, list] : lists) {
    for (const auto& [candidate_path, _candidate] : list) candidates += seen.insert(candidate_path).second ? 1 : 0;
  }
  result["candidates"] = candidates;
  seen.clear();
  auto& files = result["files"];
  std::array<std::size_t, 3> next{};
  for (bool progressed = true; progressed && files.size() < limit;) {
    progressed = false;
    for (std::size_t li = 0; li < lists.size() && files.size() < limit; ++li) {
      const auto& list = lists[li].second;
      while (next[li] < list.size() && seen.contains(list[next[li]].first)) ++next[li];
      if (next[li] == list.size()) continue;
      const auto& [candidate_path, candidate] = list[next[li]++];
      seen.insert(candidate_path);
      files.push_back({{"source_file", candidate_path}, {"why", candidate.why}, {"signal", lists[li].first}});
      progressed = true;
    }
  }
  return result;
}

}  // namespace cgraph
