#pragma once

#include "cgraph/content_root.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cgraph {

using Properties = std::unordered_map<std::string, std::string>;

// Session-memory checkpoint nodes live in the `memory:` id namespace (mirroring
// the `doc:`/`concept:` prefix convention). They are host/agent-authored notes,
// inert to code analysis and code retrieval — see graph-session-memory.
[[nodiscard]] inline bool is_memory_node_id(std::string_view id) {
  return id.starts_with("memory:");
}

// Semantic-enrichment nodes live in the host-authored `doc:`/`concept:`/`media:`/
// `topic:` id namespaces. They are prose about the code, not code symbols, so in
// code search they are ranked after structural results (they still appear -- they
// do not eclipse -- but a code query surfaces code first).
[[nodiscard]] inline bool is_enrichment_node_id(std::string_view id) {
  return id.starts_with("doc:") || id.starts_with("concept:") || id.starts_with("media:") ||
         id.starts_with("topic:");
}

enum class Confidence {
  Extracted,
  Inferred,
  Ambiguous,
};

enum class BuildState {
  Empty,
  DeterministicReady,
  Enriching,
  Idle,
  Failed,
};

struct SourceLocation {
  std::uint32_t start_line = 0;
  std::uint32_t start_column = 0;
  std::uint32_t end_line = 0;
  std::uint32_t end_column = 0;
};

struct Node {
  std::string id;
  std::string label;
  std::string source_file;
  std::optional<SourceLocation> source_location;
  std::string kind;
  Confidence confidence = Confidence::Extracted;
  std::optional<double> confidence_score;
  Properties properties;
};

struct Edge {
  std::string source;
  std::string target;
  std::string relation;
  Confidence confidence = Confidence::Extracted;
  std::optional<double> confidence_score;
  Properties properties;
};

struct Hyperedge {
  std::string id;
  std::vector<std::string> nodes;
  std::string relation;
  Confidence confidence = Confidence::Extracted;
  std::optional<double> confidence_score;
  Properties properties;
};

// A function body reduced to what survives renaming: identifiers and literals
// replaced by placeholders, then 5-token shingles hashed and winnowed (see
// fingerprint.hpp). Runtime-only -- never part of a fragment file or graph.json
// -- so Graphify parity holds; `report clones` compares these by Jaccard.
struct FunctionFingerprint {
  std::vector<std::uint64_t> shingles;  // sorted, unique winnowed shingle hashes
  std::uint32_t tokens = 0;             // normalized tokens in the body
};

struct Fragment {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<Hyperedge> hyperedges;
  std::vector<std::string> warnings;
  // Keyed by function node id; travels with the fragment through the
  // incremental index so a re-extracted file replaces its own entries.
  std::unordered_map<std::string, FunctionFingerprint> fingerprints;
};

struct GraphSnapshot {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<Hyperedge> hyperedges;
  BuildState build_state = BuildState::Empty;
  double cache_hit_rate = 0.0;
  ContentRoot content_root;
  // Runtime-only evidence for source-backed snippets. Keys are lexically
  // normalized generic source paths; values hash the exact buffers parsed by
  // extraction. Exporters intentionally ignore this ledger so graph JSON and
  // deterministic topology remain unchanged.
  std::unordered_map<std::string, std::string> source_hashes;
  // Runtime-only like source_hashes: the union of every merged fragment's
  // function fingerprints, keyed by node id. An id that dedup later removed
  // simply has no node; readers skip it.
  std::unordered_map<std::string, FunctionFingerprint> fingerprints;
};

}  // namespace cgraph
