#include "cgraph/daemon_ops.hpp"

#include "cgraph/engine.hpp"

#include "cgraph/file_cache.hpp"
#include "cgraph/fragment_json.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/semantic_connectivity.hpp"
#include "cgraph/snapshot_source_reader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cgraph {
namespace {

// Bounds on the source slice returned with a node so a single explain stays
// token-cheap even for a large function body.
constexpr std::size_t kMaxSnippetLines = 40;
constexpr std::size_t kMaxSnippetChars = 2000;

// Default caps so a broad query or a wide blast radius stays bounded. 0 means
// "no limit" when a caller passes it explicitly.
constexpr std::size_t kDefaultQueryLimit = 50;
constexpr std::size_t kDefaultImpactLimit = 200;
constexpr int kDefaultImpactDepth = 3;

// God nodes can touch hundreds of edges; explain caps the neighbor list (most
// central first) so one call stays token-cheap.
constexpr std::size_t kDefaultExplainNeighborLimit = 100;

// How many did-you-mean candidates a missed lookup returns.
constexpr std::size_t kMaxSuggestions = 5;

// Context packing: a token budget to fill and how far around the focal symbol to
// gather candidates.
constexpr std::size_t kDefaultContextBudget = 6000;  // tokens
constexpr int kDefaultContextDepth = 2;
// Knapsack packing gathers a wider ego graph (validated sweet spot; k=4 is past
// the crossover). See research/2510.00446.
constexpr int kKnapsackContextDepth = 3;
// Hard ceiling on the knapsack DP capacity so a pathological `budget` param can
// never blow up the O(n*capacity) table.
constexpr std::size_t kMaxKnapsackCapacity = 50000;

// Rough token estimate: ~4 characters per token. Good enough to pack a context
// bundle under a budget without pulling in a real tokenizer.
[[nodiscard]] std::size_t estimate_tokens(const std::string& text) {
  return (text.size() + 3) / 4;
}

[[nodiscard]] std::size_t estimate_tokens_for_length(std::size_t byte_length) {
  return (byte_length + 3) / 4;
}

[[nodiscard]] nlohmann::json error_response(std::string message) {
  return nlohmann::json{{"ok", false}, {"error", std::move(message)}};
}

[[nodiscard]] nlohmann::json ok_response(nlohmann::json result = nlohmann::json::object()) {
  return nlohmann::json{{"ok", true}, {"result", std::move(result)}};
}

[[nodiscard]] bool is_root_pinnable_read(DaemonOp op) {
  switch (op) {
    case DaemonOp::Query:
    case DaemonOp::Path:
    case DaemonOp::Explain:
    case DaemonOp::Impact:
    case DaemonOp::Context:
    case DaemonOp::Recall:
      return true;
    case DaemonOp::Update:
    case DaemonOp::Status:
    case DaemonOp::Shutdown:
    case DaemonOp::Remember:
    case DaemonOp::Count:
      return false;
  }
  return false;
}

[[nodiscard]] bool root_pin_matches_snapshot(
    DaemonOp op,
    const nlohmann::json& params,
    const GraphSnapshot& graph) {
  if (!is_root_pinnable_read(op) || !params.contains("expected_content_root")) {
    return true;
  }
  const auto& expected = params.at("expected_content_root");
  return expected.is_string() && is_valid_content_root(graph.content_root) &&
         expected.get_ref<const std::string&>() == graph.content_root.sha256;
}

// Degree centrality (normalized 0..1) computed by analyze_graph and stored on
// the node; 0 when absent (e.g. a snapshot that has not been analyzed).
[[nodiscard]] double node_centrality(const Node& node) {
  const auto it = node.properties.find("degree_centrality");
  if (it == node.properties.end()) {
    return 0.0;
  }
  const char* begin = it->second.c_str();
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  return end == begin ? 0.0 : value;
}

// Compact, file-read-free descriptor: enough for an agent to open the symbol at
// the right place (source_file + 1-based line) and judge its importance
// (centrality, god_node) without a follow-up lookup.
[[nodiscard]] nlohmann::json node_brief(const Node& node) {
  nlohmann::json brief{{"id", node.id}, {"label", node.label}, {"kind", node.kind}};
  if (!node.source_file.empty()) {
    brief["source_file"] = node.source_file;
  }
  if (node.source_location && node.source_location->start_line > 0) {
    brief["line"] = node.source_location->start_line;
  }
  if (node.properties.contains("degree_centrality")) {
    brief["centrality"] = node_centrality(node);
  }
  if (node.properties.contains("god_node")) {
    brief["god_node"] = true;
  }
  return brief;
}

// Read the node's source lines [start_line, end_line] (1-based, inclusive)
// through the request-local exact-buffer cache.
[[nodiscard]] SnapshotSourceSnippet read_source_snippet(
    SnapshotSourceReader& source_reader,
    const Node& node) {
  return source_reader.read_snippet(node, kMaxSnippetLines, kMaxSnippetChars);
}

void add_source_location(nlohmann::json& brief, const Node& node) {
  if (node.source_location && node.source_location->start_line > 0) {
    brief["location"] = {
        {"start_line", node.source_location->start_line},
        {"start_column", node.source_location->start_column},
        {"end_line", node.source_location->end_line},
        {"end_column", node.source_location->end_column}};
  }
}

// Enrich a brief with the focal node's full location block and an on-disk source
// snippet (bounded by read_source_snippet). Used wherever a single node is the
// subject and the code itself is worth returning.
[[nodiscard]] nlohmann::json with_source(
    nlohmann::json brief,
    const Node& node,
    SnapshotSourceReader& source_reader) {
  add_source_location(brief, node);
  if (const auto snippet = read_source_snippet(source_reader, node); !snippet.text.empty()) {
    brief["snippet"] = snippet.text;
    brief["source_sha256"] = snippet.source_sha256;
    if (snippet.truncated) {
      brief["snippet_truncated"] = true;
    }
  }
  return brief;
}

// Lowercased alphanumeric terms, breaking camelCase and snake_case and keeping
// terms of length >= 3. Mirrors the offline harness's relevance value
// (research/select.py::_terms) so the knapsack scorer matches what step A validated.
[[nodiscard]] std::unordered_set<std::string> lexical_terms(std::string_view text) {
  std::unordered_set<std::string> terms;
  std::string word;
  const auto flush = [&]() {
    if (word.empty()) {
      return;
    }
    std::string sub;
    const auto push_sub = [&]() {
      if (sub.size() >= 3) {
        terms.insert(sub);
      }
      sub.clear();
    };
    for (std::size_t i = 0; i < word.size(); ++i) {
      const auto ch = static_cast<unsigned char>(word[i]);
      const bool upper = std::isupper(ch) != 0;
      const bool prev_upper = i > 0 && std::isupper(static_cast<unsigned char>(word[i - 1])) != 0;
      const bool next_lower =
          i + 1 < word.size() && std::islower(static_cast<unsigned char>(word[i + 1])) != 0;
      if (i > 0 && upper && (!prev_upper || next_lower)) {
        push_sub();
      }
      sub.push_back(static_cast<char>(std::tolower(ch)));
    }
    push_sub();
    word.clear();
  };
  for (const char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      word.push_back(c);
    } else {
      flush();
    }
  }
  flush();
  return terms;
}

// Fraction of query terms also present in the node label (0..1).
[[nodiscard]] double query_term_overlap(const std::unordered_set<std::string>& query_terms,
                                        std::string_view label) {
  if (query_terms.empty()) {
    return 0.0;
  }
  const auto label_terms = lexical_terms(label);
  std::size_t hit = 0;
  for (const auto& term : query_terms) {
    if (label_terms.contains(term)) {
      ++hit;
    }
  }
  return static_cast<double>(hit) / static_cast<double>(query_terms.size());
}

// Number of lexical focal seeds whose neighborhoods a context gather unions when
// resolving a free-text query. Seeding from several hedges query ambiguity, but
// pool size trades against packing dilution: with idf-weighted ranking, three
// seeds measure best at the shipped default budget and above (k=3 beats k=5 by
// ~3 recall points and k=10 loses outright; the tighter set is only safe BECAUSE
// the ranking is good -- k=3 under the old plain-overlap ranking was a wash.
// research/focal-ranking, measured through the committed e2e gate).
constexpr std::size_t kFocalSeedCount = 3;
// Same-file expansion targets the measured focal-file recall tail without
// letting generated or unusually dense files flood the candidate set. Five
// preserves the committed-fixture recall gain while bounding candidate growth.
constexpr std::size_t kSameFileCandidateCap = 5;

// Nodes that share at least one lexical term with the query, ranked by overlap
// (then centrality, then label). The fallback when literal substring matching
// (`matching_nodes`) finds nothing, so a natural-language query — which is almost
// never a substring of a symbol label — still resolves a focal node instead of
// returning empty. A query that shares no term with any label yields no match, so
// a genuinely off-topic request stays an honest zero hit.
// One-shot suffix-strip stemming for seed matching, so a query inflection
// ("packing", "resolves", "imports") matches the code identifier it names
// ("pack", "resolve", "import"). Deliberately light: a single strip from a
// fixed suffix list, and the stem must keep >= 4 chars so short roots stay
// exact -- doubled-consonant gerunds ("running" -> "runn") are accepted misses
// rather than reasons for a heavier stemmer. Used ONLY by lexical_matches
// below; lexical_terms itself, the adaptive gather gate, and the knapsack
// value keep exact-match semantics. Measured on the committed fixture:
// +1.4 to +3.5 end-to-end recall points at every budget, and the share of
// rows whose grade-2 anchor is lexically reachable at all rises 0.733 -> 0.747
// (research/beyond-lexical).
[[nodiscard]] std::string stem_term(const std::string& term) {
  static constexpr std::string_view kSuffixes[] = {"ing", "tion", "sion", "ers", "ies",
                                                   "ed",  "es",   "al",   "er",  "s"};
  for (const auto suffix : kSuffixes) {
    if (term.size() >= suffix.size() + 4 && term.ends_with(suffix)) {
      return term.substr(0, term.size() - suffix.size());
    }
  }
  return term;
}

[[nodiscard]] std::unordered_set<std::string> stem_terms(const std::unordered_set<std::string>& terms) {
  std::unordered_set<std::string> out;
  out.reserve(terms.size());
  for (const auto& term : terms) {
    out.insert(stem_term(term));
  }
  return out;
}

