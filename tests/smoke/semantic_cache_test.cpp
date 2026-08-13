#include "cgraph/semantic_cache.hpp"

#include "cgraph/file_cache.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

void expect(bool& ok, bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

void expect_empty_cache_no_throw(
    bool& ok,
    const std::filesystem::path& root,
    const std::string& name,
    const std::string& contents) {
  const auto path = root / ("cache-malformed-" + name + ".json");
  write_file(path, contents);
  try {
    const auto loaded = cgraph::read_semantic_cache(path);
    if (loaded.size() != 0) {
      std::cerr << "FAIL: malformed cache field was reused: " << name << '\n';
      ok = false;
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL: malformed cache field threw: " << name << ": "
              << error.what() << '\n';
    ok = false;
  } catch (...) {
    std::cerr << "FAIL: malformed cache field threw a non-standard exception: "
              << name << '\n';
    ok = false;
  }
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph-semantic-cache-v2-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto doc_a = root / "docs" / "a.md";
  const auto doc_b = root / "docs" / "b.md";
  const auto fragment = root / "semantic-drop" / "chunk_01.json";
  write_file(doc_a, "# Doc A\nContent A\n");
  write_file(doc_b, "# Doc A\nContent A\n");  // same content as doc_a
  write_file(fragment, R"({"nodes":[],"edges":[]})");

  const auto hash_a = cgraph::sha256_file_hex(doc_a);
  const auto hash_b = cgraph::sha256_file_hex(doc_b);
  const auto frag_hash = cgraph::sha256_file_hex(fragment);

  bool ok = true;

  // Composite identity: same content, different paths -> separate records
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec_a;
    rec_a.source_path = doc_a;
    rec_a.content_hash = hash_a;
    rec_a.fragment_path = fragment;
    rec_a.fragment_hash = frag_hash;
    rec_a.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(rec_a);

    cgraph::SemanticCacheRecord rec_b;
    rec_b.source_path = doc_b;
    rec_b.content_hash = hash_b;
    rec_b.fragment_path = fragment;
    rec_b.fragment_hash = frag_hash;
    rec_b.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(rec_b);

    expect(ok, cache.size() == 2, "duplicate-content sources are separate records");
    const auto found_a = cache.find_for_source(doc_a);
    const auto found_b = cache.find_for_source(doc_b);
    expect(ok, found_a.has_value() && found_a->source_path == std::filesystem::weakly_canonical(doc_a),
           "find_for_source returns normalized record for a");
    expect(ok, found_b.has_value() && found_b->source_path == std::filesystem::weakly_canonical(doc_b),
           "find_for_source returns normalized record for b");
    expect(ok, cache.find_for_source(doc_a, hash_a).has_value(),
           "composite lookup matches source path and content hash");
    expect(ok, !cache.find_for_source(doc_a, "different-hash").has_value(),
           "composite lookup rejects an old content hash for the same path");
  }

  // Fragment hash persists and round-trips
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = fragment;
    rec.fragment_hash = frag_hash;
    rec.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(rec);

    const auto cache_path = root / "cache-frag-hash.json";
    cgraph::write_semantic_cache(cache, cache_path);
    const auto loaded = cgraph::read_semantic_cache(cache_path);
    const auto loaded_rec = loaded.find_for_source(doc_a);
    expect(ok, loaded_rec.has_value(), "round-trip: record loaded");
    expect(ok, loaded_rec->fragment_hash == frag_hash, "round-trip: fragment_hash preserved");
    expect(ok, loaded_rec->content_hash == hash_a, "round-trip: content_hash preserved");
    expect(ok, loaded_rec->fragment_path == std::filesystem::weakly_canonical(fragment),
           "round-trip: normalized fragment_path preserved");
  }

  // Dependency fingerprints persist
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = fragment;
    rec.fragment_hash = frag_hash;
    rec.state = cgraph::SemanticCacheState::Valid;
    rec.dependencies.push_back(cgraph::SemanticDependency{
        .node_id = "code:service",
        .source_path = "src/service.ts",
        .source_sha256 = "abc123",
    });
    rec.dependencies.push_back(cgraph::SemanticDependency{
        .node_id = "code:handler",
        .source_path = "src/handler.ts",
        .source_sha256 = "def456",
    });
    cache.upsert(rec);

    const auto cache_path = root / "cache-deps.json";
    cgraph::write_semantic_cache(cache, cache_path);
    const auto loaded = cgraph::read_semantic_cache(cache_path);
    const auto loaded_rec = loaded.find_for_source(doc_a);
    expect(ok, loaded_rec.has_value() && loaded_rec->dependencies.size() == 2,
           "round-trip: dependencies preserved count");
    expect(ok, loaded_rec->dependencies[0].node_id == "code:service" &&
                   loaded_rec->dependencies[0].source_path == "src/service.ts" &&
                   loaded_rec->dependencies[0].source_sha256 == "abc123",
           "round-trip: first dependency fields");
    expect(ok, loaded_rec->dependencies[1].node_id == "code:handler" &&
                   loaded_rec->dependencies[1].source_path == "src/handler.ts" &&
                   loaded_rec->dependencies[1].source_sha256 == "def456",
           "round-trip: second dependency fields");
  }

  // Rejection reason (last_error) persists
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = fragment;
    rec.state = cgraph::SemanticCacheState::Failed;
    rec.last_error = "dependency node missing: code:deleted";
    cache.upsert(rec);

    const auto cache_path = root / "cache-error.json";
    cgraph::write_semantic_cache(cache, cache_path);
    const auto loaded = cgraph::read_semantic_cache(cache_path);
    const auto loaded_rec = loaded.find_for_source(doc_a);
    expect(ok, loaded_rec.has_value() && loaded_rec->state == cgraph::SemanticCacheState::Failed,
           "round-trip: failed state preserved");
    expect(ok, loaded_rec->last_error == "dependency node missing: code:deleted",
           "round-trip: last_error preserved");
  }

  // v1 schema is rejected (treated as empty cache)
  {
    const auto v1_path = root / "cache-v1.json";
    write_file(v1_path, R"({"version":1,"records":[{"content_hash":"abc","source_path":"x","fragment_path":"y","state":"valid"}]})");
    const auto loaded = cgraph::read_semantic_cache(v1_path);
    expect(ok, loaded.size() == 0, "v1 schema rejected as empty cache");
  }

  // Unsupported version rejected
  {
    const auto v99_path = root / "cache-v99.json";
    write_file(v99_path, R"({"version":99,"records":[]})");
    const auto loaded = cgraph::read_semantic_cache(v99_path);
    expect(ok, loaded.size() == 0, "unsupported version rejected");
  }

  // A v2 label alone is insufficient: malformed dependency-fingerprint shape
  // invalidates the whole cache instead of partially reusing ambiguous records.
  {
    const auto malformed_v2_path = root / "cache-malformed-v2.json";
    write_file(
        malformed_v2_path,
        R"({"version":2,"records":[{"source_path":"x","content_hash":"abc","fragment_path":"y","fragment_hash":"def","state":"valid"}]})");
    const auto loaded = cgraph::read_semantic_cache(malformed_v2_path);
    expect(ok, loaded.size() == 0, "malformed v2 schema rejected as an empty cache");
  }

  // Duplicate source identities make the entire v2 cache ambiguous, both when
  // the serialized paths are identical and when they are lexical aliases.
  {
    const auto duplicate_path = root / "cache-duplicate-source.json";
    write_file(
        duplicate_path,
        R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"first","fragment_path":"drop-a.json","fragment_hash":"fragment-a","state":"valid","dependencies":[]},{"source_path":"docs/a.md","content_hash":"second","fragment_path":"drop-b.json","fragment_hash":"fragment-b","state":"valid","dependencies":[]},{"source_path":"docs/unrelated.md","content_hash":"third","fragment_path":"drop-c.json","fragment_hash":"fragment-c","state":"valid","dependencies":[]}]})");
    const auto loaded = cgraph::read_semantic_cache(duplicate_path);
    expect(ok, loaded.size() == 0,
           "duplicate source identity rejects the entire v2 cache");
  }
  {
    const auto alias_path = root / "cache-aliased-source.json";
    write_file(
        alias_path,
        R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"first","fragment_path":"drop-a.json","fragment_hash":"fragment-a","state":"valid","dependencies":[]},{"source_path":"docs/nested/../a.md","content_hash":"second","fragment_path":"drop-b.json","fragment_hash":"fragment-b","state":"valid","dependencies":[]},{"source_path":"docs/unrelated.md","content_hash":"third","fragment_path":"drop-c.json","fragment_hash":"fragment-c","state":"valid","dependencies":[]}]})");
    const auto loaded = cgraph::read_semantic_cache(alias_path);
    expect(ok, loaded.size() == 0,
           "lexical source alias rejects the entire v2 cache");
  }

  // Every malformed schema-v2 field fails closed without an exception or
  // partial reuse. last_error remains optional, but when present it is a string;
  // external dependency fingerprints are complete, non-empty string triples.
  {
    const std::vector<std::pair<std::string, std::string>> malformed = {
        {"missing-version", R"({"records":[]})"},
        {"version-type", R"({"version":"2","records":[]})"},
        {"records-type", R"({"version":2,"records":{}})"},
        {"record-type", R"({"version":2,"records":[[]]})"},
        {"source-path-type", R"({"version":2,"records":[{"source_path":7,"content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[]}]})"},
        {"content-hash-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":false,"fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[]}]})"},
        {"fragment-path-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":[],"fragment_hash":"def","state":"valid","dependencies":[]}]})"},
        {"fragment-hash-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":null,"state":"valid","dependencies":[]}]})"},
        {"state-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":0,"dependencies":[]}]})"},
        {"state-value", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"unknown","dependencies":[]}]})"},
        {"last-error-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"failed","last_error":["bad"],"dependencies":[]}]})"},
        {"dependencies-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":{}}]})"},
        {"dependency-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":["code:service"]}]})"},
        {"dependency-node-id-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[{"node_id":4,"source_path":"src/service.ts","source_sha256":"123"}]}]})"},
        {"dependency-node-id-empty", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[{"node_id":"","source_path":"src/service.ts","source_sha256":"123"}]}]})"},
        {"dependency-source-path-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[{"node_id":"code:service","source_path":9,"source_sha256":"123"}]}]})"},
        {"dependency-source-path-empty", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[{"node_id":"code:service","source_path":"","source_sha256":"123"}]}]})"},
        {"dependency-source-sha-type", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[{"node_id":"code:service","source_path":"src/service.ts","source_sha256":123}]}]})"},
        {"dependency-source-sha-empty", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[{"node_id":"code:service","source_path":"src/service.ts","source_sha256":""}]}]})"},
        {"dependency-field-missing", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[{"node_id":"code:service","source_path":"src/service.ts"}]}]})"},
        {"partial-reuse", R"({"version":2,"records":[{"source_path":"docs/a.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"valid","dependencies":[]},{"source_path":"docs/b.md","content_hash":"abc","fragment_path":"drop.json","fragment_hash":"def","state":"failed","last_error":false,"dependencies":[]}]})"},
    };
    for (const auto& [name, contents] : malformed) {
      expect_empty_cache_no_throw(ok, root, name, contents);
    }
  }

  // Malformed JSON treated as empty cache
  {
    const auto bad_path = root / "cache-bad.json";
    write_file(bad_path, "not json at all");
    const auto loaded = cgraph::read_semantic_cache(bad_path);
    expect(ok, loaded.size() == 0, "malformed JSON treated as empty cache");
  }

  // find_for_fragment returns all records for a given fragment
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec_a;
    rec_a.source_path = doc_a;
    rec_a.content_hash = hash_a;
    rec_a.fragment_path = fragment;
    rec_a.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(rec_a);

    cgraph::SemanticCacheRecord rec_b;
    rec_b.source_path = doc_b;
    rec_b.content_hash = hash_b;
    rec_b.fragment_path = fragment;
    rec_b.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(rec_b);

    const auto for_frag = cache.find_for_fragment(fragment);
    expect(ok, for_frag.size() == 2, "find_for_fragment returns both records");
  }

  // Upsert overwrites by source path
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = "old_hash";
    rec.fragment_path = fragment;
    rec.state = cgraph::SemanticCacheState::Failed;
    rec.last_error = "old error";
    cache.upsert(rec);

    cgraph::SemanticCacheRecord rec2;
    rec2.source_path = doc_a;
    rec2.content_hash = hash_a;
    rec2.fragment_path = fragment;
    rec2.state = cgraph::SemanticCacheState::Valid;
    rec2.last_error.clear();
    cache.upsert(rec2);

    expect(ok, cache.size() == 1, "upsert replaces same source_path");
    const auto found = cache.find_for_source(doc_a);
    expect(ok, found->state == cgraph::SemanticCacheState::Valid, "upsert cleared failure state");
    expect(ok, found->last_error.empty(), "upsert cleared last_error");
  }

  // count_valid / count_stale / count_failed
  {
    cgraph::SemanticCache cache;
    const auto doc_c = root / "docs" / "c.md";
    write_file(doc_c, "# C\n");

    cgraph::SemanticCacheRecord r1;
    r1.source_path = doc_a;
    r1.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(r1);

    cgraph::SemanticCacheRecord r2;
    r2.source_path = doc_b;
    r2.state = cgraph::SemanticCacheState::Stale;
    cache.upsert(r2);

    cgraph::SemanticCacheRecord r3;
    r3.source_path = doc_c;
    r3.state = cgraph::SemanticCacheState::Failed;
    cache.upsert(r3);

    expect(ok, cache.count_valid() == 1, "count_valid");
    expect(ok, cache.count_stale() == 1, "count_stale");
    expect(ok, cache.count_failed() == 1, "count_failed");
  }

  // Reconciliation: missing dependency node invalidates record
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = fragment;
    rec.fragment_hash = frag_hash;
    rec.state = cgraph::SemanticCacheState::Valid;
    rec.dependencies.push_back(cgraph::SemanticDependency{
        .node_id = "code:deleted_node",
        .source_path = "src/deleted.ts",
        .source_sha256 = "xyz",
    });
    cache.upsert(rec);

    cgraph::GraphSnapshot graph;
    graph.nodes.push_back(cgraph::Node{.id = "code:other", .label = "Other", .kind = "class"});

    const auto result = cgraph::reconcile_semantic_cache(cache, graph);
    expect(ok, result.records_checked == 1, "reconcile: checked one record");
    expect(ok, result.records_invalidated == 1, "reconcile: invalidated record with missing dep");
    const auto rec_after = cache.find_for_source(doc_a);
    expect(ok, rec_after->state == cgraph::SemanticCacheState::Stale, "reconcile: record marked stale");
    expect(ok, rec_after->last_error.find("code:deleted_node") != std::string::npos,
           "reconcile: last_error mentions missing node");
  }

  // Reconciliation: missing fragment file invalidates record
  {
    cgraph::SemanticCache cache;
    const auto missing_frag = root / "semantic-drop" / "missing.json";
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = missing_frag;
    rec.fragment_hash = "does_not_matter";
    rec.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(rec);

    cgraph::GraphSnapshot graph;
    const auto result = cgraph::reconcile_semantic_cache(cache, graph);
    expect(ok, result.records_invalidated == 1, "reconcile: missing fragment invalidated");
    expect(ok, cache.find_for_source(doc_a)->state == cgraph::SemanticCacheState::Stale,
           "reconcile: missing fragment -> stale");
  }

  // Reconciliation: changed fragment hash invalidates record
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = fragment;
    rec.fragment_hash = "wrong_hash";
    rec.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(rec);

    cgraph::GraphSnapshot graph;
    const auto result = cgraph::reconcile_semantic_cache(cache, graph);
    expect(ok, result.records_invalidated == 1, "reconcile: changed fragment hash invalidated");
  }

  // Existing paths whose bytes cannot be fingerprinted are read failures, not
  // ordinary content changes. Directories provide real, present unreadable
  // filesystem objects without a hash-function seam.
  {
    const auto unreadable_source = root / "docs" / "unreadable-source";
    std::filesystem::create_directories(unreadable_source);
    expect(ok, std::filesystem::exists(unreadable_source) &&
                   cgraph::sha256_file_hex(unreadable_source).empty(),
           "reconcile: source read-failure fixture returns an empty fingerprint");

    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = unreadable_source;
    rec.content_hash = "previous-source-fingerprint";
    rec.fragment_path = fragment;
    rec.fragment_hash = frag_hash;
    cache.upsert(rec);

    const auto result = cgraph::reconcile_semantic_cache(
        cache, cgraph::GraphSnapshot{});
    const auto after = cache.find_for_source(unreadable_source);
    expect(ok, result.records_invalidated == 1 && after.has_value() &&
                   after->state == cgraph::SemanticCacheState::Stale,
           "reconcile: source fingerprint read failure marks the record stale");
    expect(ok, after.has_value() &&
                   after->last_error.find("semantic source fingerprint read failed") !=
                       std::string::npos,
           "reconcile: source fingerprint read failure has an explicit reason");
    expect(ok, after.has_value() &&
                   after->last_error.find("hash changed") == std::string::npos,
           "reconcile: source fingerprint read failure is not a content change");
  }
  {
    const auto unreadable_fragment = root / "semantic-drop" / "unreadable-fragment";
    std::filesystem::create_directories(unreadable_fragment);
    expect(ok, std::filesystem::exists(unreadable_fragment) &&
                   cgraph::sha256_file_hex(unreadable_fragment).empty(),
           "reconcile: fragment read-failure fixture returns an empty fingerprint");

    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = unreadable_fragment;
    rec.fragment_hash = "previous-fragment-fingerprint";
    cache.upsert(rec);

    const auto result = cgraph::reconcile_semantic_cache(
        cache, cgraph::GraphSnapshot{});
    const auto after = cache.find_for_source(doc_a);
    expect(ok, result.records_invalidated == 1 && after.has_value() &&
                   after->state == cgraph::SemanticCacheState::Stale,
           "reconcile: fragment fingerprint read failure marks the record stale");
    expect(ok, after.has_value() &&
                   after->last_error.find("fragment fingerprint read failed") !=
                       std::string::npos,
           "reconcile: fragment fingerprint read failure has an explicit reason");
    expect(ok, after.has_value() &&
                   after->last_error.find("hash changed") == std::string::npos,
           "reconcile: fragment fingerprint read failure is not a content change");
  }

  // Reconciliation: valid record with matching deps preserved
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = fragment;
    rec.fragment_hash = frag_hash;
    rec.state = cgraph::SemanticCacheState::Valid;
    rec.dependencies.push_back(cgraph::SemanticDependency{
        .node_id = "code:service",
        .source_path = "src/service.ts",
        .source_sha256 = "service-hash",
    });
    cache.upsert(rec);

    cgraph::GraphSnapshot graph;
    graph.nodes.push_back(cgraph::Node{.id = "code:service", .label = "Service", .source_file = "src/service.ts", .kind = "class"});
    graph.source_hashes["src/service.ts"] = "service-hash";

    const auto result = cgraph::reconcile_semantic_cache(cache, graph);
    expect(ok, result.records_preserved == 1, "reconcile: matching deps preserved");
    expect(ok, cache.find_for_source(doc_a)->state == cgraph::SemanticCacheState::Valid,
           "reconcile: record stays valid");
  }

  // Reconciliation: already-stale records are preserved (not double-invalidated)
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.state = cgraph::SemanticCacheState::Stale;
    rec.last_error = "previously stale";
    cache.upsert(rec);

    cgraph::GraphSnapshot graph;
    const auto result = cgraph::reconcile_semantic_cache(cache, graph);
    expect(ok, result.records_preserved == 1, "reconcile: already-stale not re-invalidated");
    expect(ok, result.records_invalidated == 0, "reconcile: no double invalidation");
  }

  // Reconciliation compares an external dependency to the selected snapshot's
  // exact source ledger; an unrelated source hash does not invalidate it.
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    rec.fragment_path = fragment;
    rec.fragment_hash = frag_hash;
    rec.dependencies.push_back(cgraph::SemanticDependency{
        .node_id = "code:service",
        .source_path = "src/service.ts",
        .source_sha256 = "service-v1",
    });
    cache.upsert(rec);

    cgraph::GraphSnapshot unrelated_edit;
    unrelated_edit.nodes.push_back(cgraph::Node{
        .id = "code:service",
        .label = "Service",
        .source_file = "src/service.ts",
        .kind = "class",
    });
    unrelated_edit.source_hashes["src/service.ts"] = "service-v1";
    unrelated_edit.source_hashes["src/unrelated.ts"] = "unrelated-v2";
    const auto preserved = cgraph::reconcile_semantic_cache(cache, unrelated_edit);
    expect(ok, preserved.records_invalidated == 0 && preserved.records_preserved == 1,
           "reconcile: unrelated code edit preserves cache hit");

    auto changed_dependency = unrelated_edit;
    changed_dependency.source_hashes["src/service.ts"] = "service-v2";
    const auto invalidated = cgraph::reconcile_semantic_cache(cache, changed_dependency);
    expect(ok, invalidated.records_invalidated == 1,
           "reconcile: referenced code source hash change invalidates record");
    expect(ok, cache.find_for_source(doc_a)->last_error.find("source hash changed") != std::string::npos,
           "reconcile: dependency hash error is explicit");
  }

  // A fragment is one overlay unit: if one mapped source becomes invalid, every
  // still-valid peer record for that fragment is requeued too.
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord invalid;
    invalid.source_path = doc_a;
    invalid.content_hash = hash_a;
    invalid.fragment_path = fragment;
    invalid.fragment_hash = frag_hash;
    invalid.dependencies.push_back(cgraph::SemanticDependency{
        .node_id = "code:missing",
        .source_path = "src/missing.ts",
        .source_sha256 = "missing-hash",
    });
    cache.upsert(invalid);

    cgraph::SemanticCacheRecord peer;
    peer.source_path = doc_b;
    peer.content_hash = hash_b;
    peer.fragment_path = fragment;
    peer.fragment_hash = frag_hash;
    cache.upsert(peer);

    cgraph::GraphSnapshot graph;
    const auto reconciled = cgraph::reconcile_semantic_cache(cache, graph);
    expect(ok, reconciled.records_invalidated == 2,
           "reconcile: invalidates every source mapped to an invalid fragment");
    expect(ok, cache.find_for_source(doc_a)->state == cgraph::SemanticCacheState::Stale &&
                   cache.find_for_source(doc_b)->state == cgraph::SemanticCacheState::Stale,
           "reconcile: fragment peers are both stale");
  }

