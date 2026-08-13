#include "cgraph/file_cache.hpp"
#include "cgraph/snapshot_source_reader.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace {

void write_file(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
}

cgraph::Node source_node(
    const std::filesystem::path& path,
    std::uint32_t start_line,
    std::uint32_t end_line) {
  return cgraph::Node{
      .id = "node-" + std::to_string(start_line),
      .label = "node",
      .source_file = path.generic_string(),
      .source_location = cgraph::SourceLocation{.start_line = start_line, .end_line = end_line},
      .kind = "function",
  };
}

bool is_snapshot_mismatch(const auto& operation) {
  try {
    operation();
  } catch (const cgraph::SourceSnapshotMismatch& error) {
    const std::string message = error.what();
    return message.find("source-snapshot-mismatch") != std::string::npos &&
           message.find("synchronize again") != std::string::npos;
  }
  return false;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "cgraph-snapshot-source-reader-test";
  fs::remove_all(root);
  fs::create_directories(root / "nested");

  const auto source = root / "sample.cpp";
  const std::string original = "line one\nline two\r\nline three\nline four\n";
  const std::string changed = "line one\nchanged two\nchanged three\nline four\n";
  write_file(source, original);

  const auto normalized = source.lexically_normal().generic_string();
  std::unordered_map<std::string, std::string> hashes{{normalized, cgraph::sha256_hex(original)}};
  cgraph::SnapshotSourceReader reader(hashes, true);

  // The first slice hashes and caches the exact complete buffer. CRLF bytes are
  // preserved just as the historical binary getline path preserved '\r'.
  const auto first = reader.read_snippet(source_node(source, 2, 3), 40, 2000);
  if (first.text != "line two\r\nline three" || first.source_sha256 != cgraph::sha256_hex(original) ||
      first.truncated || reader.files_read() != 1) {
    return 1;
  }

  // A rewrite between two same-file slices cannot mix buffers within one
  // request-local reader: the second slice still comes from the verified bytes.
  write_file(source, changed);
  const auto second = reader.read_snippet(source_node(root / "nested" / ".." / "sample.cpp", 4, 4), 40, 2000);
  if (second.text != "line four" || second.source_sha256 != cgraph::sha256_hex(original) ||
      reader.files_read() != 1) {
    return 2;
  }

  // Line and character bounds are applied to the cached bytes, not a reopened
  // stream, and a line-bound slice reports truncation.
  const auto bounded = reader.read_snippet(source_node(source, 1, 4), 2, 2000);
  if (bounded.text != "line one\nline two\r" || !bounded.truncated || reader.files_read() != 1) {
    return 3;
  }

  cgraph::SnapshotSourceReader changed_reader(hashes, true);
  if (!is_snapshot_mismatch([&] { (void)changed_reader.read_snippet(source_node(source, 1, 1), 40, 2000); }) ||
      changed_reader.files_read() != 1) {
    return 4;
  }

  const std::unordered_map<std::string, std::string> no_hashes;
  cgraph::SnapshotSourceReader missing_evidence_reader(no_hashes, true);
  if (!is_snapshot_mismatch(
          [&] { (void)missing_evidence_reader.read_snippet(source_node(source, 1, 1), 40, 2000); }) ||
      missing_evidence_reader.files_read() != 0) {
    return 5;
  }

  // Unpinned compatibility remains explicitly eventual: current bytes are
  // returned with their current digest even when no snapshot evidence exists.
  cgraph::SnapshotSourceReader eventual_reader(no_hashes, false);
  const auto eventual = eventual_reader.read_snippet(source_node(source, 2, 2), 40, 2000);
  if (eventual.text != "changed two" || eventual.source_sha256 != cgraph::sha256_hex(changed) ||
      eventual_reader.files_read() != 1) {
    return 6;
  }

  fs::remove(source);
  cgraph::SnapshotSourceReader missing_file_reader(hashes, true);
  if (!is_snapshot_mismatch(
          [&] { (void)missing_file_reader.read_snippet(source_node(source, 1, 1), 40, 2000); }) ||
      missing_file_reader.files_read() != 1) {
    return 7;
  }

  fs::remove_all(root);
  return 0;
}