[[nodiscard]] std::vector<const Node*> lexical_matches(
    const GraphSnapshot& graph, const std::unordered_set<std::string>& raw_query_terms) {
  std::vector<std::pair<double, const Node*>> scored;
  if (raw_query_terms.empty()) {
    return {};
  }
  const auto query_terms = stem_terms(raw_query_terms);
  // Rank by idf-weighted overlap: each query term is worth log(1 + N/(1+df)),
  // so a rare identifier outranks a ubiquitous word instead of every term
  // counting equally. Plain query-term fraction let common words dominate the
  // seed set; idf weighting moved end-to-end grade-2 recall at the default
  // budget from 0.280 to 0.387 on the committed fixture (research/focal-ranking,
  // with the 3-seed gather above). Label term sets are computed once per node in
  // the same pass that builds the document frequencies -- one O(nodes) scan, the
  // same shape as index_nodes and matching_nodes on this read path.
  std::vector<std::pair<const Node*, std::unordered_set<std::string>>> node_terms;
  node_terms.reserve(graph.nodes.size());
  std::unordered_map<std::string, std::size_t> df;
  for (const auto& node : graph.nodes) {
    if (is_memory_node_id(node.id)) {
      continue;  // session-memory checkpoints are not code matches
    }
    auto terms = stem_terms(lexical_terms(node.label));
    for (const auto& term : terms) {
      ++df[term];
    }
    node_terms.emplace_back(&node, std::move(terms));
  }
  const double node_count = static_cast<double>(node_terms.size());
  const auto idf = [&](const std::string& term) {
    const auto found = df.find(term);
    const double freq = found == df.end() ? 0.0 : static_cast<double>(found->second);
    return std::log(1.0 + node_count / (1.0 + freq));
  };
  // A term absent from every label gets the maximum idf but can match no node,
  // so it deflates all scores equally and never reorders them.
  double denom = 0.0;
  for (const auto& term : query_terms) {
    denom += idf(term);
  }
  for (const auto& [node, terms] : node_terms) {
    double weighted = 0.0;
    for (const auto& term : query_terms) {
      if (terms.contains(term)) {
        weighted += idf(term);
      }
    }
    if (weighted > 0.0) {
      scored.emplace_back(weighted / denom, node);
    }
  }
  std::ranges::sort(scored, [](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) {
      return lhs.first > rhs.first;  // higher overlap first
    }
    const auto lc = node_centrality(*lhs.second);
    const auto rc = node_centrality(*rhs.second);
    if (lc != rc) {
      return lc > rc;  // more central first
    }
    return lhs.second->label < rhs.second->label;  // deterministic tiebreak
  });
  std::vector<const Node*> out;
  out.reserve(scored.size());
  for (const auto& [overlap, node] : scored) {
    out.push_back(node);
  }
  return out;
}

// Metadata-only estimate of the node's capped source slice. Selection must finish
// before SnapshotSourceReader touches any path: a candidate that packing omits or
// degrades to brief-only must not be read or snapshot-verified.
// Source locations provide exact line spans and a useful final-column bound; forty
// characters per preceding line preserves the established char/4 budget model
// without consulting mutable working-tree bytes.
[[nodiscard]] std::size_t slice_token_cost(const Node& node) {
  if (node.source_file.empty() || !node.source_location ||
      node.source_location->start_line == 0) {
    return std::max<std::size_t>(1, estimate_tokens(node.label));
  }

  constexpr std::size_t kEstimatedSourceLineChars = 40;
  const auto& location = *node.source_location;
  const auto end_line = std::max(location.start_line, location.end_line);
  const auto raw_line_count =
      static_cast<std::uint64_t>(end_line) - location.start_line + 1;
  const auto line_count = static_cast<std::size_t>(
      std::min<std::uint64_t>(raw_line_count, kMaxSnippetLines));
  const auto final_line_chars = raw_line_count <= kMaxSnippetLines
                                    ? static_cast<std::size_t>(location.end_column)
                                    : kEstimatedSourceLineChars;
  const auto estimated_chars = std::min(
      kMaxSnippetChars,
      std::max(node.label.size(),
               (line_count - 1) * kEstimatedSourceLineChars + final_line_chars));
  return std::max<std::size_t>(1, (estimated_chars + 3) / 4);
}

// Project the serialized cost of a source-bearing entry without opening its
// source path. A fixed/greedy candidate is materialized only after this projected
// full entry fits; otherwise the exact brief cost decides brief-only vs omitted.
[[nodiscard]] std::size_t projected_source_entry_token_cost(
    nlohmann::json entry,
    const Node& node) {
  add_source_location(entry, node);
  if (!node.source_file.empty() && node.source_location &&
      node.source_location->start_line > 0) {
    constexpr std::size_t kSha256HexChars = 64;
    const auto estimated_snippet_chars =
        std::min(kMaxSnippetChars, slice_token_cost(node) * 4);
    entry["snippet"] = std::string(estimated_snippet_chars, 'x');
    entry["source_sha256"] = std::string(kSha256HexChars, '0');
    const auto& location = *node.source_location;
    const auto end_line = std::max(location.start_line, location.end_line);
    if (static_cast<std::uint64_t>(end_line) - location.start_line + 1 >
        kMaxSnippetLines) {
      entry["snippet_truncated"] = true;
    }
  }
  return estimate_tokens(entry.dump());
}

[[nodiscard]] std::size_t emitted_entry_tokens(
    const nlohmann::json& focus,
    const nlohmann::json& included) {
  // Measure the serialized ARRAY rather than summing per-entry costs: summing
  // misses the array's own framing (brackets, separators) and was observed to
  // overshoot a 3000 budget by a token. This is the number reported as
  // tokens_used and tested against the budget in both packing modes.
  return estimate_tokens(focus.dump()) + estimate_tokens(included.dump());
}

// A returned row without a snippet is marked so a caller can tell a failed
// read from a kind that has no source extent at all (documents, concepts,
// media). Only the former is a defect signal; the latter is a followable
// pointer by design. The extent test mirrors read_source_snippet's guard.
void annotate_snippet_absence(nlohmann::json& entry, const Node& node) {
  if (entry.contains("snippet") || entry.value("snippet_omitted", false) ||
      entry.value("snippet_unavailable", false)) {
    return;
  }
  const bool has_extent = !node.source_file.empty() && node.source_location &&
                          node.source_location->start_line != 0;
  entry[has_extent ? "snippet_omitted" : "snippet_unavailable"] = true;
}

[[nodiscard]] std::unordered_map<std::string, const Node*> index_nodes(const GraphSnapshot& graph) {
  std::unordered_map<std::string, const Node*> by_id;
  by_id.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    by_id.emplace(node.id, &node);
  }
  return by_id;
}

[[nodiscard]] char ascii_lower(char value) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

[[nodiscard]] std::string ascii_lower(std::string_view text) {
  std::string lowered(text);
  std::ranges::transform(lowered, lowered.begin(), [](char value) { return ascii_lower(value); });
  return lowered;
}

// Case-insensitive substring search; the needle must already be lowercase.
// Symbol queries are typed by an agent that rarely knows the exact casing.
[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view lower_needle) {
  if (lower_needle.empty()) {
    return true;
  }
  const auto it = std::search(
      haystack.begin(), haystack.end(), lower_needle.begin(), lower_needle.end(),
      [](char hay, char needle) { return ascii_lower(hay) == needle; });
  return it != haystack.end();
}

[[nodiscard]] std::vector<const Node*> matching_nodes(const GraphSnapshot& graph, const std::string& needle) {
  const auto lower_needle = ascii_lower(needle);
  std::vector<const Node*> matches;
  for (const auto& node : graph.nodes) {
    if (is_memory_node_id(node.id)) {
      continue;  // session-memory checkpoints are not code matches; recall surfaces them
    }
    if (contains_ci(node.id, lower_needle) || contains_ci(node.label, lower_needle)) {
      matches.push_back(&node);
    }
  }
  return matches;
}

// Levenshtein distance, abandoned (returning cap + 1) as soon as it must exceed
// `cap` — suggestion scoring only cares about near misses.
[[nodiscard]] std::size_t bounded_edit_distance(std::string_view a, std::string_view b, std::size_t cap) {
  if (a.size() > b.size()) {
    std::swap(a, b);
  }
  if (b.size() - a.size() > cap) {
    return cap + 1;
  }
  std::vector<std::size_t> row(a.size() + 1);
  for (std::size_t i = 0; i <= a.size(); ++i) {
    row[i] = i;
  }
  for (std::size_t j = 1; j <= b.size(); ++j) {
    std::size_t diagonal = row[0];
    row[0] = j;
    std::size_t row_min = row[0];
    for (std::size_t i = 1; i <= a.size(); ++i) {
      const std::size_t substitution = diagonal + (a[i - 1] == b[j - 1] ? 0 : 1);
      diagonal = row[i];
      row[i] = std::min({row[i] + 1, row[i - 1] + 1, substitution});
      row_min = std::min(row_min, row[i]);
    }
    if (row_min > cap) {
      return cap + 1;
    }
  }
  return row.back();
}

// The leading symbol token of a label: everything before the first '(' or
// whitespace. Labels often carry full signatures ("merge_fragments(GraphSnapshot&
// graph, ...)"); agents pass bare names ("merge_fragments").
[[nodiscard]] std::string_view label_symbol(const Node& node) {
  const std::string_view label{node.label};
  const auto cut = label.find_first_of("( \t\n");
  return cut == std::string_view::npos ? label : label.substr(0, cut);
}

// Closest labels to a missed lookup, so an agent can self-correct (wrong
// casing, typo, label-vs-id confusion) instead of concluding the symbol does
// not exist and falling back to grep.
[[nodiscard]] nlohmann::json suggest_similar(const GraphSnapshot& graph, const std::string& needle) {
  auto suggestions = nlohmann::json::array();
  if (needle.empty()) {
    return suggestions;
  }
  const auto lower_needle = ascii_lower(needle);
  const auto cap = std::max<std::size_t>(2, lower_needle.size() / 3);

  std::vector<std::pair<std::size_t, const Node*>> scored;
  for (const auto& node : graph.nodes) {
    auto distance = bounded_edit_distance(lower_needle, ascii_lower(label_symbol(node)), cap);
    // The trailing segment of the id (after the last separator) is usually the
    // bare symbol name; match against it too so path-qualified ids still rank.
    if (distance > cap) {
      const auto tail_start = node.id.find_last_of(":/.");
      if (tail_start != std::string::npos && tail_start + 1 < node.id.size()) {
        const auto tail = std::string_view{node.id}.substr(tail_start + 1);
        distance = bounded_edit_distance(lower_needle, ascii_lower(tail), cap);
      }
    }
    if (distance <= cap) {
      scored.emplace_back(distance, &node);
    }
  }
  std::ranges::sort(scored, [](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) {
      return lhs.first < rhs.first;  // closer first
    }
    const auto lc = node_centrality(*lhs.second);
    const auto rc = node_centrality(*rhs.second);
    if (lc != rc) {
      return lc > rc;  // more important first
    }
    return lhs.second->label < rhs.second->label;
  });
  if (scored.size() > kMaxSuggestions) {
    scored.resize(kMaxSuggestions);
  }
  for (const auto& [distance, node] : scored) {
    suggestions.push_back(node_brief(*node));
  }
  return suggestions;
}

