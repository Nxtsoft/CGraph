#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgraph {

struct GraphSnapshot;

// Lowercased alphanumeric terms, breaking camelCase and snake_case and keeping
// terms of length >= 3. Mirrors the offline harness's relevance value
// (research/select.py::_terms) so the knapsack scorer matches what step A validated.
[[nodiscard]] std::unordered_set<std::string> lexical_terms(std::string_view text);

// One-shot suffix-strip stemming for seed matching, so a query inflection
// ("packing", "resolves", "imports") matches the code identifier it names
// ("pack", "resolve", "import"). Deliberately light: a single strip from a
// fixed suffix list, and the stem must keep >= 4 chars so short roots stay
// exact -- doubled-consonant gerunds ("running" -> "runn") are accepted misses
// rather than reasons for a heavier stemmer. Used for seed ranking only (label
// overlap and the body index); lexical_terms itself, the adaptive gather gate,
// and the knapsack value keep exact-match semantics. Measured on the committed
// fixture: +1.4 to +3.5 end-to-end recall points at every budget, and the share
// of rows whose grade-2 anchor is lexically reachable at all rises 0.733 -> 0.747
// (research/beyond-lexical).
[[nodiscard]] std::string stem_term(const std::string& term);
[[nodiscard]] std::unordered_set<std::string> stem_terms(const std::unordered_set<std::string>& terms);

// BM25 index over the source text of every code symbol's span (CGR-25). A
// free-text query (stack trace, review comment, issue text) rarely names a
// symbol label, but it shares vocabulary with the code that implements it:
// identifiers it calls, strings it logs, comments above it.
//
// Runtime-only, like GraphSnapshot::source_hashes: built from the snapshot's
// own nodes and the exact source bytes it extracted, never exported, never
// persisted, so Graphify parity and the fast-load index are unaffected. A file
// is tokenized only when its bytes on disk hash to the snapshot's recorded
// source hash -- a span cut from different bytes would attribute the wrong
// text to a symbol -- so a snapshot without source hashes has an empty index.
//
// Maintained per file: refresh() reuses a previous index's tokenized file
// whenever its source hash and its indexed node spans are unchanged, so an
// incremental update re-reads only the files it re-extracted, and republishing
// an unchanged snapshot (overlays, memory) returns the previous index as is.
class BodyIndex {
 public:
  struct Doc {
    std::string node_id;
    std::uint32_t length = 0;  // term occurrences in the span
    // (file-local term index, term frequency)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> term_counts;
  };
  struct File {
    std::string sha256;
    std::uint64_t span_signature = 0;  // indexed node ids + spans in this file
    std::vector<std::string> terms;   // file-local vocabulary (stemmed)
    std::vector<Doc> docs;
  };
  struct Stats {
    std::size_t files = 0;
    std::size_t files_tokenized = 0;  // read and tokenized by this refresh (rest reused)
    std::size_t documents = 0;
    std::size_t postings = 0;
    std::size_t terms = 0;
    // Files whose read or tokenization threw, as "path: reason". Such a file
    // has no documents in this index; the next refresh retries it.
    std::vector<std::string> failures;
  };

  // The index for `graph`, reusing `previous` per file where nothing changed.
  // Returns `previous` itself when every file is reused and none was removed.
  [[nodiscard]] static std::shared_ptr<const BodyIndex> refresh(
      const GraphSnapshot& graph, const std::shared_ptr<const BodyIndex>& previous);

  // Node ids ranked by BM25 score (best first, ties by id), at most `limit`.
  // `query_terms` are stemmed lexical terms of the query.
  [[nodiscard]] std::vector<std::pair<double, const std::string*>> rank(
      const std::unordered_set<std::string>& query_terms, std::size_t limit) const;

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

 private:
  struct Posting {
    std::uint32_t doc = 0;
    std::uint32_t tf = 0;
  };
  struct DocRef {
    const std::string* node_id = nullptr;
    std::uint32_t length = 0;
  };

  void aggregate();

  std::unordered_map<std::string, std::shared_ptr<const File>> files_;
  std::vector<DocRef> docs_;
  std::unordered_map<std::string, std::vector<Posting>> postings_;
  double average_length_ = 0.0;
  Stats stats_;
};

}  // namespace cgraph
