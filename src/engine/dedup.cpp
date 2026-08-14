#include "cgraph/dedup.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cgraph {
namespace {

constexpr std::size_t kMinHashCount = 16;
constexpr std::size_t kBandSize = 4;

class UnionFind {
 public:
  explicit UnionFind(std::size_t size) : parent_(size) {
    std::iota(parent_.begin(), parent_.end(), 0);
  }

  [[nodiscard]] std::size_t find(std::size_t index) {
    if (parent_[index] != index) {
      parent_[index] = find(parent_[index]);
    }
    return parent_[index];
  }

  void unite(std::size_t lhs, std::size_t rhs) {
    auto left = find(lhs);
    auto right = find(rhs);
    if (left == right) {
      return;
    }
    if (right < left) {
      std::swap(left, right);
    }
    parent_[right] = left;
  }

 private:
  std::vector<std::size_t> parent_;
};

[[nodiscard]] std::string community_of(const Node& node) {
  if (const auto iter = node.properties.find("community"); iter != node.properties.end()) {
    return iter->second;
  }
  return {};
}

[[nodiscard]] bool has_meaningful_entropy(std::string_view normalized, const DedupOptions& options) {
  return normalized.size() >= 3 && shannon_entropy(normalized) >= options.entropy_floor;
}

// True when two labels differ at exactly one position (a same-length typo such
// as "extractor"/"extractar"). The only short-label pair we still let fuzzy
// merge.
[[nodiscard]] bool single_char_substitution(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  std::size_t diffs = 0;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (lhs[index] != rhs[index] && ++diffs > 1) {
      return false;
    }
  }
  return diffs == 1;
}

// Mirrors Graphify's fuzzy-merge blockers. Jaro-Winkler's prefix bonus scores
// these pairs high, but they are almost never true duplicates:
//   * prefix-extension pairs — one label is a strict prefix of the other
//     ("messagebubble"/"messagebubbleprops", "parseconfig"/"parseconfigfile",
//     a component and its props type, a getter and its plural). Merging them
//     deletes a real node and reattaches its edges to the wrong symbol.
//   * short labels (< 12 chars) — insertions/deletions on short strings score
//     high via the prefix bonus but are abbreviations/variants, not dupes; only
//     a same-length single-char substitution (a genuine typo) is allowed.
[[nodiscard]] bool fuzzy_merge_blocked(std::string_view lhs, std::string_view rhs, double jw_score) {
  if (lhs == rhs) {
    return false;  // identical labels are governed by the same-file rule
  }
  const std::string_view shorter = lhs.size() <= rhs.size() ? lhs : rhs;
  const std::string_view longer = lhs.size() <= rhs.size() ? rhs : lhs;
  if (longer.size() > shorter.size() && longer.starts_with(shorter)) {
    return true;
  }
  if (std::max(lhs.size(), rhs.size()) < 12) {
    return !(jw_score >= 0.97 && single_char_substitution(lhs, rhs));
  }
  return false;
}

[[nodiscard]] std::vector<std::string> shingles(std::string_view normalized) {
  std::vector<std::string> values;
  if (normalized.size() <= 3) {
    values.emplace_back(normalized);
    return values;
  }

  for (std::size_t index = 0; index + 3 <= normalized.size(); ++index) {
    values.emplace_back(normalized.substr(index, 3));
  }
  return values;
}

[[nodiscard]] std::array<std::uint64_t, kMinHashCount> minhash(std::string_view normalized) {
  std::array<std::uint64_t, kMinHashCount> signature;
  signature.fill(std::numeric_limits<std::uint64_t>::max());

  const auto grams = shingles(normalized);
  for (std::size_t seed = 0; seed < signature.size(); ++seed) {
    for (const auto& gram : grams) {
      const auto value = std::hash<std::string>{}(std::to_string(seed) + ":" + gram);
      signature[seed] = std::min(signature[seed], static_cast<std::uint64_t>(value));
    }
  }
  return signature;
}

[[nodiscard]] std::vector<std::string> lsh_bands(const std::array<std::uint64_t, kMinHashCount>& signature) {
  std::vector<std::string> bands;
  for (std::size_t start = 0; start < signature.size(); start += kBandSize) {
    std::string band = std::to_string(start);
    for (std::size_t offset = 0; offset < kBandSize && start + offset < signature.size(); ++offset) {
      band += ":" + std::to_string(signature[start + offset]);
    }
    bands.push_back(std::move(band));
  }
  return bands;
}

[[nodiscard]] bool same_community(std::string_view lhs, std::string_view rhs) {
  return !lhs.empty() && lhs == rhs;
}