// Resolve a node by exact id, exact label, or bare symbol name (the label's
// leading token, case-insensitive) — every id-taking op accepts any of these,
// so an agent can pass a symbol name without a prior query round-trip. When a
// bare name is ambiguous, the highest-centrality match wins; the response
// echoes the resolved id so the agent sees which one.
[[nodiscard]] const Node* resolve_node(
    const GraphSnapshot& graph,
    const std::unordered_map<std::string, const Node*>& by_id,
    const std::string& key) {
  if (const auto it = by_id.find(key); it != by_id.end()) {
    return it->second;
  }
  for (const auto& node : graph.nodes) {
    if (node.label == key) {
      return &node;
    }
  }
  const auto lower_key = ascii_lower(key);
  const Node* best = nullptr;
  for (const auto& node : graph.nodes) {
    if (ascii_lower(label_symbol(node)) != lower_key) {
      continue;
    }
    if (best == nullptr || node_centrality(node) > node_centrality(*best) ||
        (node_centrality(node) == node_centrality(*best) && node.label < best->label)) {
      best = &node;
    }
  }
  return best;
}

// ---- query intent routing (route-query-by-intent) ---------------------------

// Neighbors of `node` reachable by one typed edge, ranked most-important-first.
// Shared by the structural route (one relation/direction) and the entity route's
// caller/callee/reference summary, so neither duplicates explain_node's walk.
[[nodiscard]] std::vector<const Node*> typed_neighbors(
    const GraphSnapshot& graph,
    const std::unordered_map<std::string, const Node*>& by_id,
    const Node& node, std::string_view relation, std::string_view direction) {
  std::vector<const Node*> out;
  for (const auto& edge : graph.edges) {
    const bool outgoing = edge.source == node.id;
    const bool incoming = edge.target == node.id;
    if (!outgoing && !incoming) {
      continue;
    }
    if ((direction == "out" && !outgoing) || (direction == "in" && !incoming)) {
      continue;
    }
    if (!relation.empty() && edge.relation != relation) {
      continue;
    }
    const auto& other = outgoing ? edge.target : edge.source;
    if (const auto it = by_id.find(other); it != by_id.end()) {
      out.push_back(it->second);
    }
  }
  std::ranges::stable_sort(out, [](const Node* lhs, const Node* rhs) {
    return node_centrality(*lhs) > node_centrality(*rhs);
  });
  return out;
}

// A compact {count, top:[id...]} summary for one neighbor bucket of the entity route.
[[nodiscard]] nlohmann::json neighbor_summary(const std::vector<const Node*>& nodes, std::size_t top) {
  auto ids = nlohmann::json::array();
  for (std::size_t i = 0; i < nodes.size() && i < top; ++i) {
    ids.push_back(nodes[i]->id);
  }
  return {{"count", nodes.size()}, {"top", std::move(ids)}};
}

// A structural-intent phrase ("who calls X") parses to a typed traversal plus the
// operand symbol text. Fixed, case-insensitive grammar; an NL query essentially
// never trips these prefixes. Relation tokens reuse the stored edge-type strings.
struct StructuralIntent {
  std::string relation;
  std::string direction;    // "in" | "out"
  std::string intent_name;  // callers | callees | references | implementations | importers
  std::string operand;
};

[[nodiscard]] std::optional<StructuralIntent> parse_structural_phrase(const std::string& needle) {
  struct Rule {
    std::string_view prefix;
    std::string_view suffix;
    std::string_view relation;
    std::string_view direction;
    std::string_view name;
  };
  static constexpr std::array<Rule, 14> rules{{
      {"callers of ", "", "CALLS", "in", "callers"},
      {"who calls ", "", "CALLS", "in", "callers"},
      {"what calls ", "", "CALLS", "in", "callers"},
      {"callees of ", "", "CALLS", "out", "callees"},
      {"what does ", " call", "CALLS", "out", "callees"},
      {"references to ", "", "references", "in", "references"},
      {"who references ", "", "references", "in", "references"},
      {"uses of ", "", "references", "in", "references"},
      {"implementations of ", "", "inherits", "in", "implementations"},
      {"who implements ", "", "inherits", "in", "implementations"},
      {"subclasses of ", "", "inherits", "in", "implementations"},
      {"who extends ", "", "inherits", "in", "implementations"},
      {"importers of ", "", "imports", "in", "importers"},
      {"who imports ", "", "imports", "in", "importers"},
  }};
  const auto lower = ascii_lower(needle);
  for (const auto& rule : rules) {
    if (!lower.starts_with(rule.prefix)) {
      continue;
    }
    const std::size_t begin = rule.prefix.size();
    std::size_t end = needle.size();
    if (!rule.suffix.empty()) {
      if (lower.size() < rule.prefix.size() + rule.suffix.size() || !lower.ends_with(rule.suffix)) {
        continue;
      }
      end = needle.size() - rule.suffix.size();
    }
    std::string operand = needle.substr(begin, end - begin);
    while (!operand.empty() && (operand.back() == ' ' || operand.back() == '\t' || operand.back() == '?')) {
      operand.pop_back();
    }
    while (!operand.empty() && (operand.front() == ' ' || operand.front() == '\t')) {
      operand.erase(operand.begin());
    }
    if (operand.empty()) {
      continue;
    }
    return StructuralIntent{std::string(rule.relation), std::string(rule.direction),
                            std::string(rule.name), std::move(operand)};
  }
  return std::nullopt;
}

[[nodiscard]] nlohmann::json query_graph(
    const GraphSnapshot& graph,
    const nlohmann::json& params,
    SnapshotSourceReader& source_reader) {
  const auto needle = params.value("q", params.value("query", std::string{}));
  const auto by_id = index_nodes(graph);
  const auto limit = params.value("limit", kDefaultQueryLimit);
  const auto kind = ascii_lower(params.value("kind", std::string{}));
  const auto file = ascii_lower(params.value("file", std::string{}));

  const auto narrow = [&](std::vector<const Node*>& set) {
    if (!kind.empty()) {
      std::erase_if(set, [&](const Node* node) { return ascii_lower(node->kind) != kind; });
    }
    if (!file.empty()) {
      std::erase_if(set, [&](const Node* node) { return !contains_ci(node->source_file, file); });
    }
  };
  const auto as_nodes = [&](std::vector<const Node*> set, std::string route,
                            nlohmann::json extra) -> nlohmann::json {
    narrow(set);
    const auto total = set.size();
    if (limit > 0 && set.size() > limit) {
      set.resize(limit);
    }
    auto nodes = nlohmann::json::array();
    for (const auto* node : set) {
      nodes.push_back(node_brief(*node));
    }
    const auto returned = nodes.size();
    nlohmann::json result{{"route", std::move(route)}, {"nodes", std::move(nodes)},
                          {"total", total}, {"returned", returned}};
    result.update(std::move(extra));
    return result;
  };

  // Route 1 — a structural-intent phrase whose operand resolves: return the typed
  // neighbor set directly (the same set explain would, filtered to that relation).
  if (const auto intent = parse_structural_phrase(needle)) {
    if (const Node* target = resolve_node(graph, by_id, intent->operand)) {
      return as_nodes(typed_neighbors(graph, by_id, *target, intent->relation, intent->direction),
                      intent->intent_name, {{"of", target->id}});
    }
  }

  auto matches = matching_nodes(graph, needle);

  // Route 2 — a whitespace-free needle that names exactly one symbol: return that
  // entity plus a compact typed-neighbor summary, so "find X and who calls it" is one
  // call. The gate is a unique *exact* match (id / label / leading label symbol),
  // counted independently of substring neighbors. A name that equals no symbol but is
  // a substring of several (e.g. "pha" -> Alpha, AlphaLeaf) stays a search (route 3),
  // and a name several symbols share (same type in N files) is ambiguous and also stays
  // a search -- but a precise symbol like "HandlerCtx" still routes to entity even on a
  // large graph where its token collides as a substring of many other ids/labels.
  // Every exact match is necessarily in `matches` (a string contains itself), so the
  // substring set is a complete candidate pool to filter.
  if (!needle.empty() && needle.find_first_of(" \t") == std::string::npos) {
    const auto low = ascii_lower(needle);
    const auto is_exact = [&](const Node* node) {
      return node->id == needle || ascii_lower(node->label) == low ||
             ascii_lower(label_symbol(*node)) == low;
    };
    const Node* only = nullptr;
    std::size_t exact_count = 0;
    for (const Node* node : matches) {
      if (is_exact(node)) {
        ++exact_count;
        only = node;
      }
    }
    if (exact_count == 1) {
      auto nodes = nlohmann::json::array();
      nodes.push_back(with_source(node_brief(*only), *only, source_reader));
      return {
          {"route", "entity"},
          {"nodes", std::move(nodes)},
          {"neighbors",
           {{"callers", neighbor_summary(typed_neighbors(graph, by_id, *only, "CALLS", "in"), 5)},
            {"callees", neighbor_summary(typed_neighbors(graph, by_id, *only, "CALLS", "out"), 5)},
            {"references", neighbor_summary(typed_neighbors(graph, by_id, *only, "references", "in"), 5)}}},
          {"total", 1},
          {"returned", 1}};
    }
  }

  // Route 3 — lexical search. Natural-language queries are almost never a substring
  // of a symbol label; fall back to lexical term overlap so the op returns relevant
  // symbols instead of nothing. Lexical results arrive already ranked by overlap,
  // so the centrality re-sort below is skipped for them.
  const bool lexical_fallback = matches.empty();
  if (lexical_fallback) {
    matches = lexical_matches(graph, lexical_terms(needle));
  }
  narrow(matches);
  if (!lexical_fallback) {
    std::ranges::sort(matches, [](const Node* lhs, const Node* rhs) {
      const auto lc = node_centrality(*lhs);
      const auto rc = node_centrality(*rhs);
      if (lc != rc) {
        return lc > rc;
      }
      return lhs->label < rhs->label;  // stable, deterministic tiebreak
    });
  }
  // Enrichment nodes (doc:/concept:/media:/topic:) are host-authored prose about
  // the code, not code symbols. Keep them after structural results so a code
  // search surfaces code first -- an agent looking up `resolveRepo` should get
  // the functions, not a saved memory that merely mentions the name. They still
  // appear (no eclipse) and `total` is unchanged; this is a stable partition, so
  // the centrality/overlap order within each group is preserved.
  std::ranges::stable_partition(
      matches, [](const Node* node) { return !is_enrichment_node_id(node->id); });

  const auto total = matches.size();
  if (limit > 0 && matches.size() > limit) {
    matches.resize(limit);
  }
  auto nodes = nlohmann::json::array();
  for (const auto* node : matches) {
    nodes.push_back(node_brief(*node));
  }
  const auto returned = nodes.size();  // capture before the move below
  nlohmann::json result{{"route", "search"}, {"nodes", std::move(nodes)}, {"total", total},
                        {"returned", returned}};
  if (total == 0) {
    result["suggestions"] = suggest_similar(graph, needle);
  }
  return result;
}

