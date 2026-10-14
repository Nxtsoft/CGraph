#include "cgraph/body_index.hpp"

#include "cgraph/file_cache.hpp"
#include "cgraph/incremental_update.hpp"
#include "cgraph/types.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <system_error>
#include <thread>

namespace cgraph {
namespace {

// Same whole-file bound as extraction and SnapshotSourceReader: a file larger
// than this was never extracted, so it has no spans to index.
constexpr std::size_t kMaxIndexedFileBytes = 8 * 1024 * 1024;

// Standard Okapi BM25 parameters.
constexpr double kBm25K1 = 1.2;
constexpr double kBm25B = 0.75;

// Calls `emit(word)` for every maximal alphanumeric run in `text`.
template <typename Emit>
void for_each_word(std::string_view text, Emit&& emit) {
  std::size_t begin = 0;
  while (begin < text.size()) {
    while (begin < text.size() && std::isalnum(static_cast<unsigned char>(text[begin])) == 0) {
      ++begin;
    }
    auto end = begin;
    while (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end])) != 0) {
      ++end;
    }
    if (end > begin) {
      emit(text.substr(begin, end - begin));
    }
    begin = end;
  }
}

// Calls `emit(term)` for each lowercased camelCase/snake_case piece of one
// alphanumeric word that is at least three characters long.
template <typename Emit>
void split_word(std::string_view word, Emit&& emit) {
  std::string sub;
  const auto push_sub = [&]() {
    if (sub.size() >= 3) {
      emit(sub);
    }
    sub.clear();
  };
  for (std::size_t i = 0; i < word.size(); ++i) {
    const auto ch = static_cast<unsigned char>(word[i]);
    const bool upper = std::isupper(ch) != 0;
    const bool prev_upper = i > 0 && std::isupper(static_cast<unsigned char>(word[i - 1])) != 0;
    const bool next_lower = i + 1 < word.size() && std::islower(static_cast<unsigned char>(word[i + 1])) != 0;
    if (i > 0 && upper && (!prev_upper || next_lower)) {
      push_sub();
    }
    sub.push_back(static_cast<char>(std::tolower(ch)));
  }
  push_sub();
}

// Heterogeneous lookup so a word is looked up straight out of the file buffer.
struct WordHash {
  using is_transparent = void;
  [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept {
    return std::hash<std::string_view>{}(text);
  }
};

[[nodiscard]] bool indexable(const Node& node) {
  return !node.source_file.empty() && node.source_location && node.source_location->start_line > 0 &&
         !is_memory_node_id(node.id) && !is_enrichment_node_id(node.id);
}

// FNV-1a over the indexed nodes' ids and spans, in graph order: a file entry is
// reusable only when the snapshot still cuts the same spans from it.
void mix(std::uint64_t& hash, std::string_view bytes) {
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ULL;
  }
  hash ^= 0xffU;
  hash *= 1099511628211ULL;
}

void mix(std::uint64_t& hash, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    hash ^= (value >> shift) & 0xffU;
    hash *= 1099511628211ULL;
  }
}

// The file's bytes, or nullopt when it is unreadable, over the bound, or no
// longer the bytes the snapshot extracted.
[[nodiscard]] std::optional<std::string> read_extracted_source(const std::string& path,
                                                               const std::string& expected_sha256) {
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error || size > kMaxIndexedFileBytes) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (static_cast<std::size_t>(input.gcount()) != contents.size() || input.peek() != EOF) {
    return std::nullopt;  // the file changed size under us
  }
  if (sha256_hex(contents) != expected_sha256) {
    return std::nullopt;
  }
  return contents;
}