[[nodiscard]] std::string canonical_node_id(const std::vector<Node>& nodes, UnionFind& groups, std::size_t index) {
  const auto root = groups.find(index);
  return nodes[root].id;
}

void rewrite_edges(GraphSnapshot& graph, UnionFind& groups, const std::unordered_map<std::string, std::size_t>& index_by_id) {
  std::unordered_set<std::string> seen_edges;
  seen_edges.reserve(graph.edges.size());
  std::vector<Edge> rewritten;
  rewritten.reserve(graph.edges.size());

  for (auto edge : graph.edges) {
    if (const auto source = index_by_id.find(edge.source); source != index_by_id.end()) {
      edge.source = canonical_node_id(graph.nodes, groups, source->second);
    }
    if (const auto target = index_by_id.find(edge.target); target != index_by_id.end()) {
      edge.target = canonical_node_id(graph.nodes, groups, target->second);
    }

    const auto key = edge.source + "\n" + edge.relation + "\n" + edge.target;
    if (seen_edges.insert(key).second) {
      rewritten.push_back(std::move(edge));
    }
  }

  graph.edges = std::move(rewritten);
}

void collapse_nodes(GraphSnapshot& graph, UnionFind& groups) {
  std::vector<Node> collapsed;
  collapsed.reserve(graph.nodes.size());

  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    if (groups.find(index) == index) {
      collapsed.push_back(graph.nodes[index]);
    }
  }

  graph.nodes = std::move(collapsed);
}