// Transitive blast radius: BFS from a node along directed edges. `dependents`
// follows edges *into* the node (callers, importers, subclasses — what breaks if
// you change it); `dependencies` follows edges *out* (what it relies on); `both`
// unions the two. Optionally filtered to one relation, bounded by depth, and
// capped. Results are ordered by depth, then by centrality within a depth.
[[nodiscard]] nlohmann::json impact_radius(const GraphSnapshot& graph, const nlohmann::json& params) {
  const auto id = params.value("id", std::string{});
  const auto direction = params.value("direction", std::string{"dependents"});
  const auto relation = params.value("relation", std::string{});
  const auto max_depth = std::max(0, params.value("max_depth", kDefaultImpactDepth));
  const auto limit = params.value("limit", kDefaultImpactLimit);

  const auto by_id = index_nodes(graph);
  const auto* seed = resolve_node(graph, by_id, id);
  if (seed == nullptr) {
    return {{"id", id}, {"found", false}, {"direction", direction}, {"max_depth", max_depth},
            {"total", 0}, {"returned", 0}, {"nodes", nlohmann::json::array()},
            {"suggestions", suggest_similar(graph, id)}};
  }
  // The canonical id (the requested key may have been a label).
  const auto& seed_id = seed->id;

  const bool want_dependents = direction == "dependents" || direction == "both";
  const bool want_dependencies = direction == "dependencies" || direction == "both";

  // Adjacency in the requested direction(s), filtered by relation if given.
  struct Link {
    std::string to;
    std::string relation;
  };
  std::unordered_map<std::string, std::vector<Link>> adjacency;
  for (const auto& edge : graph.edges) {
    if (!relation.empty() && edge.relation != relation) {
      continue;
    }
    if (want_dependents) {
      adjacency[edge.target].push_back({edge.source, edge.relation});  // who points at target
    }
    if (want_dependencies) {
      adjacency[edge.source].push_back({edge.target, edge.relation});  // what source points to
    }
  }

  struct Reached {
    int depth = 0;
    std::string via;
  };
  std::unordered_map<std::string, Reached> reached;
  std::queue<std::string> frontier;
  reached[seed_id] = {0, {}};
  frontier.push(seed_id);
  while (!frontier.empty()) {
    const auto current = frontier.front();
    frontier.pop();
    const auto depth = reached[current].depth;
    if (depth >= max_depth) {
      continue;
    }
    const auto links = adjacency.find(current);
    if (links == adjacency.end()) {
      continue;
    }
    for (const auto& link : links->second) {
      if (reached.contains(link.to)) {
        continue;  // first (shortest) path wins
      }
      reached[link.to] = {depth + 1, link.relation};
      frontier.push(link.to);
    }
  }

  // Drop the seed itself; order by (depth asc, centrality desc).
  std::vector<const Node*> hits;
  for (const auto& [node_id, info] : reached) {
    if (node_id == seed_id) {
      continue;
    }
    if (const auto it = by_id.find(node_id); it != by_id.end()) {
      hits.push_back(it->second);
    }
  }
  std::ranges::sort(hits, [&](const Node* lhs, const Node* rhs) {
    const auto ld = reached[lhs->id].depth;
    const auto rd = reached[rhs->id].depth;
    if (ld != rd) {
      return ld < rd;
    }
    const auto lc = node_centrality(*lhs);
    const auto rc = node_centrality(*rhs);
    if (lc != rc) {
      return lc > rc;
    }
    return lhs->label < rhs->label;
  });

  const auto total = hits.size();
  const bool truncated = limit > 0 && hits.size() > limit;
  if (truncated) {
    hits.resize(limit);
  }

  auto nodes = nlohmann::json::array();
  for (const auto* node : hits) {
    auto brief = node_brief(*node);
    brief["depth"] = reached[node->id].depth;
    if (!reached[node->id].via.empty()) {
      brief["via"] = reached[node->id].via;
    }
    nodes.push_back(std::move(brief));
  }

  // Echo the canonical id (the request may have used the label).
  nlohmann::json result{
      {"id", seed_id}, {"direction", direction}, {"max_depth", max_depth},
      {"total", total}, {"returned", nodes.size()}, {"nodes", std::move(nodes)}};
  if (truncated) {
    result["truncated"] = true;
  }
  return result;
}