// Tokenizes the file once -- every term occurrence with its line -- and cuts
// each node's document out of that stream by line range. Spans nest (a class
// holds its methods), so tokenizing per span would re-split the same text once
// per enclosing symbol; and each distinct raw term is stemmed once per file.
[[nodiscard]] BodyIndex::File tokenize_file(const std::string& contents,
                                            const std::vector<const Node*>& nodes,
                                            std::string sha256,
                                            std::uint64_t span_signature) {
  BodyIndex::File file;
  file.sha256 = std::move(sha256);
  file.span_signature = span_signature;

  // Words repeat heavily in code, so each distinct word is split and stemmed
  // once per file and its term ids are reused for every later occurrence.
  // Each word's term ids live in one flat vector as an (offset, count) range,
  // so a new word costs no allocation beyond its map key.
  std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>, WordHash, std::equal_to<>> word_terms;
  std::vector<std::uint32_t> word_term_ids;
  std::unordered_map<std::string, std::uint32_t> vocabulary;
  std::vector<std::uint32_t> stream;          // term id per occurrence, in file order
  std::vector<std::size_t> line_first_token;  // stream offset where each line starts
  std::size_t offset = 0;
  while (offset <= contents.size()) {
    const auto newline = contents.find('\n', offset);
    const auto line_end = newline == std::string::npos ? contents.size() : newline;
    line_first_token.push_back(stream.size());
    for_each_word(std::string_view{contents.data() + offset, line_end - offset}, [&](std::string_view word) {
      auto found = word_terms.find(word);
      if (found == word_terms.end()) {
        const auto first = static_cast<std::uint32_t>(word_term_ids.size());
        split_word(word, [&](const std::string& raw) {
          const auto [entry, inserted] =
              vocabulary.try_emplace(stem_term(raw), static_cast<std::uint32_t>(file.terms.size()));
          if (inserted) {
            file.terms.push_back(entry->first);
          }
          word_term_ids.push_back(entry->second);
        });
        found = word_terms
                    .emplace(std::string(word),
                             std::pair{first, static_cast<std::uint32_t>(word_term_ids.size()) - first})
                    .first;
      }
      const auto [first, count] = found->second;
      stream.insert(stream.end(), word_term_ids.begin() + first, word_term_ids.begin() + first + count);
    });
    if (newline == std::string::npos) {
      break;
    }
    offset = newline + 1;
  }
  const auto line_count = static_cast<std::uint32_t>(line_first_token.size());
  line_first_token.push_back(stream.size());

  std::vector<std::uint32_t> counts(file.terms.size(), 0);
  std::vector<std::uint32_t> touched;
  file.docs.reserve(nodes.size());
  for (const auto* node : nodes) {
    const auto start = node->source_location->start_line;
    const auto end = std::min(std::max(start, node->source_location->end_line), line_count);
    if (start > line_count) {
      continue;  // a span past the end of the bytes it was cut from has no text
    }
    BodyIndex::Doc doc;
    doc.node_id = node->id;
    for (auto i = line_first_token[start - 1]; i < line_first_token[end]; ++i) {
      const auto term = stream[i];
      if (counts[term]++ == 0) {
        touched.push_back(term);
      }
      ++doc.length;
    }
    if (doc.length == 0) {
      continue;
    }
    doc.term_counts.reserve(touched.size());
    for (const auto term : touched) {
      doc.term_counts.emplace_back(term, counts[term]);
      counts[term] = 0;
    }
    touched.clear();
    file.docs.push_back(std::move(doc));
  }
  return file;
}

}  // namespace

std::unordered_set<std::string> lexical_terms(std::string_view text) {
  std::unordered_set<std::string> terms;
  for_each_word(text, [&](std::string_view word) {
    split_word(word, [&](const std::string& term) { terms.insert(term); });
  });
  return terms;
}

std::string stem_term(const std::string& term) {
  static constexpr std::string_view kSuffixes[] = {"ing", "tion", "sion", "ers", "ies",
                                                   "ed",  "es",   "al",   "er",  "s"};
  for (const auto suffix : kSuffixes) {
    if (term.size() >= suffix.size() + 4 && term.ends_with(suffix)) {
      return term.substr(0, term.size() - suffix.size());
    }
  }
  return term;
}

std::unordered_set<std::string> stem_terms(const std::unordered_set<std::string>& terms) {
  std::unordered_set<std::string> out;
  out.reserve(terms.size());
  for (const auto& term : terms) {
    out.insert(stem_term(term));
  }
  return out;
}