#if defined(__APPLE__) || defined(__unix__)
  // Multiple records mapped to one normalized fragment fingerprint its bytes
  // once per reconciliation. A FIFO supplies different bytes to a hypothetical
  // second open, making repeated reads observable without a hash-function seam.
  {
    const auto fragment_fifo = root / "semantic-drop" / "shared-fragment.fifo";
    std::filesystem::remove(fragment_fifo);
    if (::mkfifo(fragment_fifo.c_str(), 0600) != 0) {
      std::cerr << "FAIL: unable to create reconciliation FIFO\n";
      ok = false;
    } else {
      constexpr std::string_view first_fragment = "stable fragment bytes";
      constexpr std::string_view second_fragment = "different bytes on a repeated read";
      std::atomic<bool> stop_writer{false};
      std::atomic<int> writes{0};

      const auto write_when_read = [&](std::string_view contents) {
        while (!stop_writer.load()) {
          const int descriptor = ::open(fragment_fifo.c_str(), O_WRONLY | O_NONBLOCK);
          if (descriptor >= 0) {
            std::size_t written = 0;
            while (written < contents.size()) {
              const auto count = ::write(
                  descriptor,
                  contents.data() + written,
                  contents.size() - written);
              if (count <= 0) {
                break;
              }
              written += static_cast<std::size_t>(count);
            }
            ::close(descriptor);
            if (written == contents.size()) {
              ++writes;
            }
            return;
          }
          if (errno != ENXIO && errno != ENOENT) {
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
      };

      std::thread writer([&] {
        write_when_read(first_fragment);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        write_when_read(second_fragment);
      });

      cgraph::SemanticCache cache;
      for (const auto& source : {doc_a, doc_b}) {
        cgraph::SemanticCacheRecord record;
        record.source_path = source;
        record.content_hash = cgraph::sha256_file_hex(source);
        record.fragment_path = fragment_fifo;
        record.fragment_hash = cgraph::sha256_hex(first_fragment);
        cache.upsert(std::move(record));
      }

      const auto reconciled = cgraph::reconcile_semantic_cache(
          cache, cgraph::GraphSnapshot{});
      stop_writer = true;
      writer.join();

      expect(ok, writes.load() == 1,
             "reconcile: shared fragment bytes read once");
      expect(ok, reconciled.records_invalidated == 0 &&
                     reconciled.records_preserved == 2,
             "reconcile: one shared fragment fingerprint preserves both records");
    }
  }
#endif

  // remove() deletes by source path
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = doc_a;
    rec.content_hash = hash_a;
    cache.upsert(rec);
    expect(ok, cache.size() == 1, "pre-remove size");
    cache.remove(doc_a);
    expect(ok, cache.size() == 0, "post-remove size");
    expect(ok, !cache.find_for_source(doc_a).has_value(), "post-remove find returns empty");
  }

  std::filesystem::remove_all(root);
  return ok ? 0 : 1;
}