// Token-budgeted context bundle. Given a focal symbol (by id, label, or the
// highest-centrality match for a query) and a token budget, gather the
// surrounding neighborhood (undirected BFS to max_depth), rank it by
// (depth asc, centrality desc), and greedily pack source snippets until the
// budget fills. A candidate whose full snippet would overflow degrades to a
// brief-only entry (so the agent still learns the symbol exists and where);
// anything that no longer fits is counted in `omitted`. The focal node is always
// included with its snippet, even if it alone exceeds the budget.
[[nodiscard]] nlohmann::json pack_context(
    const GraphSnapshot& graph,
    const nlohmann::json& params,
    SnapshotSourceReader& source_reader) {
  // Read signed and clamp: a negative budget must floor at 0 (focal-only,
  // truncated), not wrap to a practically-unbounded unsigned ceiling.
  const auto raw_budget =
      params.value("budget", static_cast<long long>(kDefaultContextBudget));
  const std::size_t budget = raw_budget > 0 ? static_cast<std::size_t>(raw_budget) : 0;
  // Packing strategy: "greedy" (default ordered full/brief degradation) or
  // "knapsack" (the step-A-validated 0/1 fill). Knapsack gathers a wider ego graph
  // by default; both strategies finish candidate selection from metadata first.
  const auto packing = params.value("packing", std::string{"greedy"});
  // Adaptive relevance-gated gather (the default; pass gather="fixed" to opt out):
  // keeps the full 2-hop core but expands past depth 1 only along query-relevant
  // nodes, reaching beyond 2 hops without the full 3-hop fan-out. Gather and fill
  // are independent: adaptive widens the candidate pool (and raises the default
  // depth) but fills rank-ordered greedy unless knapsack is asked for. Adaptive
  // used to imply the knapsack fill; on the current, denser graph that cost the
  // default path 2.6-5.9 recall points against plain fixed-k2 greedy (measured
  // per-row at budgets 2000/4000, research/packer-regression), because the wider
  // pool fed the DP confetti to prefer. A wider pool is harmless under a greedy
  // rank-order fill -- nearer, higher-centrality candidates pack first.
  const auto gather = params.value("gather", std::string{"adaptive"});
  const bool adaptive = gather == "adaptive";
  const double gather_theta = std::clamp(params.value("gather_theta", 0.05), 0.0, 1.0);
  const bool use_knapsack = packing == "knapsack";
  const int default_depth = (use_knapsack || adaptive) ? kKnapsackContextDepth : kDefaultContextDepth;
  const auto max_depth = std::max(0, params.value("max_depth", default_depth));
  const auto by_id = index_nodes(graph);

  // Resolve the focal node: exact id, then label, then the top-ranked match for
  // a free-text query.
  const auto id = params.value("id", std::string{});
  const auto needle = params.value("q", params.value("query", std::string{}));
  const Node* focal = id.empty() ? nullptr : resolve_node(graph, by_id, id);
  // The gather is seeded from `seeds`. For an exact/substring/id resolution that is
  // just the focal; a free-text query that resolves only via lexical overlap seeds
  // from the top-N matches and unions their ego graphs (the dominant recall lever —
  // a single lexical seed is the right symbol only ~23% of the time).
  std::vector<const Node*> seeds;
  if (focal == nullptr && !needle.empty()) {
    for (const auto* match : matching_nodes(graph, needle)) {
      if (focal == nullptr || node_centrality(*match) > node_centrality(*focal)) {
        focal = match;
      }
    }
    if (focal == nullptr) {
      seeds = lexical_matches(graph, lexical_terms(needle));
      if (seeds.size() > kFocalSeedCount) {
        seeds.resize(kFocalSeedCount);
      }
      focal = seeds.empty() ? nullptr : seeds.front();
    }
  }
  if (focal == nullptr) {
    return {{"focus", nullptr}, {"budget", budget}, {"tokens_used", 0},
            {"packing", use_knapsack ? "knapsack" : "greedy"}, {"gather", adaptive ? "adaptive" : "fixed"},
            {"included", nlohmann::json::array()}, {"omitted", 0},
            {"suggestions", suggest_similar(graph, id.empty() ? needle : id)}};
  }
  if (seeds.empty()) {
    seeds.push_back(focal);  // exact / substring / id resolution stays single-seed
  }

  // Query terms for the adaptive gather gate and the knapsack relevance value.
  const auto query_terms = lexical_terms(needle);

  // Undirected neighborhood: callers, callees, container, and siblings reachable
  // within max_depth are all relevant context for understanding the focal symbol.
  struct Reached {
    int depth = 0;
    std::string via;
  };
  std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> adjacency;
  for (const auto& edge : graph.edges) {
    adjacency[edge.source].push_back({edge.target, edge.relation});
    adjacency[edge.target].push_back({edge.source, edge.relation});
  }
  std::unordered_map<std::string, Reached> reached;
  std::queue<std::string> frontier;
  // Seed the walk from every lexical seed at depth 0 (usually just the focal); a
  // node first reached from a lower-ranked seed keeps its shallowest depth.
  for (const auto* seed : seeds) {
    if (reached.emplace(seed->id, Reached{0, {}}).second) {
      frontier.push(seed->id);
    }
  }
  // Adaptive reach accounting: how many frontier nodes the relevance gate rejected
  // past the 2-hop core. Reported on the response so callers/telemetry can see
  // whether the gate actually narrowed the third hop.
  int gated_at_core = 0;
  while (!frontier.empty()) {
    const auto current = frontier.front();
    frontier.pop();
    const auto depth = reached[current].depth;
    if (depth >= max_depth) {
      continue;
    }
    // Adaptive gather: past the 2-hop core, expand a node only when it is relevant
    // to the query, so the third hop follows relevant paths, not the whole ball.
    if (adaptive && depth >= 2) {
      const auto node_it = by_id.find(current);
      if (node_it == by_id.end() ||
          query_term_overlap(query_terms, node_it->second->label) < gather_theta) {
        ++gated_at_core;
        continue;
      }
    }
    const auto links = adjacency.find(current);
    if (links == adjacency.end()) {
      continue;
    }
    for (const auto& [to, relation] : links->second) {
      if (reached.contains(to)) {
        continue;
      }
      reached[to] = {depth + 1, relation};
      frontier.push(to);
    }
  }

  // The graph has no direct sibling edge between otherwise-unrelated symbols in
  // one file. Under adaptive gathering only, admit siblings from the primary
  // focal's file as direct, query-ranked candidates. They do not enter `frontier`:
  // the measured opportunity is focal-file context, not opening another graph
  // neighborhood through every sibling. Fixed gathering remains structurally
  // untouched by keeping the whole path behind `adaptive`.
  if (adaptive && !query_terms.empty() && !focal->source_file.empty()) {
    std::vector<std::pair<double, const Node*>> same_file;
    for (const auto& node : graph.nodes) {
      if (node.source_file != focal->source_file || node.kind == "file" ||
          is_memory_node_id(node.id) || reached.contains(node.id)) {
        continue;
      }
      same_file.emplace_back(query_term_overlap(query_terms, node.label), &node);
    }
    std::ranges::sort(same_file, [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) {
        return lhs.first > rhs.first;
      }
      const auto lc = node_centrality(*lhs.second);
      const auto rc = node_centrality(*rhs.second);
      if (lc != rc) {
        return lc > rc;
      }
      return lhs.second->id < rhs.second->id;
    });

    const auto admitted = std::min(kSameFileCandidateCap, same_file.size());
    for (std::size_t i = 0; i < admitted; ++i) {
      // Same-file proximity is weaker evidence than a persisted graph edge.
      // Depth two keeps these candidates available without tying direct
      // structural neighbors in the knapsack value model.
      reached.emplace(same_file[i].second->id, Reached{2, "same_file"});
    }
  }

  std::vector<const Node*> candidates;
  int expanded_past_core = 0;  // candidates reached beyond the 2-hop core (depth >= 3)
  for (const auto& [node_id, info] : reached) {
    if (node_id == focal->id) {
      continue;
    }
    if (is_memory_node_id(node_id)) {
      continue;  // memory checkpoints never enter code context, even when a concerns edge makes them adjacent
    }
    if (const auto it = by_id.find(node_id); it != by_id.end()) {
      candidates.push_back(it->second);
      if (info.depth >= 3) {
        ++expanded_past_core;
      }
    }
  }
  std::ranges::sort(candidates, [&](const Node* lhs, const Node* rhs) {
    const auto ld = reached[lhs->id].depth;
    const auto rd = reached[rhs->id].depth;
    if (ld != rd) {
      return ld < rd;  // nearer first
    }
    const auto lc = node_centrality(*lhs);
    const auto rc = node_centrality(*rhs);
    if (lc != rc) {
      return lc > rc;  // more important first
    }
    return lhs->label < rhs->label;
  });

  const auto candidate_brief = [&](const Node& node) {
    auto brief = node_brief(node);
    brief["depth"] = reached[node.id].depth;
    if (const auto& via = reached[node.id].via; !via.empty()) {
      brief["via"] = via;
    }
    return brief;
  };

  // Knapsack packing path (flag-gated; greedy below is the default and unchanged).
  // 0/1 knapsack over candidates: weight = metadata-estimated source-slice token
  // cost, value = relevance (nearer hops + query-term overlap). The focal is always
  // included; the knapsack fills the remaining budget. No source path is touched
  // until backtracking has produced the final whole-or-nothing selection.
  if (use_knapsack) {
    std::vector<std::size_t> weight(candidates.size());
    std::vector<double> value(candidates.size());
    std::size_t total_weight = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      const auto* node = candidates[i];
      weight[i] = slice_token_cost(*node);
      const auto depth = reached[node->id].depth;
      const auto overlap = query_term_overlap(query_terms, node->label);
      // Same-file admission is an inferred relationship, so it earns lexical
      // value only. Giving it the ordinary hop bonus displaced real graph
      // neighbors when lexical focal resolution chose an ambiguous primary.
      value[i] = reached[node->id].via == "same_file"
                     ? overlap
                     : 1.0 / (1.0 + static_cast<double>(depth)) + overlap;
      // Scale by sqrt(slice size): with per-ITEM value the DP's optimum is many
      // tiny weakly-relevant entries -- on this repo's graph it packed 21 entries
      // of median ~143 tokens while shedding the 240-313-token symbols the query
      // was actually about (grade-2 recall 0.407 vs greedy's 0.494 @4000,
      // research/packer-regression). The sqrt curve prices longer slices as
      // adding context sublinearly: measured deltas against greedy land at
      // 0.009-0.012 across budgets 2000-8000, inside the parity band at every
      // budget, where linear cost-scaling sat on the band edge at 6000.
      value[i] *= std::sqrt(static_cast<double>(weight[i]));
      total_weight += weight[i];
    }

    const std::size_t focal_cost = slice_token_cost(*focal);
    const std::size_t raw_capacity = budget > focal_cost ? budget - focal_cost : 0;
    // DP-capacity guard: never allocate beyond what the candidates can fill, and
    // clamp pathological budgets so the O(n*capacity) table stays bounded.
    const std::size_t capacity = std::min({raw_capacity, total_weight, kMaxKnapsackCapacity});

    std::vector<const Node*> chosen;
    std::size_t selection_used = focal_cost;
    if (capacity > 0 && !candidates.empty()) {
      const std::size_t n = candidates.size();
      // dp[i][c] = max total value using the first i candidates within weight c.
      std::vector<std::vector<double>> dp(n + 1, std::vector<double>(capacity + 1, 0.0));
      for (std::size_t i = 1; i <= n; ++i) {
        const auto w = weight[i - 1];
        const auto v = value[i - 1];
        for (std::size_t c = 0; c <= capacity; ++c) {
          dp[i][c] = dp[i - 1][c];
          if (w <= c) {
            const double take = dp[i - 1][c - w] + v;
            if (take > dp[i][c]) {
              dp[i][c] = take;
            }
          }
        }
      }
      // Backtrack to recover the chosen items (size_t-safe reverse iteration).
      std::size_t c = capacity;
      for (std::size_t i = n; i-- > 0;) {
        if (dp[i + 1][c] != dp[i][c]) {
          chosen.push_back(candidates[i]);
          selection_used += weight[i];
          c -= weight[i];
        }
      }
    }

    // Emit nearest-first for a stable, readable bundle (selection is the DP above).
    std::ranges::sort(chosen, [&](const Node* lhs, const Node* rhs) {
      const auto ld = reached[lhs->id].depth;
      const auto rd = reached[rhs->id].depth;
      if (ld != rd) {
        return ld < rd;
      }
      return lhs->label < rhs->label;
    });

    auto focus = with_source(node_brief(*focal), *focal, source_reader);
    annotate_snippet_absence(focus, *focal);

    // Render each selected entry, then re-cost the selection at its TRUE
    // serialized size and shed until it fits. The DP's slice-cost weights above
    // are deliberately kept as the ranking heuristic -- including the JSON
    // overhead flattens the weight spread and degenerates the knapsack toward
    // greedy -- but the report and the budget test are measured, never
    // estimated. Numbers and method: openspec/changes/honest-context-budget.
    struct Selected {
      nlohmann::json entry;
      std::size_t bytes = 0;  // compact-serialized entry length
      std::size_t cost = 0;   // estimate_tokens over that length
      double value = 0.0;
      std::size_t order = 0;
    };
    std::unordered_map<std::string, double> value_by_id;
    value_by_id.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      value_by_id[candidates[i]->id] = value[i];
    }
    std::vector<Selected> selected;
    selected.reserve(chosen.size());
    for (std::size_t i = 0; i < chosen.size(); ++i) {
      const auto* node = chosen[i];
      auto full = with_source(candidate_brief(*node), *node, source_reader);
      annotate_snippet_absence(full, *node);
      const auto bytes = full.dump().size();
      selected.push_back(Selected{
          std::move(full), bytes, estimate_tokens_for_length(bytes), value_by_id[node->id], i});
    }

    // The focal entry is charged first and is never dropped: a small budget
    // still answers with the symbol the caller asked about. Shed by ascending
    // value DENSITY (value per serialized token) so a cheap relevant row
    // outlives an expensive marginal one -- shedding by raw value systematically
    // protected snippet-less depth-1 rows over depth-2 code (the four-arm
    // comparison lives in openspec/changes/honest-context-budget).
    const std::size_t focus_cost = estimate_tokens(focus.dump());
    const auto density = [](const Selected& item) {
      return item.value / static_cast<double>(std::max<std::size_t>(1, item.cost));
    };
    std::ranges::sort(selected, [&](const Selected& lhs, const Selected& rhs) {
      return density(lhs) < density(rhs);
    });
    // O(n) exact shed: a compact JSON array serializes to
    // 2 + sum(entry_bytes) + (k-1) bytes for k>0 entries (brackets + commas),
    // so the measured cost of every suffix falls out of one byte total --
    // re-dumping survivors per step (O(n^2)) regressed the context op 4x at
    // the default budget and ~16x on sourceless-heavy graphs.
    std::size_t suffix_bytes = 0;
    for (const auto& item : selected) {
      suffix_bytes += item.bytes;
    }
    const auto suffix_cost = [&](std::size_t kept, std::size_t bytes) {
      const std::size_t array_len = kept > 0 ? 2 + bytes + (kept - 1) : 2;
      return focus_cost + estimate_tokens_for_length(array_len);
    };
    std::size_t dropped_over_budget = 0;
    while (dropped_over_budget < selected.size() &&
           suffix_cost(selected.size() - dropped_over_budget, suffix_bytes) > budget) {
      suffix_bytes -= selected[dropped_over_budget].bytes;
      ++dropped_over_budget;
    }
    selected.erase(selected.begin(),
                   selected.begin() + static_cast<std::ptrdiff_t>(dropped_over_budget));
    // Restore the nearest-first emit order the shed sort disturbed.
    std::ranges::sort(selected, [](const Selected& lhs, const Selected& rhs) {
      return lhs.order < rhs.order;
    });

    auto included = nlohmann::json::array();
    for (auto& item : selected) {
      included.push_back(std::move(item.entry));
    }
    const auto used = emitted_entry_tokens(focus, included);
    const std::size_t omitted = candidates.size() - chosen.size() + dropped_over_budget;
    nlohmann::json result{
        {"focus", std::move(focus)},
        {"budget", budget},
        {"tokens_used", used},
        {"selection_tokens_used", selection_used},
        {"budget_basis", "measured_serialized_tokens"},
        {"packing", "knapsack"},
        {"gather", adaptive ? "adaptive" : "fixed"},
        {"source_files_read", source_reader.files_read()},
        {"included", std::move(included)},
        {"omitted", omitted}};
    if (omitted > 0 || used > budget) {
      result["truncated"] = true;
    }
    // Adaptive reach summary: did the relevance gate actually expand the third hop,
    // and how much did it prune? expanded_past_core == 0 is the honest "collapsed
    // to the 2-hop core" signal. Present only for adaptive gather.
    if (adaptive) {
      result["reach"] = {{"candidates", candidates.size()},
                         {"expanded_past_core", expanded_past_core},
                         {"gated_at_core", gated_at_core}};
    }
    return result;
  }

  struct PlannedEntry {
    const Node* node = nullptr;
    nlohmann::json entry;
    bool materialize = false;
  };
  std::vector<PlannedEntry> planned;
  planned.reserve(candidates.size());
  std::size_t projected_used =
      projected_source_entry_token_cost(node_brief(*focal), *focal);
  std::size_t omitted = 0;
  for (const auto* node : candidates) {
    // Same-file admission is an inferred relationship and packs only on lexical
    // evidence. The knapsack encodes this as zero value (never taken); greedy
    // has no value gate, so the equivalent is skipping the candidate outright --
    // otherwise a generous budget floods the bundle with unrelated same-file
    // siblings.
    if (reached[node->id].via == "same_file" &&
        query_term_overlap(query_terms, node->label) == 0.0) {
      continue;
    }
    auto brief = candidate_brief(*node);
    const auto full_cost = projected_source_entry_token_cost(brief, *node);
    if (projected_used <= budget && full_cost <= budget - projected_used) {
      projected_used += full_cost;
      planned.push_back({node, std::move(brief), true});
      continue;
    }

    // Full snippet overflows: keep a brief-only entry if it still fits.
    brief["snippet_omitted"] = true;
    const auto brief_cost = estimate_tokens(brief.dump());
    if (projected_used <= budget && brief_cost <= budget - projected_used) {
      projected_used += brief_cost;
      planned.push_back({node, std::move(brief), false});
    } else {
      ++omitted;
    }
  }

  // All full/brief/omitted decisions above are metadata-only. Only the focal and
  // entries selected for full emission reach SnapshotSourceReader.
  auto focus = with_source(node_brief(*focal), *focal, source_reader);
  annotate_snippet_absence(focus, *focal);
  auto included = nlohmann::json::array();
  for (auto& item : planned) {
    auto entry = item.materialize
                     ? with_source(std::move(item.entry), *item.node, source_reader)
                     : std::move(item.entry);
    annotate_snippet_absence(entry, *item.node);
    included.push_back(std::move(entry));
  }
  // The projection above is a plan; the ceiling is measured. Greedy's insertion
  // order is its priority order (depth, then centrality), so it sheds from the
  // end -- positionally, unlike the knapsack's density order; a shed FULL row
  // can therefore outrank a surviving earlier BRIEF row, which is the
  // documented cost of keeping greedy's ordering byte-stable. O(n) via prefix
  // byte sums (see the knapsack shed above for the arithmetic).
  {
    const std::size_t greedy_focus_cost = estimate_tokens(focus.dump());
    std::vector<std::size_t> entry_bytes;
    entry_bytes.reserve(included.size());
    std::size_t total_bytes = 0;
    for (const auto& item : included) {
      entry_bytes.push_back(item.dump().size());
      total_bytes += entry_bytes.back();
    }
    std::size_t kept = included.size();
    while (kept > 0) {
      const std::size_t array_len = 2 + total_bytes + (kept - 1);
      if (greedy_focus_cost + estimate_tokens_for_length(array_len) <= budget) {
        break;
      }
      --kept;
      total_bytes -= entry_bytes[kept];
    }
    while (included.size() > kept) {
      included.erase(included.end() - 1);
      ++omitted;
    }
  }
  const auto used = emitted_entry_tokens(focus, included);

  nlohmann::json result{
      {"focus", std::move(focus)},
      {"budget", budget},
      {"tokens_used", used},
      {"selection_tokens_used", projected_used},
      {"budget_basis", "measured_serialized_tokens"},
      {"packing", "greedy"},
      {"gather", adaptive ? "adaptive" : "fixed"},
      {"source_files_read", source_reader.files_read()},
      {"included", std::move(included)},
      {"omitted", omitted}};
  if (omitted > 0 || used > budget) {
    result["truncated"] = true;
  }
  // The reach summary describes the GATHER stage, so it accompanies adaptive
  // regardless of which fill packed the candidates.
  if (adaptive) {
    result["reach"] = {{"candidates", candidates.size()},
                       {"expanded_past_core", expanded_past_core},
                       {"gated_at_core", gated_at_core}};
  }
  return result;
}

