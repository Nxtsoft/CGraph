#pragma once

#include "cgraph/types.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace cgraph {

struct SnapshotSourceSnippet {
  std::string text;
  std::string source_sha256;
  bool truncated = false;
};

class SourceSnapshotMismatch final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Request-local source access for snippet-producing daemon reads. Every distinct
// normalized source path is opened at most once; snippets and their digest are
// derived from that one cached byte buffer.
class SnapshotSourceReader {
 public:
  SnapshotSourceReader(
      const std::unordered_map<std::string, std::string>& source_hashes,
      bool pinned);

  [[nodiscard]] SnapshotSourceSnippet read_snippet(
      const Node& node,
      std::size_t max_lines,
      std::size_t max_chars);
  [[nodiscard]] std::size_t files_read() const noexcept;

 private:
  struct CachedSource {
    std::string contents;
    std::string source_sha256;
    std::string failure;
    bool available = false;
  };

  [[nodiscard]] CachedSource& read_source(const std::string& normalized_path);
  [[nodiscard]] const std::string* expected_hash(const std::string& normalized_path) const;
  [[noreturn]] void throw_mismatch(const std::string& normalized_path, std::string reason) const;

  const std::unordered_map<std::string, std::string>& source_hashes_;
  bool pinned_ = false;
  std::size_t files_read_ = 0;
  std::unordered_map<std::string, CachedSource> sources_;
};

}  // namespace cgraph