std::shared_ptr<const BodyIndex> BodyIndex::refresh(const GraphSnapshot& graph,
                                                    const std::shared_ptr<const BodyIndex>& previous) {
  // Indexed nodes grouped by the normalized path the snapshot hashed.
  std::unordered_map<std::string, std::vector<const Node*>> by_file;
  for (const auto& node : graph.nodes) {
    if (indexable(node)) {
      by_file[incremental_file_key(node.source_file)].push_back(&node);
    }
  }

  struct Pending {
    const std::string* key = nullptr;
    const std::vector<const Node*>* nodes = nullptr;
    const std::string* sha256 = nullptr;
    std::uint64_t signature = 0;
    std::shared_ptr<const File> file;  // null when the bytes no longer match or loading failed
    std::string failure;
  };
  auto index = std::make_shared<BodyIndex>();
  std::vector<Pending> pending;
  for (const auto& [key, nodes] : by_file) {
    const auto expected = graph.source_hashes.find(key);
    if (expected == graph.source_hashes.end()) {
      continue;  // no extracted bytes to cut spans from
    }
    std::uint64_t signature = 14695981039346656037ULL;
    for (const auto* node : nodes) {
      mix(signature, node->id);
      mix(signature, node->source_location->start_line);
      mix(signature, node->source_location->end_line);
    }
    if (previous != nullptr) {
      const auto old = previous->files_.find(key);
      if (old != previous->files_.end() && old->second->sha256 == expected->second &&
          old->second->span_signature == signature) {
        index->files_.emplace(key, old->second);
        continue;
      }
    }
    pending.push_back(Pending{&key, &nodes, &expected->second, signature, nullptr, {}});
  }
  if (previous != nullptr && pending.empty() && index->files_.size() == previous->files_.size()) {
    return previous;
  }

  // Read, verify, and tokenize the changed files concurrently, one worker per
  // core as extract_files does; each worker writes only its own slot. A file
  // is read at the path the snapshot extracted it from -- the key is only its
  // normalized form.
  // A throw (allocation failure, a filesystem error) must not escape a worker
  // thread -- that terminates the daemon. It skips the file and is reported in
  // stats().failures, as extract_detected_file reports a failed extraction.
  const auto load = [&](Pending& item) {
    try {
      const auto contents = read_extracted_source(item.nodes->front()->source_file, *item.sha256);
      if (contents) {
        item.file = std::make_shared<const File>(tokenize_file(*contents, *item.nodes, *item.sha256, item.signature));
      }
    } catch (const std::exception& error) {
      item.failure = *item.key + ": " + error.what();
    } catch (...) {
      item.failure = *item.key + ": unknown error";
    }
  };
  const unsigned hardware = std::thread::hardware_concurrency();
  const auto worker_count = std::min<std::size_t>(hardware == 0 ? 1 : hardware, pending.size());
  std::atomic<std::size_t> next{0};
  const auto worker = [&] {
    for (auto i = next.fetch_add(1, std::memory_order_relaxed); i < pending.size();
         i = next.fetch_add(1, std::memory_order_relaxed)) {
      load(pending[i]);
    }
  };
  std::vector<std::thread> workers;
  for (std::size_t i = 1; i < worker_count; ++i) {
    workers.emplace_back(worker);
  }
  worker();  // the calling thread participates too
  for (auto& thread : workers) {
    thread.join();
  }
  for (auto& item : pending) {
    if (!item.failure.empty()) {
      index->stats_.failures.push_back(std::move(item.failure));
    } else if (item.file != nullptr) {  // else changed on disk since extraction; the next publish re-reads it
      index->files_.emplace(*item.key, std::move(item.file));
      ++index->stats_.files_tokenized;
    }
  }
  index->aggregate();
  return index;
}

void BodyIndex::aggregate() {
  stats_.files = files_.size();
  std::size_t total_length = 0;
  for (const auto& [_, file] : files_) {
    for (const auto& doc : file->docs) {
      const auto doc_index = static_cast<std::uint32_t>(docs_.size());
      docs_.push_back(DocRef{&doc.node_id, doc.length});
      total_length += doc.length;
      for (const auto& [term, tf] : doc.term_counts) {
        postings_[file->terms[term]].push_back(Posting{doc_index, tf});
      }
      stats_.postings += doc.term_counts.size();
    }
  }
  stats_.documents = docs_.size();
  stats_.terms = postings_.size();
  average_length_ = docs_.empty() ? 0.0 : static_cast<double>(total_length) / static_cast<double>(docs_.size());
}

std::vector<std::pair<double, const std::string*>> BodyIndex::rank(
    const std::unordered_set<std::string>& query_terms, std::size_t limit) const {
  std::vector<std::pair<double, const std::string*>> ranked;
  if (docs_.empty() || limit == 0) {
    return ranked;
  }
  const auto doc_count = static_cast<double>(docs_.size());
  std::vector<double> scores(docs_.size(), 0.0);
  std::vector<std::uint32_t> touched;
  for (const auto& term : query_terms) {
    const auto found = postings_.find(term);
    if (found == postings_.end()) {
      continue;
    }
    const auto df = static_cast<double>(found->second.size());
    const double idf = std::log(1.0 + (doc_count - df + 0.5) / (df + 0.5));
    for (const auto& posting : found->second) {
      const auto tf = static_cast<double>(posting.tf);
      const double norm = 1.0 - kBm25B + kBm25B * static_cast<double>(docs_[posting.doc].length) / average_length_;
      if (scores[posting.doc] == 0.0) {
        touched.push_back(posting.doc);
      }
      scores[posting.doc] += idf * tf * (kBm25K1 + 1.0) / (tf + kBm25K1 * norm);
    }
  }
  ranked.reserve(touched.size());
  for (const auto doc : touched) {
    ranked.emplace_back(scores[doc], docs_[doc].node_id);
  }
  const auto by_score = [](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) {
      return lhs.first > rhs.first;
    }
    return *lhs.second < *rhs.second;
  };
  if (ranked.size() > limit) {
    std::ranges::partial_sort(ranked, ranked.begin() + static_cast<std::ptrdiff_t>(limit), by_score);
    ranked.resize(limit);
  } else {
    std::ranges::sort(ranked, by_score);
  }
  return ranked;
}

}  // namespace cgraph