[[nodiscard]] nlohmann::json explain_node(
    const GraphSnapshot& graph,
    const nlohmann::json& params,
    SnapshotSourceReader& source_reader) {
  const auto id = params.value("id", std::string{});
  // "in" keeps only edges into the node (callers/importers), "out" only edges
  // it points at (callees/imports); default is both.
  const auto direction = params.value("direction", std::string{"both"});
  // Optional typed traversal: when set, keep only edges of this relation, using
  // the same exact match as impact_radius (no case-folding) so the two ops share
  // one filter dialect. find-callers = direction:in + relation:CALLS, etc.
  const auto relation = params.value("relation", std::string{});
  const auto limit = params.value("limit", kDefaultExplainNeighborLimit);

  const auto by_id = index_nodes(graph);
  const auto* node = resolve_node(graph, by_id, id);
  if (node == nullptr) {
    return {{"id", id}, {"found", false}, {"neighbors", nlohmann::json::array()},
            {"suggestions", suggest_similar(graph, id)}};
  }

  struct NeighborEntry {
    nlohmann::json entry;
    double centrality = 0.0;
  };
  std::vector<NeighborEntry> neighbors;
  for (const auto& edge : graph.edges) {
    const bool outgoing = edge.source == node->id;
    const bool incoming = edge.target == node->id;
    if (!outgoing && !incoming) {
      continue;
    }
    if ((direction == "out" && !outgoing) || (direction == "in" && !incoming)) {
      continue;
    }
    if (!relation.empty() && edge.relation != relation) {
      continue;
    }
    nlohmann::json entry{
        {"source", edge.source},
        {"target", edge.target},
        {"relation", edge.relation},
        {"direction", outgoing ? "out" : "in"}};
    // Attach the brief of the node on the other end so an agent can navigate
    // (open the caller/callee) without a second lookup.
    const auto& other_id = outgoing ? edge.target : edge.source;
    double centrality = 0.0;
    if (const auto it = by_id.find(other_id); it != by_id.end()) {
      entry["node"] = node_brief(*it->second);
      centrality = node_centrality(*it->second);
    }
    neighbors.push_back({std::move(entry), centrality});
  }

  // Most important neighbors first, so a capped list still shows what matters
  // when the node is a heavily-connected hub.
  std::ranges::stable_sort(neighbors, [](const NeighborEntry& lhs, const NeighborEntry& rhs) {
    return lhs.centrality > rhs.centrality;
  });
  const auto neighbor_count = neighbors.size();
  const bool truncated = limit > 0 && neighbors.size() > limit;
  if (truncated) {
    neighbors.resize(limit);
  }

  auto entries = nlohmann::json::array();
  for (auto& neighbor : neighbors) {
    entries.push_back(std::move(neighbor.entry));
  }
  auto result = with_source(node_brief(*node), *node, source_reader);
  result["neighbor_count"] = neighbor_count;
  result["neighbors"] = std::move(entries);
  if (truncated) {
    result["truncated"] = true;
  }
  return result;
}

[[nodiscard]] nlohmann::json shortest_path(const GraphSnapshot& graph, const nlohmann::json& params) {
  const auto by_id_nodes = index_nodes(graph);
  const auto resolve_endpoint = [&](const std::string& key) {
    const auto* node = resolve_node(graph, by_id_nodes, key);
    return node == nullptr ? key : node->id;
  };
  // Endpoints accept labels too; flag the missing one(s) with suggestions so an
  // empty path is distinguishable from "no route exists".
  const auto source_key = params.value("source", std::string{});
  const auto target_key = params.value("target", std::string{});
  const auto source = resolve_endpoint(source_key);
  const auto target = resolve_endpoint(target_key);
  nlohmann::json missing = nlohmann::json::object();
  if (!by_id_nodes.contains(source)) {
    missing["source_found"] = false;
    missing["source_suggestions"] = suggest_similar(graph, source_key);
  }
  if (!by_id_nodes.contains(target)) {
    missing["target_found"] = false;
    missing["target_suggestions"] = suggest_similar(graph, target_key);
  }
  std::unordered_map<std::string, std::vector<std::string>> adjacency;
  for (const auto& edge : graph.edges) {
    adjacency[edge.source].push_back(edge.target);
    adjacency[edge.target].push_back(edge.source);
  }

  std::queue<std::string> queue;
  std::unordered_map<std::string, std::string> previous;
  queue.push(source);
  previous[source] = {};

  while (!queue.empty()) {
    const auto current = queue.front();
    queue.pop();
    if (current == target) {
      break;
    }
    for (const auto& next : adjacency[current]) {
      if (previous.contains(next)) {
        continue;
      }
      previous[next] = current;
      queue.push(next);
    }
  }

  auto path = nlohmann::json::array();
  if (!previous.contains(target)) {
    nlohmann::json result{{"path", path}, {"path_nodes", nlohmann::json::array()}};
    result.update(missing);
    return result;
  }

  std::vector<std::string> reversed;
  for (std::string cursor = target; !cursor.empty(); cursor = previous[cursor]) {
    reversed.push_back(cursor);
  }
  std::ranges::reverse(reversed);

  // `path` stays a bare id list for existing consumers; `path_nodes` carries the
  // label/kind/source_file/line for each hop so an agent can read the route.
  auto path_nodes = nlohmann::json::array();
  for (const auto& item : reversed) {
    path.push_back(item);
    if (const auto it = by_id_nodes.find(item); it != by_id_nodes.end()) {
      path_nodes.push_back(node_brief(*it->second));
    } else {
      path_nodes.push_back({{"id", item}});
    }
  }
  nlohmann::json result{{"path", std::move(path)}, {"path_nodes", std::move(path_nodes)}};
  result.update(missing);
  return result;
}

// Agent-facing build-state label. "building" covers the Empty snapshot the
// daemon serves while the initial build runs on its worker thread.
[[nodiscard]] const char* build_state_label(BuildState value) {
  switch (value) {
    case BuildState::Empty:
      return "building";
    case BuildState::DeterministicReady:
      return "ready";
    case BuildState::Enriching:
      return "enriching";
    case BuildState::Idle:
      return "idle";
    case BuildState::Failed:
      return "failed";
  }
  return "failed";
}