void semantic_dedup_impl(
    GraphSnapshot& graph,
    const DedupOptions& options,
    const std::unordered_set<std::string>& scoped_sources,
    bool full_graph) {
  if (graph.nodes.size() < 2) {
    return;
  }

  const std::size_t node_count = graph.nodes.size();
  UnionFind groups(node_count);
  std::unordered_map<std::string, std::size_t> index_by_id;
  index_by_id.reserve(node_count);
  std::unordered_map<std::string, std::size_t> exact;
  exact.reserve(node_count);
  std::unordered_map<std::string, std::vector<std::size_t>> buckets;
  std::unordered_map<std::string, std::vector<std::size_t>> community_buckets;

  // Normalizing a label via make_id() runs three utf8proc passes; the original
  // code recomputed it once per node here and twice more per candidate pair.
  // Cache it once per node and reuse, along with each node's community, so the
  // pairwise loop reads precomputed strings instead of re-normalizing.
  std::vector<std::string> normalized_labels(node_count);
  std::vector<std::string> communities(node_count);

  const auto in_scope = [&scoped_sources, full_graph](const Node& node) {
    return full_graph || scoped_sources.contains(node.source_file);
  };

  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    index_by_id.emplace(graph.nodes[index].id, index);
    const auto& node = graph.nodes[index];
    normalized_labels[index] = make_id(node.label);
    const auto& normalized = normalized_labels[index];
    communities[index] = community_of(node);

    // A file's identity is its path, never a fuzzy label match. Two sibling
    // files with similar names ("compiq-viewer.tsx" / "compiq-viewer-states.tsx")
    // must never collapse — doing so deletes one file node and destroys the
    // import/contains edges that referenced it. File nodes never participate in
    // dedup.
    if (node.kind == "file") {
      continue;
    }

    // Pass 1: exact-label merge, restricted to one source file AND one
    // declaration site. Two files that declare the same name ("Props", "index",
    // "handler") are distinct symbols and must never collapse, so the dedup key
    // carries the source file. Nodes with no source file cannot be proven to be
    // the same symbol, so they are left untouched. This runs for every node, not
    // just high-entropy ones.
    //
    // The key also carries the start line, because a label names a symbol and one
    // file can legitimately declare a name more than once: an overload set
    // (`to_json` five times over), a constructor sharing its class's name, or
    // `operator=` for both copy and move. Those are distinct symbols at distinct
    // sites, and collapsing them deletes real code from the graph -- an agent
    // asking where `to_json` lives would get one of five answers with no hint the
    // others exist. A genuine double-extraction of the same symbol still shares a
    // line and still merges. (Before labels were reduced to bare names an
    // overload set was accidentally kept apart by its signature; the file-only
    // key silently depended on that.)
    if (!node.source_file.empty() && (full_graph || in_scope(node))) {
      // Line AND column: three overloads can share one line, and a line-only key
      // would collapse them again after add_symbol_node went to the trouble of
      // giving each a distinct id.
      const auto site = node.source_location ? std::to_string(node.source_location->start_line) + ":" +
                                                   std::to_string(node.source_location->start_column)
                                             : std::string{};
      const auto exact_key = normalized + "\n" + node.source_file + "\n" + site;
      if (const auto existing = exact.find(exact_key); existing != exact.end()) {
        groups.unite(existing->second, index);
      } else {
        exact.emplace(exact_key, index);
      }
    }

    // Pass 2 (fuzzy) only considers high-entropy labels.
    if (!has_meaningful_entropy(normalized, options)) {
      continue;
    }

    for (const auto& band : lsh_bands(minhash(normalized))) {
      buckets[band].push_back(index);
    }

    if (const auto& community = communities[index]; !community.empty()) {
      community_buckets[community].push_back(index);
    }
  }

  for (auto& [community, bucket] : community_buckets) {
    buckets["community:" + community].insert(buckets["community:" + community].end(), bucket.begin(), bucket.end());
  }

  // Pairs land in multiple bands, so a pair can be visited many times. Key the
  // dedup set on the two node indices packed into one 64-bit integer instead of
  // formatting a string per visit (node counts stay well under 2^32).
  std::unordered_set<std::uint64_t> compared;
  for (const auto& [_, bucket] : buckets) {
    for (std::size_t left_pos = 0; left_pos < bucket.size(); ++left_pos) {
      for (std::size_t right_pos = left_pos + 1; right_pos < bucket.size(); ++right_pos) {
        const auto left = bucket[left_pos];
        const auto right = bucket[right_pos];
        if (!full_graph && !in_scope(graph.nodes[left]) && !in_scope(graph.nodes[right])) {
          continue;
        }

        const auto key =
            (static_cast<std::uint64_t>(std::min(left, right)) << 32) | static_cast<std::uint64_t>(std::max(left, right));
        if (!compared.insert(key).second) {
          continue;
        }

        const auto& left_label = normalized_labels[left];
        const auto& right_label = normalized_labels[right];
        const auto threshold =
            same_community(communities[left], communities[right]) ? options.same_community_threshold : options.jaro_winkler_threshold;
        const auto similarity = jaro_winkler_similarity(left_label, right_label);
        if (similarity < threshold) {
          continue;
        }
        if (fuzzy_merge_blocked(left_label, right_label, similarity)) {
          continue;
        }
        const auto& left_node = graph.nodes[left];
        const auto& right_node = graph.nodes[right];
        const bool left_sited = left_node.source_location.has_value();
        const bool right_sited = right_node.source_location.has_value();
        // The exact pass only sees nodes with a non-empty source_file, so that flag
        // -- not the presence of a location -- is what decides which pass owns a
        // node's identity.
        const bool left_from_source = !left_node.source_file.empty();
        const bool right_from_source = !right_node.source_file.empty();

        // A node extracted from a file and one that was not are different species:
        // a code symbol and an enrichment concept. Never fuzzy-merge across that
        // line -- unite() keeps the lower index and enrichment nodes are appended
        // last, so such a merge silently deletes the concept.
        if (left_from_source != right_from_source) {
          continue;
        }

        // Identical labels, when both nodes came from a file: the exact pass owns
        // that decision, keying on source file and declaration site. Two such nodes
        // are the same symbol only if they share both, so the fuzzy pass -- which
        // has no evidence the exact pass lacks -- must not unite them. That covers
        // same-named-but-distinct symbols in different files and, since labels
        // became bare names, overload sets within one file.
        //
        // Nodes with NO source_file are the opposite case: they never enter the
        // exact pass, so the fuzzy pass is their only merge path. Two such nodes
        // with one label really are one idea, and refusing them would mean they
        // could never be deduplicated again.
        //
        // In practice that is `concept` only. `document` and `media` DO carry a
        // source_file (measured: 230/230 and 4/4 on this repo's enriched graph) --
        // integrations/skills/cgraph-enrich/SKILL.md tells hosts to set it -- they
        // just carry no source_location. So they take the `left_from_source` branch
        // above, and identical-label documents are correctly kept apart.
        if (left_label == right_label && left_from_source) {
          continue;
        }
        // An enrichment node's identity includes the file it was written from. A
        // change's proposal, its tasks, and its design are three documents about
        // one subject: labels similar enough to cross the threshold, a source_file
        // each, and no source_location -- so neither the identical-label guard
        // above nor the declaration-site guard below can protect them (measured on
        // this repo's enriched graph: 44 documents and 144 edges deleted per
        // build). Code symbols must keep merging across files -- the cross-file
        // "PaymentService"/"Payment Service" case is Graphify parity -- so the
        // rule is scoped to the enrichment kinds that carry a file: `document` and
        // `media`. `concept` records no source_file and keeps label-only identity.
        const auto file_scoped_kind = [](const Node& node) {
          return node.kind == "document" || node.kind == "media";
        };
        if ((file_scoped_kind(left_node) || file_scoped_kind(right_node)) &&
            left_node.source_file != right_node.source_file) {
          continue;
        }
        // Two nodes that each name a concrete declaration site are distinct
        // symbols, and a similar label is not evidence otherwise. Fuzzy matching
        // earns its keep on nodes with no site -- an enrichment concept where
        // "Auth flow" and "Authentication flow" really are one idea -- not on
        // code, where `validate_semantic_fragment_file` and
        // `validate_semantic_fragment_json` differ by one word and are different
        // functions.
        //
        // This guard was latent before labels became bare names: a
        // signature-bearing label was long and distinctive enough that two
        // neighbours rarely crossed the 0.92 threshold. Shortening labels removed
        // that accidental protection and the pass began deleting real functions
        // (measured on this repo: supervisor_sync into supervisor_spec,
        // drainer_uninstall into drainer_installed, query_zero_hit_rate into
        // query_zero_hits).
        if (left_sited && right_sited &&
            (left_node.source_file != right_node.source_file ||
             left_node.source_location->start_line != right_node.source_location->start_line ||
             left_node.source_location->start_column != right_node.source_location->start_column)) {
          continue;
        }
        groups.unite(left, right);
      }
    }
  }

  rewrite_edges(graph, groups, index_by_id);
  collapse_nodes(graph, groups);
}

}  // namespace

