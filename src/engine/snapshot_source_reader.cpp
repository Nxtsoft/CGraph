#include "cgraph/snapshot_source_reader.hpp"

#include "cgraph/file_cache.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace cgraph {
namespace {

// Keep snippet verification within the same whole-file bound as deterministic
// extraction. The post-open check also covers growth after the advisory size
// query, so the cached buffer can never exceed this bound.
constexpr std::size_t kMaxSourceFileBytes = 8 * 1024 * 1024;

[[nodiscard]] std::string normalized_source_path(std::string_view path) {
  return std::filesystem::path{path}.lexically_normal().generic_string();
}

}  // namespace

SnapshotSourceReader::SnapshotSourceReader(
    const std::unordered_map<std::string, std::string>& source_hashes,
    bool pinned)
    : source_hashes_(source_hashes), pinned_(pinned) {}

SnapshotSourceSnippet SnapshotSourceReader::read_snippet(
    const Node& node,
    std::size_t max_lines,
    std::size_t max_chars) {
  SnapshotSourceSnippet result;
  if (node.source_file.empty() || !node.source_location || node.source_location->start_line == 0) {
    return result;
  }

  const auto path = normalized_source_path(node.source_file);
  const auto* expected = expected_hash(path);
  if (pinned_ && expected == nullptr) {
    throw_mismatch(path, "the selected snapshot has no source hash");
  }

  auto& source = read_source(path);
  if (!source.available) {
    if (pinned_) {
      throw_mismatch(path, source.failure);
    }
    return result;
  }
  if (pinned_ && source.source_sha256 != *expected) {
    throw_mismatch(path, "the source bytes changed after synchronization");
  }

  result.source_sha256 = source.source_sha256;
  if (max_lines == 0 || max_chars == 0 || source.contents.empty()) {
    result.truncated = max_lines == 0 || max_chars == 0;
    return result;
  }

  const auto start = node.source_location->start_line;
  const auto end = std::max(start, node.source_location->end_line);
  const auto capped_line_count = std::min<std::size_t>(
      max_lines,
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
  const auto max_last = static_cast<std::uint64_t>(start) + capped_line_count - 1;
  const auto last = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      end,
      std::min<std::uint64_t>(max_last, std::numeric_limits<std::uint32_t>::max())));

  std::size_t offset = 0;
  std::uint32_t current = 0;
  while (offset < source.contents.size()) {
    const auto newline = source.contents.find('\n', offset);
    const auto line_end = newline == std::string::npos ? source.contents.size() : newline;
    const std::string_view line{source.contents.data() + offset, line_end - offset};
    ++current;
    if (current >= start) {
      if (current > last) {
        break;
      }
      // Preserve the historical bound exactly: reserve one character for the
      // separator even for the first selected line.
      if (result.text.size() + line.size() + 1 > max_chars) {
        result.truncated = true;
        break;
      }
      if (!result.text.empty()) {
        result.text.push_back('\n');
      }
      result.text.append(line);
    }
    if (newline == std::string::npos) {
      break;
    }
    offset = newline + 1;
  }
  if (end > last) {
    result.truncated = true;
  }
  return result;
}

std::size_t SnapshotSourceReader::files_read() const noexcept {
  return files_read_;
}

SnapshotSourceReader::CachedSource& SnapshotSourceReader::read_source(
    const std::string& normalized_path) {
  if (const auto cached = sources_.find(normalized_path); cached != sources_.end()) {
    return cached->second;
  }

  auto [entry, _] = sources_.try_emplace(normalized_path);
  auto& source = entry->second;
  ++files_read_;

  std::error_code size_error;
  const auto file_size = std::filesystem::file_size(normalized_path, size_error);
  if (!size_error && file_size > kMaxSourceFileBytes) {
    source.failure = "the source file exceeds the extraction size limit";
    return source;
  }

  std::ifstream input(normalized_path, std::ios::binary);
  if (!input) {
    source.failure = "the source file cannot be read";
    return source;
  }
  if (!size_error) {
    source.contents.reserve(static_cast<std::size_t>(file_size));
  }

  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0) {
      break;
    }
    const auto chunk_size = static_cast<std::size_t>(count);
    if (source.contents.size() + chunk_size > kMaxSourceFileBytes) {
      source.contents.clear();
      source.failure = "the source file exceeds the extraction size limit";
      return source;
    }
    source.contents.append(buffer.data(), chunk_size);
  }
  if (input.bad()) {
    source.contents.clear();
    source.failure = "the source file cannot be read completely";
    return source;
  }

  source.source_sha256 = sha256_hex(source.contents);
  source.available = true;
  return source;
}

const std::string* SnapshotSourceReader::expected_hash(const std::string& normalized_path) const {
  const auto expected = source_hashes_.find(normalized_path);
  return expected == source_hashes_.end() ? nullptr : &expected->second;
}

void SnapshotSourceReader::throw_mismatch(
    const std::string& normalized_path,
    std::string reason) const {
  throw SourceSnapshotMismatch(
      "source-snapshot-mismatch: " + std::move(reason) + " for '" + normalized_path +
      "'; synchronize again");
}

}  // namespace cgraph
