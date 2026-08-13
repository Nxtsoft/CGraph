#include "cgraph/semantic_ingest.hpp"

#include "cgraph/daemon_ops.hpp"
#include "cgraph/file_cache.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/semantic_chunk_plan.hpp"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

void expect(bool& ok, bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

bool has_node_label(const cgraph::GraphSnapshot& graph, const std::string& label) {
  for (const auto& node : graph.nodes) {
    if (node.label == label) {
      return true;
    }
  }
  return false;
}

bool has_edge(const cgraph::GraphSnapshot& graph, const std::string& source, const std::string& relation, const std::string& target) {
  for (const auto& edge : graph.edges) {
    if (edge.source == source && edge.relation == relation && edge.target == target) {
      return true;
    }
  }
  return false;
}

bool has_hyperedge(const cgraph::GraphSnapshot& graph, const std::string& id) {
  for (const auto& hyperedge : graph.hyperedges) {
    if (hyperedge.id == id) {
      return true;
    }
  }
  return false;
}

int open_fifo_writer_when_reader_waits(const std::filesystem::path& path) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (descriptor >= 0) {
      return descriptor;
    }
    if (errno != ENXIO) {
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return -1;
}

bool write_fifo_and_close(int descriptor, std::string_view contents) {
  const auto bytes_written = ::write(descriptor, contents.data(), contents.size());
  const auto close_result = ::close(descriptor);
  return bytes_written == static_cast<ssize_t>(contents.size()) && close_result == 0;
}

bool serve_fifo_reads_until_ready(
    std::future<cgraph::SemanticIngestResult>& ingest_future,
    const std::filesystem::path& path,
    std::string_view contents) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (ingest_future.wait_for(std::chrono::milliseconds{1}) !=
         std::future_status::ready) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (descriptor >= 0) {
      if (!write_fifo_and_close(descriptor, contents)) {
        return false;
      }
    } else if (errno != ENXIO) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  const auto root =
      std::filesystem::weakly_canonical(std::filesystem::temp_directory_path() / "cgraph-semantic-ingest-test");
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  bool ok = true;

  const auto source = root / "docs" / "guide.md";
  const auto peer_source = root / "docs" / "peer.md";
  const auto valid_fragment = root / "graphify-out" / "semantic-drop" / "chunk_01.json";
  const auto invalid_fragment = root / "graphify-out" / "semantic-drop" / "chunk_02.json";
  write_file(source, "# Guide\nMentions service\n");
  write_file(peer_source, "# Peer\nMentions service too\n");
  write_file(
      valid_fragment,
      R"({
        "nodes": [
          {"id": "doc:guide", "label": "Guide", "type": "document", "source_file": "docs/guide.md"},
          {"id": "concept:service", "label": "Service", "type": "concept", "source_file": "docs/guide.md"}
        ],
        "edges": [
          {"source": "doc:guide", "target": "concept:service", "relation": "MENTIONS"}
        ],
        "hyperedges": []
      })");
  write_file(invalid_fragment, R"({"nodes":[{"id":"missing-label"}],"edges":[]})");

  cgraph::DaemonState state;
  cgraph::GraphSnapshot initial;
  initial.nodes.push_back(cgraph::Node{.id = "code:service", .label = "CodeService", .source_file = "src/service.ts", .kind = "class"});
  initial.source_hashes["src/service.ts"] = "code-service-sha256";
  initial.build_state = cgraph::BuildState::DeterministicReady;
  cgraph::publish_graph_snapshot(state, std::move(initial));

  cgraph::SemanticCache cache;

  const auto stale_authorship = cgraph::ingest_semantic_fragment(
      state,
      cache,
      {{.path = source, .content_sha256 = "planned-before-source-edit"}},
      valid_fragment);
  expect(ok, !stale_authorship.merged && !stale_authorship.errors.empty() &&
                 stale_authorship.errors.front().find("changed since planning") != std::string::npos,
         "stale authorship is rejected against the planned semantic source hash");

  // Valid fragment merges and records dependencies
  auto result = cgraph::ingest_semantic_fragment(
      state,
      cache,
      {{.path = source, .content_sha256 = cgraph::sha256_file_hex(source)}},
      valid_fragment);
  auto graph = cgraph::read_graph_snapshot(state);
  expect(ok, result.merged && result.cache_updated && result.errors.empty(), "valid ingest succeeds");
  expect(ok, graph->nodes.size() == 3, "valid ingest: 3 nodes");
  expect(ok, has_node_label(*graph, "CodeService"), "valid ingest: code node preserved");
  expect(ok, has_node_label(*graph, "Guide"), "valid ingest: doc node merged");
  expect(ok, has_node_label(*graph, "Service"), "valid ingest: concept node merged");
  expect(ok, has_edge(*graph, "doc:guide", "MENTIONS", "concept:service"), "valid ingest: edge merged");

  // Cache record has fragment hash and source info
  const auto cached = cache.find_for_source(source);
  expect(ok, cached.has_value(), "valid ingest: cache record exists");
  expect(ok, cached->state == cgraph::SemanticCacheState::Valid, "valid ingest: state is valid");
  expect(ok, cached->fragment_path == std::filesystem::weakly_canonical(valid_fragment),
         "valid ingest: fragment_path normalized");
  expect(ok, !cached->fragment_hash.empty(), "valid ingest: fragment_hash recorded");
  expect(ok, cached->fragment_hash == cgraph::sha256_file_hex(valid_fragment), "valid ingest: fragment_hash matches file");
  expect(ok, !cached->content_hash.empty(), "valid ingest: content_hash recorded");
  expect(ok, cached->content_hash == cgraph::sha256_file_hex(source), "valid ingest: content_hash matches source");
  expect(ok, cached->last_error.empty(), "valid ingest: no error");

  // Fragment with edge to existing graph node records it as external dependency
  // (doc:guide -> concept:service are both in-fragment, but doc:guide -> code:service is external)
  // Actually the valid_fragment only references fragment-internal nodes, so no external deps.
  // Let's check a connected fragment that has an external dep.

  // Invalid fragment is rejected and records failure in cache
  result = cgraph::ingest_semantic_fragment(state, cache, {{.path = source}}, invalid_fragment);
  graph = cgraph::read_graph_snapshot(state);
  expect(ok, !result.merged, "invalid ingest: not merged");
  expect(ok, !result.errors.empty(), "invalid ingest: has errors");
  expect(ok, graph->nodes.size() == 3, "invalid ingest: graph unchanged");

  // Failed ingest records failure state in cache
  const auto cached_after_invalid = cache.find_for_source(source);
  expect(ok, cached_after_invalid.has_value(), "invalid ingest: cache record exists");
  expect(ok, cached_after_invalid->state == cgraph::SemanticCacheState::Failed, "invalid ingest: state is failed");
  expect(ok, !cached_after_invalid->last_error.empty(), "invalid ingest: last_error recorded");

  // Referential integrity: dangling edge rejected with failure recorded
  const auto dangling_fragment = root / "graphify-out" / "semantic-drop" / "chunk_03.json";
  write_file(
      dangling_fragment,
      R"({
        "nodes": [
          {"id": "doc:orphan", "label": "Orphan", "type": "document", "source_file": "docs/guide.md"}
        ],
        "edges": [
          {"source": "doc:orphan", "target": "concept:ghost", "relation": "MENTIONS"}
        ],
        "hyperedges": []
      })");
  const auto nodes_before_dangling = graph->nodes.size();
  result = cgraph::ingest_semantic_fragment(
      state, cache, {{.path = source}, {.path = peer_source}}, dangling_fragment);
  graph = cgraph::read_graph_snapshot(state);
  expect(ok, !result.merged && !result.errors.empty(), "dangling: rejected");
  expect(ok, graph->nodes.size() == nodes_before_dangling, "dangling: graph unchanged");
  expect(ok, !has_node_label(*graph, "Orphan"), "dangling: orphan node not in graph");

  const auto cached_dangling = cache.find_for_source(source);
  expect(ok, cached_dangling->state == cgraph::SemanticCacheState::Failed, "dangling: failure recorded");
  expect(ok, cached_dangling->last_error.find("concept:ghost") != std::string::npos,
         "dangling: error mentions missing node");
  expect(ok, cache.find_for_source(peer_source).has_value() &&
                 cache.find_for_source(peer_source)->state == cgraph::SemanticCacheState::Failed,
         "dangling: every mapped source records the current failure");

  // Referential integrity covers every hyperedge node before merge as one
  // atomic decision, including an endpoint encountered after valid members.
  const auto dangling_hyperedge_fragment =
      root / "graphify-out" / "semantic-drop" / "chunk_03_hyperedge.json";
  write_file(
      dangling_hyperedge_fragment,
      R"({
        "nodes": [
          {"id": "doc:hyper-orphan", "label": "Hyper Orphan", "type": "document", "source_file": "docs/guide.md"}
        ],
        "edges": [],
        "hyperedges": [
          {"id": "hyper:dangling", "nodes": ["doc:hyper-orphan", "code:service", "concept:hyper-ghost"], "relation": "CONNECTS"}
        ]
      })");
  const auto nodes_before_dangling_hyperedge = graph->nodes.size();
  const auto edges_before_dangling_hyperedge = graph->edges.size();
  const auto hyperedges_before_dangling_hyperedge = graph->hyperedges.size();
  result = cgraph::ingest_semantic_fragment(
      state,
      cache,
      {{.path = source}, {.path = peer_source}},
      dangling_hyperedge_fragment);
  graph = cgraph::read_graph_snapshot(state);
  expect(ok, !result.merged && !result.errors.empty() &&
                 result.errors.front().find("concept:hyper-ghost") != std::string::npos,
         "dangling hyperedge: rejected with the unknown endpoint");
  expect(ok, graph->nodes.size() == nodes_before_dangling_hyperedge &&
                 graph->edges.size() == edges_before_dangling_hyperedge &&
                 graph->hyperedges.size() == hyperedges_before_dangling_hyperedge,
         "dangling hyperedge: graph unchanged atomically");
  expect(ok, !has_node_label(*graph, "Hyper Orphan") &&
                 !has_hyperedge(*graph, "hyper:dangling"),
         "dangling hyperedge: no partial node or hyperedge merge");
  const auto cached_dangling_hyperedge = cache.find_for_source(source);
  expect(ok, cached_dangling_hyperedge.has_value() &&
                 cached_dangling_hyperedge->state == cgraph::SemanticCacheState::Failed &&
                 cached_dangling_hyperedge->last_error.find("concept:hyper-ghost") != std::string::npos &&
                 cached_dangling_hyperedge->dependencies.empty(),
         "dangling hyperedge: failed cache record has no partial dependencies");
  expect(ok, cache.find_for_source(peer_source).has_value() &&
                 cache.find_for_source(peer_source)->state == cgraph::SemanticCacheState::Failed,
         "dangling hyperedge: every mapped source records failure");

  // Connected fragment accepted: edge to existing graph node records external dependency
  const auto connected_fragment = root / "graphify-out" / "semantic-drop" / "chunk_04.json";
  write_file(
      connected_fragment,
      R"({
        "nodes": [
          {"id": "doc:connected", "label": "Connected", "type": "document", "source_file": "docs/guide.md"}
        ],
        "edges": [
          {"source": "doc:connected", "target": "code:service", "relation": "MENTIONS"}
        ],
        "hyperedges": []
      })");
  result = cgraph::ingest_semantic_fragment(
      state, cache, {{.path = source}, {.path = peer_source}}, connected_fragment);
  graph = cgraph::read_graph_snapshot(state);
  expect(ok, result.merged && result.errors.empty(), "connected: merged");
  expect(ok, has_node_label(*graph, "Connected"), "connected: node in graph");
  expect(ok, has_edge(*graph, "doc:connected", "MENTIONS", "code:service"), "connected: edge in graph");

  // External dependency recorded for code:service
  const auto cached_connected = cache.find_for_source(source);
  expect(ok, cached_connected->state == cgraph::SemanticCacheState::Valid, "connected: state valid");
  expect(ok, cached_connected->last_error.empty(), "connected: no error (cleared prior failure)");
  expect(ok, cached_connected->dependencies.size() == 1, "connected: 1 external dependency");
  const auto cached_peer = cache.find_for_source(peer_source);
  expect(ok, cached_peer.has_value() && cached_peer->state == cgraph::SemanticCacheState::Valid &&
                 cached_peer->dependencies.size() == 1 && cached_peer->last_error.empty(),
         "connected: successful replacement clears every mapped source");
  const auto source_ledger = graph->source_hashes.find(
      std::filesystem::weakly_canonical(source).generic_string());
  const auto peer_ledger = graph->source_hashes.find(
      std::filesystem::weakly_canonical(peer_source).generic_string());
  expect(ok, source_ledger != graph->source_hashes.end() &&
                 source_ledger->second == cgraph::sha256_file_hex(source) &&
                 peer_ledger != graph->source_hashes.end() &&
                 peer_ledger->second == cgraph::sha256_file_hex(peer_source),
         "connected: semantic source hashes join the selected snapshot ledger");
  if (!cached_connected->dependencies.empty()) {
    expect(ok, cached_connected->dependencies[0].node_id == "code:service",
           "connected: dependency node_id");
    expect(ok, cached_connected->dependencies[0].source_path == "src/service.ts",
           "connected: dependency source_path from graph node");
    expect(ok, cached_connected->dependencies[0].source_sha256 == "code-service-sha256",
           "connected: dependency source hash from selected graph");
  }

  // A hyperedge into the selected graph records the same exact dependency
  // triple as an ordinary edge.
  const auto connected_hyperedge_fragment =
      root / "graphify-out" / "semantic-drop" / "chunk_04_hyperedge.json";
  write_file(
      connected_hyperedge_fragment,
      R"({
        "nodes": [
          {"id": "doc:hyper-connected", "label": "Hyper Connected", "type": "document", "source_file": "docs/guide.md"}
        ],
        "edges": [],
        "hyperedges": [
          {"id": "hyper:connected", "nodes": ["doc:hyper-connected", "code:service"], "relation": "CONNECTS"}
        ]
      })");
  result = cgraph::ingest_semantic_fragment(
      state,
      cache,
      {{.path = source}, {.path = peer_source}},
      connected_hyperedge_fragment);
  graph = cgraph::read_graph_snapshot(state);
  expect(ok, result.merged && result.errors.empty() &&
                 has_hyperedge(*graph, "hyper:connected"),
         "connected hyperedge: merged");
  const auto cached_connected_hyperedge = cache.find_for_source(source);
  expect(ok, cached_connected_hyperedge.has_value() &&
                 cached_connected_hyperedge->dependencies.size() == 1,
         "connected hyperedge: one external dependency recorded");
  if (cached_connected_hyperedge.has_value() &&
      cached_connected_hyperedge->dependencies.size() == 1) {
    const auto& dependency = cached_connected_hyperedge->dependencies.front();
    expect(ok, dependency.node_id == "code:service" &&
                   dependency.source_path == "src/service.ts" &&
                   dependency.source_sha256 == "code-service-sha256",
           "connected hyperedge: exact snapshot dependency triple recorded");
  }

  // Successful replacement clears prior failure
  {
    cgraph::DaemonState state2;
    cgraph::GraphSnapshot g2;
    g2.nodes.push_back(cgraph::Node{.id = "code:alpha", .label = "Alpha", .kind = "function"});
    cgraph::publish_graph_snapshot(state2, std::move(g2));

    cgraph::SemanticCache cache2;
    const auto src2 = root / "docs" / "replace.md";
    const auto bad_frag = root / "semantic-drop" / "bad.json";
    const auto good_frag = root / "semantic-drop" / "good.json";
    write_file(src2, "# Replace\nContent\n");
    write_file(bad_frag, R"({"nodes":[{"id":"no-label"}],"edges":[]})");
    write_file(good_frag, R"({"nodes":[{"id":"doc:replace","label":"Replace","type":"document"}],"edges":[],"hyperedges":[]})");

    auto r1 = cgraph::ingest_semantic_fragment(state2, cache2, {{.path = src2}}, bad_frag);
    expect(ok, !r1.merged, "replacement: first ingest fails");
    const auto after_fail = cache2.find_for_source(src2);
    expect(ok, after_fail->state == cgraph::SemanticCacheState::Failed, "replacement: failed state");
    expect(ok, !after_fail->last_error.empty(), "replacement: error present");

    auto r2 = cgraph::ingest_semantic_fragment(state2, cache2, {{.path = src2}}, good_frag);
    expect(ok, r2.merged, "replacement: second ingest succeeds");
    const auto after_success = cache2.find_for_source(src2);
    expect(ok, after_success->state == cgraph::SemanticCacheState::Valid, "replacement: success clears failure");
    expect(ok, after_success->last_error.empty(), "replacement: error cleared");
    expect(ok, after_success->fragment_path == std::filesystem::weakly_canonical(good_frag),
           "replacement: fragment path updated");
  }

  // Dependency discovery and merge are one snapshot decision. The FIFO blocks
  // source hashing after fragment validation, giving a concurrent publisher a
  // deterministic window to replace the graph before that decision and merge.
  {
    cgraph::DaemonState race_state;
    cgraph::GraphSnapshot before_publication;
    before_publication.nodes.push_back(cgraph::Node{
        .id = "code:race-dependency",
        .label = "RaceDependency",
        .source_file = "src/race.ts",
        .kind = "class",
    });
    before_publication.source_hashes["src/race.ts"] = "race-dependency-sha256";
    before_publication.build_state = cgraph::BuildState::DeterministicReady;
    cgraph::publish_graph_snapshot(race_state, std::move(before_publication));

    cgraph::SemanticCache race_cache;
    const auto fifo_source = root / "docs" / "race-source.fifo";
    const auto race_fragment = root / "semantic-drop" / "race.json";
    write_file(
        race_fragment,
        R"({
          "nodes": [
            {"id": "doc:race", "label": "Race Document", "type": "document", "source_file": "docs/race.md"}
          ],
          "edges": [
            {"source": "doc:race", "target": "code:race-dependency", "relation": "MENTIONS"}
          ],
          "hyperedges": []
        })");
    expect(ok, ::mkfifo(fifo_source.c_str(), 0600) == 0,
           "snapshot race: blocking FIFO source created");

    auto ingest_future = std::async(std::launch::async, [&] {
      return cgraph::ingest_semantic_fragment(
          race_state, race_cache, {{.path = fifo_source}}, race_fragment);
    });
    const auto fifo_writer = open_fifo_writer_when_reader_waits(fifo_source);
    expect(ok, fifo_writer >= 0,
           "snapshot race: ingest reached source hashing after fragment validation");

    cgraph::GraphSnapshot concurrent_publication;
    concurrent_publication.nodes.push_back(cgraph::Node{
        .id = "code:replacement",
        .label = "ConcurrentReplacement",
        .source_file = "src/replacement.ts",
        .kind = "class",
    });
    concurrent_publication.source_hashes["src/replacement.ts"] =
        "replacement-sha256";
    concurrent_publication.build_state = cgraph::BuildState::DeterministicReady;
    cgraph::publish_graph_snapshot(race_state, std::move(concurrent_publication));
    if (fifo_writer >= 0) {
      expect(ok, write_fifo_and_close(fifo_writer, "semantic source bytes\n"),
             "snapshot race: FIFO source bytes delivered");
    }
    expect(ok,
           serve_fifo_reads_until_ready(
               ingest_future, fifo_source, "semantic source bytes\n"),
           "snapshot race: failed-ingest fingerprint read completed");

    const auto race_result = ingest_future.get();
    std::filesystem::remove(fifo_source);
    const auto race_graph = cgraph::read_graph_snapshot(race_state);
    expect(ok, !race_result.merged && !race_result.errors.empty() &&
                   race_result.errors.front().find("code:race-dependency") !=
                       std::string::npos,
           "snapshot race: dependency is revalidated against the graph being merged");
    expect(ok, has_node_label(*race_graph, "ConcurrentReplacement") &&
                   !has_node_label(*race_graph, "Race Document") &&
                   !has_edge(*race_graph,
                             "doc:race",
                             "MENTIONS",
                             "code:race-dependency"),
           "snapshot race: concurrent publication remains unchanged after rejection");
  }

  // An existing path is not a valid semantic source when hashing returns no
  // digest. A directory exercises the real file reader's read-failure path.
  {
    cgraph::DaemonState hash_state;
    cgraph::publish_graph_snapshot(hash_state, cgraph::GraphSnapshot{});
    cgraph::SemanticCache hash_cache;
    const auto unhashable_source = root / "docs" / "unhashable-source";
    const auto hash_fragment = root / "semantic-drop" / "empty-hash.json";
    std::filesystem::create_directories(unhashable_source);
    write_file(
        hash_fragment,
        R"({"nodes":[{"id":"doc:empty-hash","label":"Empty Hash","type":"document"}],"edges":[],"hyperedges":[]})");
    expect(ok, cgraph::sha256_file_hex(unhashable_source).empty(),
           "empty digest: real source hashing reports failure");

    const auto hash_result = cgraph::ingest_semantic_fragment(
        hash_state, hash_cache, {{.path = unhashable_source}}, hash_fragment);
    const auto hash_graph = cgraph::read_graph_snapshot(hash_state);
    expect(ok, !hash_result.merged && !hash_result.errors.empty() &&
                   hash_result.errors.front().find("could not be hashed") !=
                       std::string::npos,
           "empty digest: semantic ingest rejects an unhashable source");
    expect(ok, !has_node_label(*hash_graph, "Empty Hash"),
           "empty digest: rejected source does not mutate the graph");
    const auto failed_hash = hash_cache.find_for_source(unhashable_source);
    expect(ok, failed_hash.has_value() &&
                   failed_hash->state == cgraph::SemanticCacheState::Failed,
           "empty digest: rejected source replaces its cache record with failure");
  }

  // A failure replaces the fragment's complete current mapping, including
  // deleting sources that belonged only to the prior successful attempt.
  {
    cgraph::DaemonState mapping_state;
    cgraph::publish_graph_snapshot(mapping_state, cgraph::GraphSnapshot{});
    cgraph::SemanticCache mapping_cache;
    const auto current_source = root / "docs" / "mapping-current.md";
    const auto obsolete_source = root / "docs" / "mapping-obsolete.md";
    const auto mapping_fragment = root / "semantic-drop" / "mapping.json";
    write_file(current_source, "# Current mapping\n");
    write_file(obsolete_source, "# Obsolete mapping\n");
    write_file(
        mapping_fragment,
        R"({"nodes":[{"id":"doc:mapping","label":"Mapping","type":"document"}],"edges":[],"hyperedges":[]})");

    const auto mapping_success = cgraph::ingest_semantic_fragment(
        mapping_state,
        mapping_cache,
        {{.path = current_source}, {.path = obsolete_source}},
        mapping_fragment);
    expect(ok, mapping_success.merged &&
                   mapping_cache.find_for_source(current_source).has_value() &&
                   mapping_cache.find_for_source(obsolete_source).has_value(),
           "failed mapping: prior successful mapping covers both sources");

    write_file(mapping_fragment, R"({"nodes":[{"id":"invalid"}],"edges":[]})");
    const auto mapping_failure = cgraph::ingest_semantic_fragment(
        mapping_state,
        mapping_cache,
        {{.path = current_source}},
        mapping_fragment);
    const auto current_failure = mapping_cache.find_for_source(current_source);
    expect(ok, !mapping_failure.merged && current_failure.has_value() &&
                   current_failure->state == cgraph::SemanticCacheState::Failed,
           "failed mapping: current source records the failed attempt");
    expect(ok, !mapping_cache.find_for_source(obsolete_source).has_value() &&
                   mapping_cache.find_for_fragment(mapping_fragment).size() == 1,
           "failed mapping: obsolete prior source record is removed");
  }

  // Cache hit integration: enriched source is a cache hit in the planner
  {
    const auto plan = cgraph::plan_semantic_chunks(root, cache);
    expect(ok, plan.cache_hits >= 1, "planner: enriched source is a cache hit");
  }

  // Stale plan for changed source
  write_file(source, "# Guide\nChanged source\n");
  const auto stale_plan = cgraph::plan_semantic_chunks(root, cache);
  expect(ok, stale_plan.stale_inputs >= 1, "planner: changed source counted as stale");

  // Status reports enrichment state
  state.enrichment_state = cgraph::EnrichmentState::Stale;
  state.enrichment_stale = stale_plan.stale_inputs;
  const auto status = cgraph::handle_daemon_request(state, cgraph::make_request("status"));
  expect(ok, status["result"]["enrichment_state"] == "stale", "status: reports stale");
  expect(ok, status["result"]["enrichment_stale"].get<int>() >= 1, "status: reports stale count");

  std::filesystem::remove_all(root);
  return ok ? 0 : 1;
}