double shannon_entropy(std::string_view value) {
  if (value.empty()) {
    return 0.0;
  }

  std::unordered_map<char, double> counts;
  for (const auto ch : value) {
    counts[ch] += 1.0;
  }

  double entropy = 0.0;
  for (const auto& [_, count] : counts) {
    const auto probability = count / static_cast<double>(value.size());
    entropy -= probability * std::log2(probability);
  }
  return entropy;
}

double jaro_winkler_similarity(std::string_view lhs, std::string_view rhs) {
  if (lhs == rhs) {
    return 1.0;
  }
  if (lhs.empty() || rhs.empty()) {
    return 0.0;
  }

  const auto match_distance = std::max(lhs.size(), rhs.size()) / 2;
  const auto window = match_distance > 0 ? match_distance - 1 : 0;

  std::vector<bool> lhs_matches(lhs.size(), false);
  std::vector<bool> rhs_matches(rhs.size(), false);

  std::size_t matches = 0;
  for (std::size_t left = 0; left < lhs.size(); ++left) {
    const auto start = left > window ? left - window : 0;
    const auto end = std::min(left + window + 1, rhs.size());
    for (std::size_t right = start; right < end; ++right) {
      if (rhs_matches[right] || lhs[left] != rhs[right]) {
        continue;
      }
      lhs_matches[left] = true;
      rhs_matches[right] = true;
      ++matches;
      break;
    }
  }

  if (matches == 0) {
    return 0.0;
  }

  std::size_t rhs_index = 0;
  double transpositions = 0.0;
  for (std::size_t lhs_index = 0; lhs_index < lhs.size(); ++lhs_index) {
    if (!lhs_matches[lhs_index]) {
      continue;
    }
    while (!rhs_matches[rhs_index]) {
      ++rhs_index;
    }
    if (lhs[lhs_index] != rhs[rhs_index]) {
      transpositions += 1.0;
    }
    ++rhs_index;
  }
  transpositions /= 2.0;

  const auto match_count = static_cast<double>(matches);
  const auto jaro =
      (match_count / static_cast<double>(lhs.size()) +
       match_count / static_cast<double>(rhs.size()) +
       (match_count - transpositions) / match_count) /
      3.0;

  std::size_t prefix = 0;
  while (prefix < std::min<std::size_t>({4, lhs.size(), rhs.size()}) && lhs[prefix] == rhs[prefix]) {
    ++prefix;
  }

  return jaro + static_cast<double>(prefix) * 0.1 * (1.0 - jaro);
}

void semantic_dedup(GraphSnapshot& graph, const DedupOptions& options) {
  semantic_dedup_impl(graph, options, {}, true);
}

void semantic_dedup_neighborhood(
    GraphSnapshot& graph,
    std::span<const std::string> changed_source_files,
    const DedupOptions& options) {
  std::unordered_set<std::string> scoped_sources(changed_source_files.begin(), changed_source_files.end());
  if (scoped_sources.empty()) {
    return;
  }
  semantic_dedup_impl(graph, options, scoped_sources, false);
}

}  // namespace cgraph