// While the initial build is still running, every read op serves the empty
// snapshot; stamp those results so an agent can tell "no match" from "not
// built yet" instead of silently falling back to grep.
[[nodiscard]] nlohmann::json annotate_build_state(nlohmann::json result, const GraphSnapshot& graph) {
  if (graph.build_state == BuildState::Empty) {
    result["graph_state"] = "building";
    result["note"] = "graph build in progress; results may be empty or incomplete - retry shortly or poll status";
  }
  return result;
}

[[nodiscard]] nlohmann::json decorate_freshness(nlohmann::json result, const GraphSnapshot& graph) {
  result["freshness"] = freshness_metadata(graph);
  return result;
}

[[nodiscard]] nlohmann::json status(const DaemonState& state, const GraphSnapshot& graph) {
  const auto enrichment_state = [](EnrichmentState value) {
    switch (value) {
      case EnrichmentState::Idle:
        return "idle";
      case EnrichmentState::Pending:
        return "pending";
      case EnrichmentState::Running:
        return "running";
      case EnrichmentState::Stale:
        return "stale";
      case EnrichmentState::Failed:
        return "failed";
    }
    return "failed";
  };

  const std::chrono::duration<double> uptime = StatsClock::now() - state.start_time;

  // Snapshot the enrichment counters and the unextracted map under the same lock
  // the writers (enrichment refresh, drop ingest, rescan/incremental update) take,
  // so a constantly-polled status never tears a counter or iterates the map while
  // it is being mutated. Copy out, then build the payload off the local copies.
  EnrichmentState enrichment_state_value = EnrichmentState::Idle;
  std::size_t enrichment_pending = 0;
  std::size_t enrichment_running = 0;
  std::size_t enrichment_stale = 0;
  std::size_t enrichment_failed = 0;
  std::size_t enrichment_plans_run = 0;
  std::map<std::string, std::size_t> unextracted;
  std::size_t last_files_cache_hit = 0;
  double last_extract_mean_ms = 0.0;
  std::size_t last_memory_overlay_count = 0;
  {
    const std::scoped_lock lock(state.enrichment_mutex);
    enrichment_state_value = state.enrichment_state;
    enrichment_pending = state.enrichment_pending;
    enrichment_running = state.enrichment_running;
    enrichment_stale = state.enrichment_stale;
    enrichment_failed = state.enrichment_failed;
    enrichment_plans_run = state.enrichment_plans_run;
    unextracted = state.unextracted;
    // Written by the build/serve threads under the same lock; snapshot here so
    // a status read never tears them.
    last_files_cache_hit = state.last_files_cache_hit;
    last_extract_mean_ms = state.last_extract_mean_ms;
    last_memory_overlay_count = state.last_memory_overlay_count;
  }

  const auto info = build_info();
  nlohmann::json payload{
      {"pid", state.pid},
      {"engine_version", std::string{info.version}},
      {"engine_revision", std::string{info.revision}},
      {"uptime_seconds", uptime.count()},
      {"node_count", graph.nodes.size()},
      {"edge_count", graph.edges.size()},
      {"build_state", build_state_label(graph.build_state)},
      {"cache_hit_rate", graph.cache_hit_rate},
      {"enrichment_state", enrichment_state(enrichment_state_value)},
      {"enrichment_pending", enrichment_pending},
      {"enrichment_running", enrichment_running},
      {"enrichment_stale", enrichment_stale},
      {"enrichment_failed", enrichment_failed},
      {"enrichment_plans_run", enrichment_plans_run},
      {"watching", state.watching},
      {"incremental_updates", state.incremental_updates},
      {"unextracted", unextracted},
      {"ops", op_stats_json(state.op_stats)},
      {"freshness", freshness_metadata(graph)},
  };
  // Modeled cache saving = files_reused x mean(per-file extract time) from the
  // most recent build's Layer A timings. Omitted (never fabricated) when there
  // was no reuse or no per-file mean to model from.
  if (last_files_cache_hit > 0 && last_extract_mean_ms > 0.0) {
    payload["cache_saved_ms_estimate"] =
        static_cast<double>(last_files_cache_hit) * last_extract_mean_ms;
  }

  // Session-memory inventory: checkpoints in the live snapshot, sidecars on disk,
  // recall volume + miss count, recency, and the last re-overlay size. Lets an
  // operator see memory usage now that it is durable.
  std::size_t checkpoint_count = 0;
  for (const auto& node : graph.nodes) {
    if (is_memory_node_id(node.id)) {
      ++checkpoint_count;
    }
  }
  std::size_t sidecar_count = 0;
  if (!state.memory_dir.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(state.memory_dir, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(state.memory_dir, ec)) {
        if (ec) {
          break;
        }
        if (entry.path().extension() == ".json") {
          ++sidecar_count;
        }
      }
    }
  }
  payload["memory"] = {
      {"checkpoint_count", checkpoint_count},
      {"sidecar_count", sidecar_count},
      {"recall_count", state.op_stats.count[static_cast<std::size_t>(DaemonOp::Recall)]},
      {"recall_zero_hits", state.op_stats.recall_zero_hits},
      {"last_remember_at", state.last_remember_at},
      {"last_recall_at", state.last_recall_at},
      {"last_overlay_count", last_memory_overlay_count},
  };

  // Semantic connectivity: how well the host-authored layer connects to code.
  // Computed from the current snapshot so it reflects live enrichment.
  const auto connectivity = compute_semantic_connectivity(graph);
  payload["semantic"] = {
      {"doc_nodes", connectivity.doc_nodes},
      {"concept_nodes", connectivity.concept_nodes},
      {"connected_docs", connectivity.connected_docs},
      {"orphan_docs", connectivity.orphan_docs},
      {"orphan_concepts", connectivity.orphan_concepts},
      {"doc_code_edges", connectivity.doc_code_edges},
      {"connectivity_rate", connectivity.connectivity_rate},
  };
  return payload;
}

// --- Session memory: checkpoint write (remember) + recall --------------------
// Checkpoints are agent-authored notes that survive /clear (graphd is external
// to Claude's context). The body is a markdown file under memory_dir; the node
// points at it via source_file so the existing snippet machinery surfaces it.
// See graph-session-memory.

// ~4k tokens; large enough for a useful summary, capped so recall stays bounded.
constexpr std::size_t kMaxCheckpointBodyChars = 16384;

// A filesystem-safe slug: keep [a-z0-9], collapse every other run to a single
// '-'. This strips path separators and dots, so a title can never traverse out
// of memory_dir.
[[nodiscard]] std::string slugify(std::string_view title) {
  std::string slug;
  for (const char ch : title) {
    const auto uc = static_cast<unsigned char>(ch);
    if (std::isalnum(uc) != 0) {
      slug.push_back(static_cast<char>(std::tolower(uc)));
    } else if (!slug.empty() && slug.back() != '-') {
      slug.push_back('-');
    }
  }
  while (!slug.empty() && slug.back() == '-') {
    slug.pop_back();
  }
  if (slug.empty()) {
    slug = "checkpoint";
  }
  if (slug.size() > 60) {
    slug.resize(60);
  }
  return slug;
}

[[nodiscard]] nlohmann::json remember_checkpoint(DaemonState& state, const nlohmann::json& params) {
  if (state.memory_dir.empty()) {
    return error_response("session memory is not enabled on this daemon");
  }
  const auto title = params.value("title", std::string{});
  const auto body = params.value("body", std::string{});
  if (title.empty()) {
    return error_response("remember requires a non-empty title");
  }
  if (body.size() > kMaxCheckpointBodyChars) {
    return error_response("checkpoint body exceeds " + std::to_string(kMaxCheckpointBodyChars) + " chars");
  }

  // Resolve touches against the current snapshot BEFORE mutating; an unresolved
  // entry yields no edge (and is reported), never a dangling target.
  const auto graph = read_graph_snapshot(state);
  const auto by_id = index_nodes(*graph);
  std::vector<std::string> resolved;
  auto unresolved = nlohmann::json::array();
  for (const auto& touch : params.value("touches", nlohmann::json::array())) {
    if (!touch.is_string()) {
      continue;
    }
    const auto key = touch.get<std::string>();
    if (const auto* node = resolve_node(*graph, by_id, key); node != nullptr) {
      resolved.push_back(node->id);
    } else {
      unresolved.push_back(key);
    }
  }

  // Wall-clock nanos stamp the id, created_at, and recency ordering (a write
  // boundary, like the ledger flush — the running substrate stays monotonic).
  // The stamp is forced strictly increasing within the process so two checkpoints
  // in the same clock tick get distinct ids; an identical id would otherwise be
  // silently dropped by merge_fragment's first-occurrence-wins dedup.
  const auto now = std::chrono::system_clock::now();
  const auto clock_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  static std::atomic<long long> last_stamp{0};
  long long expected = last_stamp.load(std::memory_order_relaxed);
  long long stamp = 0;
  do {
    stamp = std::max<long long>(clock_ns, expected + 1);
  } while (!last_stamp.compare_exchange_weak(expected, stamp, std::memory_order_relaxed));
  const auto ts = std::to_string(stamp);
  const auto id = "memory:checkpoint:" + ts;
  const auto filename = ts + "-" + slugify(title) + ".md";

  std::error_code ec;
  std::filesystem::create_directories(state.memory_dir, ec);
  const auto path = state.memory_dir / filename;
  // Defensive sandbox: the resolved path must stay inside memory_dir.
  const auto canon_dir = std::filesystem::weakly_canonical(state.memory_dir, ec).generic_string();
  const auto canon_path = std::filesystem::weakly_canonical(path, ec).generic_string();
  if (canon_dir.empty() || canon_path.rfind(canon_dir, 0) != 0) {
    return error_response("checkpoint path escapes the memory directory");
  }
  const auto content = "# " + title + "\n\n" + body + "\n";
  {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      return error_response("failed to write checkpoint body: " + path.generic_string());
    }
    out << content;
  }
  // Span the whole body so the existing snippet machinery (read_source_snippet)
  // surfaces it on recall / graph_context. Bounded by kMaxSnippetLines downstream.
  const auto line_count = static_cast<std::uint32_t>(std::count(content.begin(), content.end(), '\n'));

  std::string tags;
  for (const auto& tag : params.value("tags", nlohmann::json::array())) {
    if (!tag.is_string()) {
      continue;
    }
    if (!tags.empty()) {
      tags += ",";
    }
    tags += tag.get<std::string>();
  }

  Node node;
  node.id = id;
  node.label = title;
  node.source_file = path.generic_string();
  node.source_location =
      SourceLocation{.start_line = 1, .start_column = 0, .end_line = std::max<std::uint32_t>(1, line_count), .end_column = 0};
  node.kind = "checkpoint";
  node.confidence = Confidence::Inferred;
  node.properties["created_at"] = ts;
  if (!tags.empty()) {
    node.properties["tags"] = tags;
  }

  Fragment fragment;
  fragment.nodes.push_back(node);
  for (const auto& target : resolved) {
    fragment.edges.push_back(
        Edge{.source = id, .target = target, .relation = "concerns", .confidence = Confidence::Inferred});
  }

  // Durable sidecar: the fragment beside the body is the source of truth for this
  // checkpoint. It is re-overlaid after every graph rebuild (see ingest_all_memory),
  // so the checkpoint survives restarts, incremental edits, and full rescans -- the
  // live snapshot node below is only the immediate, in-session copy.
  const auto sidecar = std::filesystem::path(path).replace_extension(".json");
  const auto sidecar_contents = to_json(fragment).dump(2) + '\n';
  {
    std::ofstream out(sidecar, std::ios::binary);
    if (!out) {
      return error_response("failed to write checkpoint sidecar: " + sidecar.generic_string());
    }
    out << sidecar_contents;
  }

  mutate_graph_snapshot(state, [&](GraphSnapshot& current) {
    merge_fragment(current, fragment);
    current.source_hashes[path.lexically_normal().generic_string()] = sha256_hex(content);
    current.source_hashes[sidecar.lexically_normal().generic_string()] =
        sha256_hex(sidecar_contents);
  });
  state.last_remember_at = ts;  // observability: recency of the last checkpoint write

  return ok_response({
      {"id", id},
      {"label", title},
      {"source_file", node.source_file},
      {"created_at", ts},
      {"concerns", resolved.size()},
      {"unresolved", std::move(unresolved)},
      {"written", true},
  });
}

