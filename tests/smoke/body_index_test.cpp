// BodyIndex (CGR-25): BM25 over the source text of each symbol's span, built
// from real files on disk and the snapshot's recorded source hashes.

#include "cgraph/body_index.hpp"

#include "cgraph/file_cache.hpp"
#include "cgraph/types.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

cgraph::Node fn(const std::string& id, const fs::path& file, std::uint32_t start, std::uint32_t end) {
  return cgraph::Node{.id = id,
                      .label = id,
                      .source_file = file.generic_string(),
                      .source_location = cgraph::SourceLocation{.start_line = start, .end_line = end},
                      .kind = "function"};
}

std::string write(const fs::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary) << text;
  return text;
}

void record(cgraph::GraphSnapshot& graph, const fs::path& path, const std::string& text) {
  graph.source_hashes[path.lexically_normal().generic_string()] = cgraph::sha256_hex(text);
}

std::string top(const cgraph::BodyIndex& index, const std::string& query) {
  const auto ranked = index.rank(cgraph::stem_terms(cgraph::lexical_terms(query)), 5);
  return ranked.empty() ? std::string{} : *ranked.front().second;
}

}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / "cgraph-body-index-test";
  fs::remove_all(root);
  fs::create_directories(root);
  const auto net = root / "net.go";
  const auto db = root / "db.go";

  // Labels say nothing about the body: only the span text can connect a
  // free-text query to schedule(). The last span ends on a line with no
  // trailing newline, so an off-by-one cut would drop "flush".
  const auto net_text = write(net,
                              "package net\n"
                              "func schedule() {\n"
                              "  // retry with exponential backoff and jitter\n"
                              "  sleep(backoffJitter)\n"
                              "}\n"
                              "func dial() {\n"
                              "  openSocket(timeoutDeadline)\n"
                              "}\n"
                              "func drain() { flushWriteBuffer() }");
  const auto db_text = write(db,
                             "package db\n"
                             "func commit() {\n"
                             "  writeAheadLog.sync()\n"
                             "}\n");

  cgraph::GraphSnapshot graph;
  graph.nodes = {fn("schedule", net, 2, 5), fn("dial", net, 6, 8), fn("drain", net, 9, 9),
                 fn("commit", db, 2, 4),
                 // Prose and memory are never code bodies, and a node with no
                 // span has nothing to index.
                 cgraph::Node{.id = "doc:notes", .label = "notes", .source_file = net.generic_string(),
                              .source_location = cgraph::SourceLocation{.start_line = 1, .end_line = 9}},
                 cgraph::Node{.id = "module", .label = "module", .kind = "module"}};
  record(graph, net, net_text);
  record(graph, db, db_text);

  const auto index = cgraph::BodyIndex::refresh(graph, nullptr);
  expect(index->stats().files == 2 && index->stats().files_tokenized == 2, "both hashed files tokenized");
  expect(index->stats().documents == 4, "four located code symbols indexed (doc: and span-less excluded)");
  expect(top(*index, "requests keep retrying with backoff") == "schedule", "body vocabulary finds schedule");
  expect(top(*index, "socket deadline exceeded") == "dial", "span of the middle function is its own");
  expect(top(*index, "flush the write buffer") == "drain", "last span without trailing newline is cut whole");
  expect(top(*index, "write ahead log sync") == "commit", "second file indexed");
  expect(index->rank(cgraph::stem_terms(cgraph::lexical_terms("unrelated zebra")), 5).empty(),
         "a query sharing no body term scores nothing");

  // Republishing the same snapshot (an overlay or memory mutation) reuses the
  // index as is: no file is read again.
  expect(cgraph::BodyIndex::refresh(graph, index) == index, "unchanged snapshot returns the previous index");

  // An incremental edit to one file re-reads only that file; the other file's
  // tokenized entry is carried over.
  const auto db_edited = write(db,
                               "package db\n"
                               "func commit() {\n"
                               "  compactTombstones()\n"
                               "}\n");
  record(graph, db, db_edited);
  const auto edited = cgraph::BodyIndex::refresh(graph, index);
  expect(edited != index, "an edited file yields a new index");
  expect(edited->stats().files_tokenized == 1, "only the edited file is tokenized again");
  expect(top(*edited, "compact tombstones") == "commit", "edited body is searchable");
  expect(top(*edited, "write ahead log sync") != "commit", "the replaced body is gone");
  expect(top(*edited, "requests keep retrying with backoff") == "schedule", "reused file still searchable");

  // A span change in an unchanged file (same bytes, different extraction) is
  // not reusable: the doc must be cut again.
  graph.nodes[0].source_location->end_line = 3;
  const auto respanned = cgraph::BodyIndex::refresh(graph, edited);
  expect(respanned->stats().files_tokenized == 1, "a re-spanned file is tokenized again");
  expect(top(*respanned, "exponential backoff jitter") == "schedule", "comment line still inside the span");
  expect(respanned->rank(cgraph::stem_terms(cgraph::lexical_terms("sleep")), 5).empty(),
         "a line cut out of the span is no longer indexed");

  // Bytes on disk that no longer match the snapshot's hash are never indexed:
  // the spans were cut from different bytes.
  write(net, net_text + "\n// edited after extraction\n");
  const auto stale = cgraph::BodyIndex::refresh(graph, nullptr);
  expect(stale->stats().files == 1, "a file whose bytes changed since extraction is skipped");
  expect(top(*stale, "requests keep retrying with backoff").empty(), "skipped file contributes nothing");

  // A file that leaves the snapshot leaves the index.
  graph.nodes.erase(graph.nodes.begin(), graph.nodes.begin() + 3);
  const auto removed = cgraph::BodyIndex::refresh(graph, respanned);
  expect(removed != respanned && removed->stats().files_tokenized == 0 && removed->stats().files == 1 && removed->stats().documents == 1,
         "an index over the remaining file only");

  // A file node's document is the text no symbol encloses plus its path: a
  // test file's anonymous top-level blocks are otherwise in no document, and a
  // named helper's body must not be counted a second time in the file's.
  const auto spec = root / "widget.test.ts";
  const auto spec_text = write(spec,
                               "import { render } from './widget'\n"
                               "function mountFixture() {\n"
                               "  return renderShadowRoot(paletteSwatch)\n"
                               "}\n"
                               "test('rejects an expired session token', () => {\n"
                               "  expect(refreshCookie()).toThrow()\n"
                               "})\n");
  cgraph::GraphSnapshot files;
  files.nodes = {cgraph::Node{.id = "spec_file", .label = "ui/widget.test.ts", .source_file = spec.generic_string(),
                              .source_location = cgraph::SourceLocation{.start_line = 1, .end_line = 1},
                              .kind = "file"},
                 fn("mountFixture", spec, 2, 4)};
  record(files, spec, spec_text);
  const auto file_docs = cgraph::BodyIndex::refresh(files, nullptr);
  expect(file_docs->stats().documents == 2, "the file node and its helper each have a document");
  expect(top(*file_docs, "expired session token cookie refresh") == "spec_file",
         "top-level test block text belongs to the file's document");
  expect(top(*file_docs, "widget") == "spec_file", "path terms belong to the file's document");
  const auto swatch = file_docs->rank(cgraph::stem_terms(cgraph::lexical_terms("palette swatch shadow")), 5);
  expect(swatch.size() == 1 && *swatch.front().second == "mountFixture",
         "a symbol's body is not counted again in its file's document");

  // A snapshot without source hashes (a bare graph.json load) has nothing
  // verified to index.
  cgraph::GraphSnapshot unhashed;
  unhashed.nodes = {fn("commit", db, 2, 4)};
  expect(cgraph::BodyIndex::refresh(unhashed, nullptr)->stats().documents == 0, "no hashes, no body index");

  fs::remove_all(root);
  if (failures != 0) {
    return 1;
  }
  std::cout << "body index OK\n";
  return 0;
}