[[nodiscard]] nlohmann::json recall_checkpoints(
    const GraphSnapshot& graph,
    const nlohmann::json& params,
    SnapshotSourceReader& source_reader) {
  const auto query = ascii_lower(params.value("query", params.value("q", std::string{})));
  const auto limit = params.value("limit", std::size_t{10});

  const auto by_id = index_nodes(graph);
  std::vector<const Node*> checkpoints;
  for (const auto& node : graph.nodes) {
    if (!is_memory_node_id(node.id) || node.kind != "checkpoint") {
      continue;
    }
    if (!query.empty()) {
      const auto tags = node.properties.find("tags");
      bool hit = contains_ci(node.label, query) ||
                 (tags != node.properties.end() && contains_ci(tags->second, query));
      // The substance of a checkpoint lives in its body (remember directs hosts to
      // write the summary there), so the filter must search it too. Read only when
      // title/tags already missed, bounded by the same caps as the returned snippet.
      if (!hit) {
        hit = contains_ci(read_source_snippet(source_reader, node).text, query);
      }
      if (!hit) {
        continue;
      }
    }
    checkpoints.push_back(&node);
  }

  // Newest-first by created_at millis (longer string = larger number; equal
  // lengths compare lexically = numerically), id as a deterministic tiebreak.
  const auto created_at = [](const Node* node) {
    const auto it = node->properties.find("created_at");
    return it == node->properties.end() ? std::string{} : it->second;
  };
  std::ranges::sort(checkpoints, [&](const Node* lhs, const Node* rhs) {
    const auto lv = created_at(lhs);
    const auto rv = created_at(rhs);
    if (lv.size() != rv.size()) {
      return lv.size() > rv.size();
    }
    if (lv != rv) {
      return lv > rv;
    }
    return lhs->id > rhs->id;
  });

  const auto total = checkpoints.size();
  if (limit > 0 && checkpoints.size() > limit) {
    checkpoints.resize(limit);
  }

  std::unordered_map<std::string, std::vector<std::string>> concerns;
  for (const auto& edge : graph.edges) {
    if (edge.relation == "concerns" && is_memory_node_id(edge.source)) {
      concerns[edge.source].push_back(edge.target);
    }
  }

  auto items = nlohmann::json::array();
  for (const auto* checkpoint : checkpoints) {
    auto entry = with_source(
        node_brief(*checkpoint), *checkpoint, source_reader);  // body snippet from source_file
    entry["created_at"] = created_at(checkpoint);
    if (const auto tags = checkpoint->properties.find("tags"); tags != checkpoint->properties.end()) {
      entry["tags"] = tags->second;
    }
    auto links = nlohmann::json::array();
    if (const auto it = concerns.find(checkpoint->id); it != concerns.end()) {
      for (const auto& target : it->second) {
        if (const auto node = by_id.find(target); node != by_id.end()) {
          links.push_back(node_brief(*node->second));
        }
      }
    }
    entry["concerns"] = std::move(links);
    items.push_back(std::move(entry));
  }
  return {{"checkpoints", std::move(items)}, {"total", total}, {"returned", items.size()}};
}

}  // namespace

nlohmann::json freshness_metadata(const GraphSnapshot& graph) {
  return {
      {"verified", is_valid_content_root(graph.content_root)},
      {"algorithm", graph.content_root.algorithm},
      {"content_root", graph.content_root.sha256},
      {"leaf_count", graph.content_root.leaf_count},
  };
}

std::shared_ptr<const GraphSnapshot> read_graph_snapshot(const DaemonState& state) {
  std::scoped_lock guard(state.snapshot_mutex);
  return state.graph_snapshot;
}

void publish_graph_snapshot(DaemonState& state, GraphSnapshot graph) {
  auto snapshot = std::make_shared<const GraphSnapshot>(std::move(graph));
  std::scoped_lock guard(state.snapshot_mutex);
  state.graph_snapshot = std::move(snapshot);
}

void mutate_graph_snapshot(DaemonState& state, const std::function<void(GraphSnapshot&)>& mutator) {
  std::scoped_lock writer_guard(state.writer_mutex);
  auto graph = *read_graph_snapshot(state);
  mutator(graph);
  publish_graph_snapshot(state, std::move(graph));
}

nlohmann::json handle_daemon_request(DaemonState& state, const nlohmann::json& request) {
  if (!protocol_version_matches(request)) {
    return error_response("protocol version mismatch");
  }
  const auto op = request.value("op", std::string{});
  auto params = request.value("params", nlohmann::json::object());
  if (!params.is_object()) {
    params = nlohmann::json::object();  // tolerate a null/absent params; ops read it with .value()
  }
  const auto graph = read_graph_snapshot(state);

  const auto known_op = daemon_op_from_string(op);
  if (!known_op) {
    return error_response("unknown op: " + op);
  }
  if (!root_pin_matches_snapshot(*known_op, params, *graph)) {
    return error_response("expected_content_root does not match the selected graph snapshot");
  }

  // Time the op at the dispatch boundary and record into op_stats. A query with
  // zero total matches is the "zero hit" signal that distinguishes useful work
  // from a daemon that is answering but finding nothing.
  double latency_ms = 0.0;
  bool zero_hit = false;
  std::string query_route;  // the query op's resolved route, for adoption telemetry
  nlohmann::json response;
  SnapshotSourceReader source_reader(graph->source_hashes, params.contains("expected_content_root"));
  {
    ScopedTimer timer(&latency_ms);
    try {
      switch (*known_op) {
        case DaemonOp::Query: {
          auto result = query_graph(*graph, params, source_reader);
          zero_hit = result.value("total", std::size_t{0}) == 0;
          query_route = result.value("route", std::string{});
          response = ok_response(decorate_freshness(annotate_build_state(std::move(result), *graph), *graph));
          break;
        }
        case DaemonOp::Path:
          response = ok_response(decorate_freshness(annotate_build_state(shortest_path(*graph, params), *graph), *graph));
          break;
        case DaemonOp::Explain:
          response = ok_response(decorate_freshness(
              annotate_build_state(explain_node(*graph, params, source_reader), *graph), *graph));
          break;
        case DaemonOp::Impact:
          response = ok_response(decorate_freshness(annotate_build_state(impact_radius(*graph, params), *graph), *graph));
          break;
        case DaemonOp::Context: {
          auto result = pack_context(*graph, params, source_reader);
          // Zero-hit for context = the focal node did not resolve (the id/query
          // matched nothing). A resolved focus is useful context even if a tight
          // budget left no neighbors room, so focal-only is NOT a zero hit.
          zero_hit = result.value("focus", nlohmann::json()).is_null();
          response = ok_response(decorate_freshness(annotate_build_state(std::move(result), *graph), *graph));
          break;
        }
        case DaemonOp::Update:
          response = state.update_handler ? ok_response(state.update_handler(params))
                                          : ok_response({{"accepted", true}});
          break;
        case DaemonOp::Status:
          response = ok_response(status(state, *graph));
          break;
        case DaemonOp::Shutdown:
          state.shutdown_requested = true;
          response = ok_response({{"shutdown", true}});
          break;
        case DaemonOp::Remember:
          // remember_checkpoint returns a full ok/error envelope (it can reject a
          // disabled daemon, oversize body, or path escape), so it is not wrapped.
          response = remember_checkpoint(state, params);
          break;
        case DaemonOp::Recall: {
          auto result = recall_checkpoints(*graph, params, source_reader);
          zero_hit = result.value("total", std::size_t{0}) == 0;
          // Recency for the status memory block: wall clock read once at this op
          // boundary (the monotonic latency substrate is untouched).
          state.last_recall_at = std::to_string(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
          response = ok_response(decorate_freshness(
              annotate_build_state(std::move(result), *graph), *graph));
          break;
        }
        case DaemonOp::Count:
          break;  // unreachable: daemon_op_from_string never yields Count
      }
    } catch (const SourceSnapshotMismatch& error) {
      // The response is assembled request-locally, so discarding it here makes
      // a failed pinned read atomic: no verified prefix or graph result escapes.
      response = error_response(error.what());
    } catch (const nlohmann::json::exception& error) {
      // A mistyped parameter (e.g. {"packing": 7}) must yield an error frame,
      // not an uncaught throw that kills the resident daemon for every caller.
      response = error_response(std::string{"invalid request parameter: "} + error.what());
    }
  }
  // A context call served with gather="adaptive" is counted distinctly so the
  // durable ledger can report adaptive adoption (pre-flip telemetry).
  const bool adaptive_context_call =
      *known_op == DaemonOp::Context && params.value("gather", std::string{}) == "adaptive";
  // A read served against the still-building empty snapshot is "not ready", not a
  // miss: record it separately and keep it out of op/zero-hit/latency accounting.
  const bool not_ready = graph->build_state == BuildState::Empty;
  state.op_stats.record(*known_op, latency_ms, zero_hit, adaptive_context_call, not_ready);
  if (!not_ready && *known_op == DaemonOp::Query) {
    state.op_stats.note_query_route(query_route);
  }
  return response;
}

}  // namespace cgraph
