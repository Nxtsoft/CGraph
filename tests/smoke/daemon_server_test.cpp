#include "cgraph/daemon_endpoint.hpp"
#include "cgraph/daemon_identity.hpp"
#include "cgraph/daemon_server.hpp"
#include "cgraph/file_cache.hpp"
#include "cgraph/protocol.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// Opens a raw connected client socket to `socket_path`, or -1. Lets the wire
// hardening tests send hand-crafted (hostile) bytes the protocol codec would
// never produce -- an oversized length header, a truncated frame -- straight at
// the real daemon.
int raw_connect(const std::filesystem::path& socket_path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const auto path = socket_path.string();
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

// Records a failed expectation with a name, so a CI flake says WHICH step
// failed instead of a bare non-zero exit.
void expect(bool& ok, bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

std::optional<nlohmann::json> request_with_retry(const std::filesystem::path& socket_path, const nlohmann::json& req) {
  std::optional<nlohmann::json> response;
  for (int attempt = 0; attempt < 100 && !response; ++attempt) {
    response = cgraph::request_over_unix_socket(socket_path, req);
    if (!response) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  return response;
}

bool response_has_label(const std::optional<nlohmann::json>& response, std::string_view label) {
  if (!response || !response->value("ok", false) || !response->contains("result")) {
    return false;
  }
  for (const auto& node : (*response)["result"].value("nodes", nlohmann::json::array())) {
    if (node.value("label", std::string{}) == label) {
      return true;
    }
  }
  return false;
}

bool wait_for_label(
    const std::filesystem::path& socket_path,
    std::string_view query,
    std::string_view label,
    bool expected) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    const auto response = request_with_retry(
        socket_path, cgraph::make_request("query", {{"q", std::string(query)}}));
    if (response && response->value("ok", false) && response_has_label(response, label) == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

std::string wait_for_node_id(
    const std::filesystem::path& socket_path,
    std::string_view query,
    std::string_view label) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    const auto response = request_with_retry(
        socket_path, cgraph::make_request("query", {{"q", std::string(query)}}));
    if (response && response->value("ok", false)) {
      for (const auto& node : (*response)["result"].value("nodes", nlohmann::json::array())) {
        if (node.value("label", std::string{}) == label) {
          return node.value("id", std::string{});
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return {};
}

}  // namespace

// Exercises the real Unix-socket daemon end to end: a server thread binds the
// per-root endpoint, a client connects over an actual socket, status/query
// round-trip through the length-prefixed protocol, and `update .` triggers a
// live rescan that picks up a newly-added source file. No mocks.
int main() {
  namespace fs = std::filesystem;
  bool ok = true;

  // Hold semantic replay on a real FIFO after deterministic extraction has
  // finished. Until that reader is released, the public daemon snapshot must
  // remain the initial empty/building graph; after release, one snapshot must
  // contain both deterministic code and the semantic overlay.
  {
    const auto atomic_root = fs::temp_directory_path() / "cgraph_daemon_atomic_overlay_test";
    const auto drop_dir = atomic_root / "cgraph-out" / "semantic-drop";
    const auto barrier_source = drop_dir / "overlay-source.fifo";
    const auto fragment_path = drop_dir / "chunk_00.json";
    const auto atomic_socket = cgraph::unix_socket_path(cgraph::daemon_identity_for(atomic_root));
    fs::remove_all(atomic_root);
    fs::remove(atomic_socket);
    write_file(
        atomic_root / "src" / "atomic.ts",
        "export class AtomicTarget { value() { return 1; } }\n");
    fs::create_directories(drop_dir);
    expect(ok, ::mkfifo(barrier_source.c_str(), 0600) == 0,
           "atomic-overlay: created real semantic-source FIFO");
    write_file(
        drop_dir / "plan.json",
        R"({"chunks":[{"index":0,"inputs":[{"path":")" +
            barrier_source.generic_string() + R"(","content_hash":""}]}]})");
    write_file(
        fragment_path,
        R"({"nodes":[{"id":"doc:atomic-overlay","label":"Atomic Overlay","type":"document"}],"edges":[],"hyperedges":[]})");

    cgraph::DaemonServerOptions atomic_options;
    atomic_options.idle_timeout = std::chrono::seconds(60);
    atomic_options.build_graph_on_start = true;
    atomic_options.code_poll_interval = std::chrono::milliseconds(0);
    atomic_options.drop_poll_interval = std::chrono::milliseconds(20);

    int atomic_rc = -1;
    std::thread atomic_server(
        [&] { atomic_rc = cgraph::run_daemon_server(atomic_root, atomic_options); });

    // O_NONBLOCK succeeds only after the daemon has opened the FIFO for reading,
    // proving the build is paused in semantic replay rather than merely still
    // extracting code. Keep the writer open without bytes so replay stays paused.
    int barrier_writer = -1;
    for (int attempt = 0; attempt < 500 && barrier_writer < 0; ++attempt) {
      barrier_writer = ::open(barrier_source.c_str(), O_WRONLY | O_NONBLOCK);
      if (barrier_writer < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    expect(ok, barrier_writer >= 0,
           "atomic-overlay: deterministic build reached semantic replay barrier");

    const auto before_publish = request_with_retry(
        atomic_socket, cgraph::make_request("query", {{"q", ""}}));
    expect(ok,
           before_publish && before_publish->value("ok", false) &&
               (*before_publish)["result"].value("graph_state", std::string{}) == "building" &&
               (*before_publish)["result"].value("nodes", nlohmann::json::array()).empty(),
           "atomic-overlay: reader saw only empty/building while fused snapshot staged");

    if (barrier_writer >= 0) {
      constexpr std::string_view semantic_source = "atomic semantic source\n";
      (void)::write(barrier_writer, semantic_source.data(), semantic_source.size());
      ::close(barrier_writer);
    }

    bool complete_snapshot_seen = false;
    for (int attempt = 0; attempt < 500 && !complete_snapshot_seen; ++attempt) {
      const auto complete = request_with_retry(
          atomic_socket, cgraph::make_request("query", {{"q", ""}}));
      complete_snapshot_seen = response_has_label(complete, "AtomicTarget") &&
                               response_has_label(complete, "Atomic Overlay");
      if (!complete_snapshot_seen) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    expect(ok, complete_snapshot_seen,
           "atomic-overlay: one published snapshot contained deterministic and semantic nodes");

    const auto atomic_shutdown = request_with_retry(
        atomic_socket, cgraph::make_request("shutdown"));
    expect(ok, atomic_shutdown && atomic_shutdown->value("ok", false),
           "atomic-overlay: shutdown accepted");
    atomic_server.join();
    expect(ok, atomic_rc == 0, "atomic-overlay: daemon exited cleanly");
    fs::remove_all(atomic_root);
  }

  const auto root = fs::temp_directory_path() / "cgraph_daemon_server_test";
  fs::remove_all(root);
  fs::create_directories(root);
  write_file(root / "src" / "alpha.ts", "export function alpha() { return 1; }\n");

  const auto identity = cgraph::daemon_identity_for(root);
  const auto socket_path = cgraph::unix_socket_path(identity);
  fs::remove(socket_path);

  cgraph::DaemonServerOptions options;
  options.idle_timeout = std::chrono::seconds(60);
  options.build_graph_on_start = true;  // build the real graph so update has a baseline
  // Fast watch/persist cadence so live-watch coverage below runs in test time.
  options.code_poll_interval = std::chrono::milliseconds(50);
  options.watch_debounce = std::chrono::milliseconds(50);
  options.persist_interval = std::chrono::seconds(1);

  int server_rc = -1;
  std::thread server([&] { server_rc = cgraph::run_daemon_server(root, options); });

  // Status round-trips immediately (the daemon serves while building on a worker
  // thread), so poll until the initial build publishes the baseline graph
  // (file + function node) rather than assuming it is ready on the first reply.
  int nodes_before = 0;
  for (int attempt = 0; attempt < 200 && nodes_before < 2; ++attempt) {
    const auto status = request_with_retry(socket_path, cgraph::make_request("status"));
    expect(ok, status && (*status)["ok"] == true, "status round-trip");
    nodes_before = status ? (*status)["result"].value("node_count", 0) : 0;
    if (nodes_before < 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, nodes_before >= 2, "initial build published baseline graph");

  // Single-owner bind: with daemon A resident on this root, a second daemon start
  // for the SAME root must defer (return 0) without unlinking A's live endpoint, and
  // A must keep answering. Regression guard against endpoint theft on a start race.
  const int defer_rc = cgraph::run_daemon_server(root, options);
  expect(ok, defer_rc == 0, "second daemon for a served root defers cleanly");
  const auto still_a = request_with_retry(socket_path, cgraph::make_request("status"));
  expect(ok, still_a && (*still_a)["ok"] == true,
         "original daemon still owns the socket after a deferred start");

  // The initial plan runs once after the build. A code-only update must re-plan
  // after dependency reconciliation because code changes can requeue semantic
  // sources even when no document bytes changed.
  int plans_run = 0;
  for (int attempt = 0; attempt < 300 && plans_run < 1; ++attempt) {
    const auto s = request_with_retry(socket_path, cgraph::make_request("status"));
    plans_run = s ? (*s)["result"].value("enrichment_plans_run", 0) : 0;
    if (plans_run < 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, plans_run >= 1, "initial enrichment plan ran after build");

  const auto code_update = request_with_retry(socket_path, cgraph::make_request("update", {{"path", "."}}));
  expect(ok, code_update && (*code_update)["ok"] == true, "code-only update accepted");
  int plans_after_code = plans_run;
  for (int attempt = 0; attempt < 300 && plans_after_code <= plans_run; ++attempt) {
    const auto after_code = request_with_retry(socket_path, cgraph::make_request("status"));
    plans_after_code = after_code ? (*after_code)["result"].value("enrichment_plans_run", 0) : -1;
    if (plans_after_code <= plans_run) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, plans_after_code > plans_run, "code-only rescan re-planned dependency health");

  write_file(root / "notes.md", "# Notes\nsome documentation\n");
  bool replanned = false;
  for (int attempt = 0; attempt < 600 && !replanned; ++attempt) {
    const auto s = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("status"));
    replanned = s && (*s)["result"].value("enrichment_plans_run", 0) > plans_after_code;
    if (!replanned) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, replanned, "doc change triggered an enrichment re-plan");

  // Add a new source file, then `update .` must rescan and grow the graph.
  write_file(root / "src" / "beta.ts", "export function beta() { return alpha(); }\n");
  const auto update = request_with_retry(socket_path, cgraph::make_request("update", {{"path", "."}}));
  expect(ok, update && (*update)["ok"] == true && (*update)["result"]["full_rescan"] == true, "update op full rescan");
  const auto nodes_after = update ? (*update)["result"].value("node_count", 0) : 0;
  expect(ok, nodes_after > nodes_before, "update grew the graph");

  // The newly-added symbol is now queryable on the resident daemon.
  const auto query = request_with_retry(socket_path, cgraph::make_request("query", {{"q", "beta"}}));
  expect(ok, query && (*query)["ok"] == true && !(*query)["result"]["nodes"].empty(), "added symbol queryable");

  // Semantic enrichment: drop a valid fragment into the daemon's drop dir and
  // the watcher must merge it into the live snapshot (no update op issued).
  const auto nodes_pre_enrich = nodes_after;
  write_file(
      root / "cgraph-out" / "semantic-drop" / "plan.json",
      R"({"chunks":[{"index":0,"inputs":[{"path":")" +
          (root / "notes.md").generic_string() + R"(","content_hash":")" +
          cgraph::sha256_file_hex(root / "notes.md") + R"("}]}]})");
  write_file(root / "cgraph-out" / "semantic-drop" / "chunk_00.json", R"({
    "nodes": [
      {"id": "doc:guide", "label": "Guide", "type": "document"},
      {"id": "concept:topic", "label": "Topic", "type": "concept"}
    ],
    "edges": [{"source": "doc:guide", "target": "concept:topic", "relation": "DESCRIBES"}]
  })");
  bool enriched = false;
  for (int attempt = 0; attempt < 200 && !enriched; ++attempt) {
    const auto s = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("status"));
    enriched = s && (*s)["result"].value("node_count", 0) >= nodes_pre_enrich + 2;
    if (!enriched) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, enriched, "valid fragment merged into live snapshot");
  const auto topic = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("query", {{"q", "Topic"}}));
  expect(ok, topic && !(*topic)["result"]["nodes"].empty(), "enriched concept queryable");

  // Touch the fragment once after its live merge so the watcher consumes it
  // against the still-current plan attribution before later tests replace the
  // plan with a different chunk.
  write_file(root / "cgraph-out" / "semantic-drop" / "chunk_00.json", R"({
    "nodes": [
      {"id": "doc:guide", "label": "Guide Attributed", "type": "document"},
      {"id": "concept:topic", "label": "Topic", "type": "concept"}
    ],
    "edges": [{"source": "doc:guide", "target": "concept:topic", "relation": "DESCRIBES"}]
  })");
  expect(ok, wait_for_label(socket_path, "Guide Attributed", "Guide Attributed", true),
         "semantic fragment cached against its manifest source");

  // A malformed fragment is rejected: enrichment_state goes failed, graph unchanged.
  const auto before_malformed = request_with_retry(socket_path, cgraph::make_request("status"));
  const int nodes_before_malformed =
      before_malformed ? (*before_malformed)["result"].value("node_count", 0) : -1;
  const auto malformed_source = root / "malformed.md";
  write_file(malformed_source, "# Malformed source\n");
  write_file(
      root / "cgraph-out" / "semantic-drop" / "plan.json",
      R"({"chunks":[{"index":1,"inputs":[{"path":")" +
          malformed_source.generic_string() + R"(","content_hash":")" +
          cgraph::sha256_file_hex(malformed_source) + R"("}]}]})");
  write_file(root / "cgraph-out" / "semantic-drop" / "chunk_01.json", R"({"nodes":[{"id":"x"}],"edges":[]})");
  bool failed_seen = false;
  int nodes_at_fail = 0;
  for (int attempt = 0; attempt < 200 && !failed_seen; ++attempt) {
    const auto s = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("status"));
    if (s && (*s)["result"]["enrichment_state"] == "failed") {
      failed_seen = true;
      nodes_at_fail = (*s)["result"].value("node_count", 0);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, failed_seen && nodes_at_fail == nodes_before_malformed,
         "malformed fragment rejected, graph unchanged");

  // Live watching: a new source file lands in the graph with NO update op. The
  // gitignored peer (written first, same watch window) must never enter.
  write_file(root / ".gitignore", "scratch\n");
  write_file(root / "scratch" / "hidden.ts", "export function hidden() { return 0; }\n");
  write_file(root / "src" / "gamma.ts", "export function gamma() { return 2; }\n");
  bool watched = false;
  for (int attempt = 0; attempt < 600 && !watched; ++attempt) {
    const auto q = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("query", {{"q", "gamma"}}));
    watched = q && !(*q)["result"]["nodes"].empty();
    if (!watched) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, watched, "live watcher picked up new file without update op");

  // status reports the watcher and counts the applied update.
  const auto watch_status = request_with_retry(socket_path, cgraph::make_request("status"));
  expect(ok,
         watch_status && (*watch_status)["result"].value("watching", false) &&
             (*watch_status)["result"].value("incremental_updates", 0) >= 1,
         "status reports watcher and applied update");

  // The semantic overlay survives the code-only incremental rebuild (fragments
  // are re-overlaid after the rebuild publishes).
  bool overlay_kept = false;
  for (int attempt = 0; attempt < 200 && !overlay_kept; ++attempt) {
    const auto q = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("query", {{"q", "Topic"}}));
    overlay_kept = q && !(*q)["result"]["nodes"].empty();
    if (!overlay_kept) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, overlay_kept, "semantic overlay survived incremental rebuild");

  // The gitignored file never entered the graph (gamma arriving proves the same
  // watch window was processed).
  const auto hidden = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("query", {{"q", "hidden"}}));
  expect(ok, hidden && (*hidden)["result"]["nodes"].empty(), "gitignored file never entered the graph");

  // Incremental state is re-persisted: graph.json on disk gains the new symbol
  // once the persist interval elapses (no update op, no shutdown).
  bool persisted = false;
  for (int attempt = 0; attempt < 600 && !persisted; ++attempt) {
    std::ifstream input(root / "cgraph-out" / "graph.json");
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    persisted = contents.find("gamma") != std::string::npos;
    if (!persisted) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, persisted, "incremental state re-persisted to graph.json");

  // Wire hardening 1 (DoS): a 4-byte header declaring a ~4 GB body must be
  // rejected WITHOUT the daemon allocating that body, and the daemon must keep
  // serving. read_frame bails on any length above kMaxFrameBodyBytes before it
  // sizes the frame buffer; if it still allocated 0xFFFFFFFF bytes the process
  // would OOM/crash here instead of the next status answering.
  {
    const int fd = raw_connect(socket_path);
    expect(ok, fd >= 0, "raw connect for oversized-frame test");
    if (fd >= 0) {
      const std::array<std::uint8_t, 4> huge{0xFF, 0xFF, 0xFF, 0xFF};  // 0xFFFFFFFF body length
      (void)::write(fd, huge.data(), huge.size());
      ::close(fd);  // send no body: the daemon must not wait on ~4 GB it never allocates
    }
  }
  const auto after_huge = request_with_retry(socket_path, cgraph::make_request("status"));
  expect(ok, after_huge && (*after_huge)["ok"] == true,
         "daemon survived an oversized-length frame and still answers status");

  // Wire hardening 2 (hang): a client that sends a partial header and then goes
  // silent must not wedge the single-threaded serve loop. SO_RCVTIMEO drops the
  // stalled read after a few seconds; the proof is that a normal status on a
  // fresh connection still round-trips (the loop was never permanently blocked).
  {
    const int fd = raw_connect(socket_path);
    expect(ok, fd >= 0, "raw connect for stalled-reader test");
    if (fd >= 0) {
      const std::array<std::uint8_t, 2> partial{0x10, 0x00};  // 2 of 4 header bytes, then stall
      (void)::write(fd, partial.data(), partial.size());
      // Hold the socket open, sending nothing. The daemon serves inline on one
      // thread, so while it is stuck in read_exact on the missing header bytes it
      // cannot service another request -- until SO_RCVTIMEO (a few seconds) fires
      // and it drops the stalled peer. A fresh status must therefore eventually
      // succeed: the loop RECOVERS rather than wedging forever. Retry past the
      // timeout window (well over the 5s SO_RCVTIMEO) to prove recovery.
      std::optional<nlohmann::json> concurrent;
      for (int attempt = 0; attempt < 400 && !concurrent; ++attempt) {  // ~8s budget > 5s timeout
        concurrent = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("status"));
        if (!concurrent || !(*concurrent).value("ok", false)) {
          concurrent.reset();
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      }
      expect(ok, concurrent && (*concurrent)["ok"] == true,
             "daemon recovers and answers status after a stalled mid-frame reader is timed out");
      ::close(fd);
    }
  }
  const auto after_stall = request_with_retry(socket_path, cgraph::make_request("status"));
  expect(ok, after_stall && (*after_stall)["ok"] == true,
         "daemon serve loop survived a stalled half-frame reader");

  // An unknown op is an error, not a crash; shutdown stops the loop cleanly.
  const auto bad = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("nonsense"));
  expect(ok, bad && (*bad)["ok"] == false, "unknown op rejected");
  const int nodes_at_shutdown =
      watch_status ? (*watch_status)["result"].value("node_count", 0) : 0;
  const auto shutdown = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("shutdown"));
  expect(ok, shutdown && (*shutdown)["ok"] == true, "shutdown accepted");

  server.join();
  expect(ok, server_rc == 0, "first server exited cleanly");

  // Restart on the same root: the daemon fast-loads the persisted graph, so the
  // extraction index is NOT hydrated. The first live edit must trigger a
  // hydrating full rescan — a per-file rebuild here would collapse the graph to
  // just the changed file (regression coverage for exactly that bug).
  int server2_rc = -1;
  std::thread server2([&] { server2_rc = cgraph::run_daemon_server(root, options); });
  int nodes_loaded = 0;
  for (int attempt = 0; attempt < 600 && nodes_loaded < nodes_at_shutdown; ++attempt) {
    const auto s = request_with_retry(socket_path, cgraph::make_request("status"));
    nodes_loaded = s ? (*s)["result"].value("node_count", 0) : 0;
    if (nodes_loaded < nodes_at_shutdown) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, nodes_loaded >= nodes_at_shutdown, "fast-load restored the full graph");

  const auto fast_status = request_with_retry(socket_path, cgraph::make_request("status"));
  const auto fast_root = fast_status
                             ? (*fast_status)["result"]["freshness"].value("content_root", std::string{})
                             : std::string{};
  const auto fast_pinned = request_with_retry(
      socket_path,
      cgraph::make_request("query", {{"q", "beta"}, {"expected_content_root", fast_root}}));
  bool fast_source_verified = false;
  if (fast_pinned && (*fast_pinned).value("ok", false)) {
    const auto expected_beta_hash = cgraph::sha256_file_hex(root / "src" / "beta.ts");
    for (const auto& node : (*fast_pinned)["result"]["nodes"]) {
      if (node.value("source_sha256", std::string{}) == expected_beta_hash) {
        fast_source_verified = true;
        break;
      }
    }
  }
  expect(ok, !fast_root.empty() && fast_source_verified,
         "fast-load reconstructed source evidence for a pinned snippet read");

  write_file(root / "src" / "delta.ts", "export function delta() { return 3; }\n");
  bool delta_seen = false;
  for (int attempt = 0; attempt < 600 && !delta_seen; ++attempt) {
    const auto q = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("query", {{"q", "delta"}}));
    delta_seen = q && !(*q)["result"]["nodes"].empty();
    if (!delta_seen) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  expect(ok, delta_seen, "first edit after fast-load picked up live");
  const auto post_edit = request_with_retry(socket_path, cgraph::make_request("status"));
  const int nodes_post_edit = post_edit ? (*post_edit)["result"].value("node_count", 0) : 0;
  expect(ok, nodes_post_edit > nodes_at_shutdown, "post-restart edit grew the graph (no collapse)");

  const auto shutdown2 = cgraph::request_over_unix_socket(socket_path, cgraph::make_request("shutdown"));
  expect(ok, shutdown2 && (*shutdown2)["ok"] == true, "second shutdown accepted");
  server2.join();
  expect(ok, server2_rc == 0, "second server exited cleanly");

  // Verified update barrier: an equal-length source rewrite with its original
  // mtime restored must still produce a different root. Live watching is off
  // for this daemon, so the explicit update below is the only synchronizer.
  const auto freshness_root = fs::temp_directory_path() / "cgraph_daemon_freshness_test";
  fs::remove_all(freshness_root);
  const auto before = std::string{"export function alpha() { return 1; }\n"};
  const auto after = std::string{"export function omega() { return 1; }\n"};
  expect(ok, before.size() == after.size(), "freshness rewrite preserves source length");
  const auto freshness_source = freshness_root / "src" / "alpha.ts";
  write_file(freshness_source, before);
  const auto freshness_socket = cgraph::unix_socket_path(cgraph::daemon_identity_for(freshness_root));
  fs::remove(freshness_socket);

  cgraph::DaemonServerOptions freshness_options = options;
  freshness_options.code_poll_interval = std::chrono::milliseconds::zero();
  int freshness_rc = -1;
  std::thread freshness_server([&] { freshness_rc = cgraph::run_daemon_server(freshness_root, freshness_options); });

  const auto old_update = request_with_retry(freshness_socket, cgraph::make_request("update", {{"path", "."}}));
  const bool old_update_has_freshness = old_update && (*old_update).contains("result") &&
                                        (*old_update)["result"].contains("freshness");
  const auto old_root = old_update_has_freshness
                            ? (*old_update)["result"]["freshness"].value("content_root", std::string{})
                            : std::string{};
  expect(ok,
         old_update && (*old_update).value("ok", false) && old_update_has_freshness &&
             (*old_update)["result"]["freshness"].value("verified", false) &&
             (*old_update)["result"]["freshness"].value("algorithm", std::string{}) == "sha256-merkle-v1" &&
             !old_root.empty() && (*old_update)["result"].value("files_hashed", std::size_t{0}) == 1 &&
             (*old_update)["result"].value("bytes_hashed", std::size_t{0}) == before.size() &&
             (*old_update)["result"].contains("files_reextracted") &&
             (*old_update)["result"].contains("files_cache_hit") && (*old_update)["result"].contains("files_removed"),
         "verified update returns root identity and verification work counts");

  if (!old_root.empty()) {
    const auto preserved_mtime = fs::last_write_time(freshness_source);
    write_file(freshness_source, after);
    fs::last_write_time(freshness_source, preserved_mtime);

    const auto mixed_snapshot_attempt = request_with_retry(
        freshness_socket,
        cgraph::make_request("query", {{"q", "alpha"}, {"expected_content_root", old_root}}));
    const auto mismatch_error = mixed_snapshot_attempt
                                    ? mixed_snapshot_attempt->value("error", std::string{})
                                    : std::string{};
    expect(ok,
           mixed_snapshot_attempt && !mixed_snapshot_attempt->value("ok", true) &&
               !mixed_snapshot_attempt->contains("result") &&
               mismatch_error.find("source-snapshot-mismatch") != std::string::npos &&
               mismatch_error.find("synchronize again") != std::string::npos,
           "pinned read rejects old-graph/new-source bytes atomically before publication");

    const auto new_update = request_with_retry(freshness_socket, cgraph::make_request("update", {{"path", "."}}));
    const bool new_update_has_freshness = new_update && (*new_update).contains("result") &&
                                          (*new_update)["result"].contains("freshness");
    const auto new_root = new_update_has_freshness
                              ? (*new_update)["result"]["freshness"].value("content_root", std::string{})
                              : std::string{};
    expect(ok,
           new_update && (*new_update).value("ok", false) && new_update_has_freshness &&
               (*new_update)["result"]["freshness"].value("verified", false) &&
               (*new_update)["result"].value("files_hashed", std::size_t{0}) == 1 &&
               (*new_update)["result"].value("bytes_hashed", std::size_t{0}) == after.size() &&
               !new_root.empty() && new_root != old_root,
           "preserved-mtime rewrite produces a new verified root");

    const auto pinned_new = request_with_retry(
        freshness_socket,
        cgraph::make_request("query", {{"q", "omega"}, {"expected_content_root", new_root}}));
    bool new_source_verified = false;
    if (pinned_new && pinned_new->value("ok", false)) {
      for (const auto& node : (*pinned_new)["result"]["nodes"]) {
        if (node.value("source_sha256", std::string{}) == cgraph::sha256_hex(after)) {
          new_source_verified = true;
          break;
        }
      }
    }
    expect(ok,
           pinned_new && (*pinned_new).value("ok", false) && !(*pinned_new)["result"]["nodes"].empty() &&
               (*pinned_new)["result"]["freshness"].value("verified", false) &&
               (*pinned_new)["result"]["freshness"].value("content_root", std::string{}) == new_root &&
               new_source_verified,
           "new root pins a query to the updated snapshot");

    const auto stale_query = request_with_retry(
        freshness_socket,
        cgraph::make_request("query", {{"q", "omega"}, {"expected_content_root", old_root}}));
    expect(ok,
           stale_query && !(*stale_query).value("ok", true) && !(*stale_query).contains("result"),
           "old root is rejected without a graph result");
  }

  const auto freshness_shutdown =
      request_with_retry(freshness_socket, cgraph::make_request("shutdown"));
  expect(ok, freshness_shutdown && (*freshness_shutdown).value("ok", false), "freshness daemon shuts down cleanly");
  freshness_server.join();
  expect(ok, freshness_rc == 0, "freshness daemon exited cleanly");
  fs::remove_all(freshness_root);

  // Never-idle + stale-socket reclaim on a fresh root. (a) A daemon started with
  // idle_timeout == 0 must reclaim a stale (non-live) socket file left at its
  // endpoint by a crashed predecessor and bind anyway. (b) It must stay resident
  // well past what a positive timeout would have killed, because idle shutdown is
  // disabled — the property that lets the supervisor keep it alive indefinitely.
  const auto root2 = fs::temp_directory_path() / "cgraph_daemon_never_idle_test";
  fs::remove_all(root2);
  fs::create_directories(root2);
  write_file(root2 / "src" / "one.ts", "export function one() { return 1; }\n");
  const auto socket2 = cgraph::unix_socket_path(cgraph::daemon_identity_for(root2));
  cgraph::ensure_unix_socket_dir(socket2);
  write_file(socket2, "stale, not a live listener");  // stale endpoint from a "crash"

  cgraph::DaemonServerOptions immortal = options;
  immortal.idle_timeout = std::chrono::seconds::zero();  // never idle-shut-down
  immortal.build_graph_on_start = false;                 // serve immediately, no build wait
  int immortal_rc = -1;
  std::thread immortal_server([&] { immortal_rc = cgraph::run_daemon_server(root2, immortal); });

  const auto reclaimed = request_with_retry(socket2, cgraph::make_request("status"));
  expect(ok, reclaimed && (*reclaimed)["ok"] == true, "daemon reclaimed the stale socket and serves");

  // With a positive timeout the daemon would exit after idle_timeout of inactivity;
  // with 0 it must still answer after several idle select windows (200ms each).
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  const auto still_up = request_with_retry(socket2, cgraph::make_request("status"));
  expect(ok, still_up && (*still_up)["ok"] == true, "idle_timeout==0 keeps the daemon resident");

  const auto shutdown3 = cgraph::request_over_unix_socket(socket2, cgraph::make_request("shutdown"));
  expect(ok, shutdown3 && (*shutdown3)["ok"] == true, "never-idle daemon shuts down on explicit op");
  immortal_server.join();
  expect(ok, immortal_rc == 0, "never-idle server exited cleanly");
  fs::remove_all(root2);

  // Semantic lifecycle: dependency invalidation omits the whole overlay,
  // survives restart, and only a freshly fingerprinted replacement recovers it.
  {
    const auto sem_root = fs::temp_directory_path() / "cgraph_daemon_semantic_lifecycle_test";
    const auto code_source = sem_root / "src" / "target.ts";
    const auto guide_source = sem_root / "docs" / "guide.md";
    const auto drop_dir = sem_root / "cgraph-out" / "semantic-drop";
    const auto sem_frag = drop_dir / "chunk_00.json";
    const auto manifest = drop_dir / "plan.json";
    fs::remove_all(sem_root);
    write_file(code_source, "export class Target { run() { return 1; } }\n");
    write_file(guide_source, "# Guide\nReferences target\n");

    const auto sem_socket = cgraph::unix_socket_path(cgraph::daemon_identity_for(sem_root));
    fs::remove(sem_socket);
    cgraph::DaemonServerOptions sem_options;
    sem_options.idle_timeout = std::chrono::seconds(60);
    sem_options.build_graph_on_start = true;
    sem_options.code_poll_interval = std::chrono::milliseconds::zero();
    sem_options.persist_interval = std::chrono::seconds(1);

    const auto target_id_from = [](const std::optional<nlohmann::json>& response) {
      if (!response || !response->value("ok", false)) {
        return std::string{};
      }
      for (const auto& node : (*response)["result"]["nodes"]) {
        if (node.value("label", std::string{}) == "Target") {
          return node.value("id", std::string{});
        }
      }
      return std::string{};
    };
    const auto wait_for_enrichment = [&](std::string_view expected_state,
                                         std::size_t minimum_pending,
                                         std::size_t maximum_failed,
                                         int attempt_limit = 400) -> std::optional<nlohmann::json> {
      for (int attempt = 0; attempt < attempt_limit; ++attempt) {
        const auto observed = request_with_retry(sem_socket, cgraph::make_request("status"));
        if (observed && (*observed)["result"].value("enrichment_state", std::string{}) == expected_state &&
            (*observed)["result"].value("enrichment_pending", std::size_t{0}) >= minimum_pending &&
            (*observed)["result"].value("enrichment_failed", std::size_t{0}) <= maximum_failed) {
          return observed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      return std::nullopt;
    };

    int sem_rc = -1;
    std::thread sem_server([&] { sem_rc = cgraph::run_daemon_server(sem_root, sem_options); });
    std::string target_node_id;
    for (int attempt = 0; attempt < 300 && target_node_id.empty(); ++attempt) {
      target_node_id = target_id_from(request_with_retry(
          sem_socket, cgraph::make_request("query", {{"q", "Target"}})));
      if (target_node_id.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    expect(ok, !target_node_id.empty(), "sem-lifecycle: deterministic target is queryable");
    expect(ok, !wait_for_enrichment("never-matches", 0, 0, 1),
           "sem-lifecycle: enrichment wait rejects a non-matching status");

    write_file(
        manifest,
        R"({"chunks":[{"index":0,"inputs":[{"path":")" + guide_source.generic_string() +
            R"(","content_hash":")" + cgraph::sha256_file_hex(guide_source) + R"("}]}]})");
    write_file(
        sem_frag,
        R"({"nodes":[{"id":"doc:guide","label":"Guide","type":"document","source_file":")" +
            guide_source.generic_string() +
            R"("}],"edges":[{"source":"doc:guide","target":")" + target_node_id +
            R"(","relation":"MENTIONS"}],"hyperedges":[]})");

    bool guide_visible = false;
    for (int attempt = 0; attempt < 300 && !guide_visible; ++attempt) {
      const auto guide_query = cgraph::request_over_unix_socket(
          sem_socket, cgraph::make_request("query", {{"q", "Guide"}}));
      guide_visible = guide_query && !(*guide_query)["result"]["nodes"].empty();
      if (!guide_visible) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    expect(ok, guide_visible, "sem-lifecycle: dependency-bound overlay merged");

    fs::remove(code_source);
    const auto delete_update = request_with_retry(
        sem_socket, cgraph::make_request("update", {{"path", "."}}));
    expect(ok, delete_update && delete_update->value("ok", false),
           "sem-lifecycle: target deletion synchronized");
    const auto guide_omitted = request_with_retry(
        sem_socket, cgraph::make_request("query", {{"q", "Guide"}}));
    expect(ok, guide_omitted && (*guide_omitted)["result"]["nodes"].empty(),
           "sem-lifecycle: invalid overlay omitted after deterministic rebuild");
    const auto stale_status = wait_for_enrichment("stale", 1, 0);
    expect(ok, stale_status && (*stale_status)["result"].value("enrichment_pending", 0) >= 1,
           "sem-lifecycle: dependency invalidation is stale and requeued");

    const auto first_shutdown = cgraph::request_over_unix_socket(
        sem_socket, cgraph::make_request("shutdown"));
    expect(ok, first_shutdown && first_shutdown->value("ok", false),
           "sem-lifecycle: invalid state shutdown accepted");
    sem_server.join();
    expect(ok, sem_rc == 0, "sem-lifecycle: invalid state server exited cleanly");

    int stale_restart_rc = -1;
    std::thread stale_restart(
        [&] { stale_restart_rc = cgraph::run_daemon_server(sem_root, sem_options); });
    const auto restart_stale_status = wait_for_enrichment("stale", 1, 0);
    const auto restart_guide = request_with_retry(
        sem_socket, cgraph::make_request("query", {{"q", "Guide"}}));
    expect(ok, restart_stale_status && restart_guide &&
                   (*restart_guide)["result"]["nodes"].empty(),
           "sem-lifecycle: restart preserves stale health and omits invalid overlay");

    write_file(code_source, "export class Target { run() { return 2; } }\n");
    const auto restore_update = request_with_retry(
        sem_socket, cgraph::make_request("update", {{"path", "."}}));
    expect(ok, restore_update && restore_update->value("ok", false),
           "sem-lifecycle: changed target restored deterministically");
    const auto restored_target = request_with_retry(
        sem_socket, cgraph::make_request("query", {{"q", "Target"}}));
    const auto restored_target_id = target_id_from(restored_target);
    const auto still_omitted = request_with_retry(
        sem_socket, cgraph::make_request("query", {{"q", "Guide"}}));
    expect(ok, !restored_target_id.empty() && still_omitted &&
                   (*still_omitted)["result"]["nodes"].empty(),
           "sem-lifecycle: stale overlay stays omitted until host replacement");

    write_file(
        sem_frag,
        R"({"nodes":[{"id":"doc:guide","label":"Guide Recovered","type":"document","source_file":")" +
            guide_source.generic_string() +
            R"("}],"edges":[{"source":"doc:guide","target":")" + restored_target_id +
            R"(","relation":"MENTIONS"}],"hyperedges":[]})");
    bool recovered = false;
    for (int attempt = 0; attempt < 400 && !recovered; ++attempt) {
      const auto recovery_query = cgraph::request_over_unix_socket(
          sem_socket, cgraph::make_request("query", {{"q", "Guide Recovered"}}));
      recovered = recovery_query && !(*recovery_query)["result"]["nodes"].empty();
      if (!recovered) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    const auto recovered_status = wait_for_enrichment("idle", 0, 0);
    expect(ok, recovered && recovered_status &&
                   (*recovered_status)["result"].value("enrichment_stale", 1) == 0 &&
                   (*recovered_status)["result"].value("enrichment_failed", 1) == 0,
           "sem-lifecycle: valid replacement clears current error state atomically");

    const auto recovered_shutdown = cgraph::request_over_unix_socket(
        sem_socket, cgraph::make_request("shutdown"));
    expect(ok, recovered_shutdown && recovered_shutdown->value("ok", false),
           "sem-lifecycle: recovered state shutdown accepted");
    stale_restart.join();
    expect(ok, stale_restart_rc == 0, "sem-lifecycle: recovered state server exited cleanly");

    int recovered_restart_rc = -1;
    std::thread recovered_restart(
        [&] { recovered_restart_rc = cgraph::run_daemon_server(sem_root, sem_options); });
    bool stable_guide_visible = false;
    for (int attempt = 0; attempt < 400 && !stable_guide_visible; ++attempt) {
      const auto stable_guide = request_with_retry(
          sem_socket, cgraph::make_request("query", {{"q", "Guide Recovered"}}));
      stable_guide_visible = stable_guide && !(*stable_guide)["result"]["nodes"].empty();
      if (!stable_guide_visible) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    const auto stable_status = wait_for_enrichment("idle", 0, 0);
    expect(ok, stable_status && stable_guide_visible &&
                   (*stable_status)["result"].value("enrichment_stale", 1) == 0 &&
                   (*stable_status)["result"].value("enrichment_failed", 1) == 0,
           "sem-lifecycle: restart reproduces recovered overlay and health");
    const auto final_shutdown = cgraph::request_over_unix_socket(
        sem_socket, cgraph::make_request("shutdown"));
    expect(ok, final_shutdown && final_shutdown->value("ok", false),
           "sem-lifecycle: final shutdown accepted");
    recovered_restart.join();
    expect(ok, recovered_restart_rc == 0, "sem-lifecycle: final server exited cleanly");
    fs::remove_all(sem_root);
  }

  // The daemon keeps graph.json deterministic-only and rebuilds semantic
  // overlays from cache-valid attribution after every restart and live event.
  {
    const auto event_root = fs::temp_directory_path() / "cgraph_daemon_overlay_events_test";
    const auto code_source = event_root / "src" / "target.ts";
    const auto guide_source = event_root / "docs" / "guide.md";
    const auto drop_dir = event_root / "cgraph-out" / "semantic-drop";
    const auto graph_path = event_root / "cgraph-out" / "graph.json";
    const auto stat_index_path = drop_dir / "semantic-stat-index.json";
    const auto event_socket = cgraph::unix_socket_path(cgraph::daemon_identity_for(event_root));
    fs::remove_all(event_root);
    fs::remove(event_socket);
    write_file(code_source, "export class Target { run() { return 1; } }\n");
    write_file(guide_source, "# Guide\nOriginal target\n");

    cgraph::DaemonServerOptions event_options;
    event_options.idle_timeout = std::chrono::seconds(60);
    event_options.build_graph_on_start = true;
    event_options.code_poll_interval = std::chrono::milliseconds(40);
    event_options.watch_debounce = std::chrono::milliseconds(20);
    event_options.persist_interval = std::chrono::seconds(1);

    const auto write_plan = [&](std::size_t index) {
      write_file(
          drop_dir / "plan.json",
          R"({"chunks":[{"index":)" + std::to_string(index) +
              R"(,"inputs":[{"path":")" + guide_source.generic_string() +
              R"(","content_hash":")" + cgraph::sha256_file_hex(guide_source) +
              R"("}]}]})");
    };
    const auto write_overlay = [&](const fs::path& path,
                                   std::string_view id,
                                   std::string_view label,
                                   std::string_view target_id) {
      write_file(
          path,
          R"({"nodes":[{"id":")" + std::string(id) +
              R"(","label":")" + std::string(label) +
              R"(","type":"document","source_file":")" + guide_source.generic_string() +
              R"("}],"edges":[{"source":")" + std::string(id) +
              R"(","target":")" + std::string(target_id) +
              R"(","relation":"MENTIONS"}],"hyperedges":[]})");
    };
    const auto shutdown_server = [&](std::thread& thread, int& rc, const char* accepted, const char* exited) {
      const auto response = request_with_retry(event_socket, cgraph::make_request("shutdown"));
      expect(ok, response && response->value("ok", false), accepted);
      thread.join();
      expect(ok, rc == 0, exited);
    };
    const auto persisted_graph_has_overlay = [&]() {
      std::ifstream input(graph_path, std::ios::binary);
      const auto graph = nlohmann::json::parse(input, nullptr, false);
      if (!graph.is_object()) {
        return true;
      }
      for (const auto& node : graph.value("nodes", nlohmann::json::array())) {
        if (node.value("id", std::string{}).starts_with("doc:event-") ||
            node.value("id", std::string{}) == "doc:orphan-live" ||
            node.value("id", std::string{}).starts_with("memory:")) {
          return true;
        }
      }
      for (const auto& edge : graph.value("links", nlohmann::json::array())) {
        if (edge.value("source", std::string{}).starts_with("doc:event-") ||
            edge.value("source", std::string{}) == "doc:orphan-live" ||
            edge.value("source", std::string{}).starts_with("memory:") ||
            edge.value("target", std::string{}).starts_with("memory:")) {
          return true;
        }
      }
      return false;
    };

    int event_rc = -1;
    std::thread event_server([&] { event_rc = cgraph::run_daemon_server(event_root, event_options); });
    const auto target_id = wait_for_node_id(event_socket, "Target", "Target");
    expect(ok, !target_id.empty(), "overlay-events: deterministic target became queryable");
    write_plan(0);
    write_overlay(drop_dir / "chunk_00.json", "doc:event-old", "Overlay Original", target_id);
    expect(ok, wait_for_label(event_socket, "Overlay Original", "Overlay Original", true),
           "overlay-events: initial attributed drop merged live");
    const auto remembered = request_with_retry(
        event_socket,
        cgraph::make_request(
            "remember",
            {{"title", "Overlay memory"}, {"body", "survives overlay rebuilds"}, {"touches", {target_id}}}));
    expect(ok, remembered && remembered->value("ok", false),
           "overlay-events: memory checkpoint created before persistence");
    shutdown_server(
        event_server,
        event_rc,
        "overlay-events: initial shutdown accepted",
        "overlay-events: initial daemon exited cleanly");
    expect(ok, !persisted_graph_has_overlay(),
           "overlay-events: graph.json excluded semantic nodes and edges");

    // A semantic source changed while the daemon was down. The code manifest is
    // still unchanged, so startup takes the persisted fast path, reconciles the
    // cache against current bytes, and must not leak the old fused overlay.
    write_file(guide_source, "# Guide\nOffline source change\n");
    int source_restart_rc = -1;
    std::thread source_restart(
        [&] { source_restart_rc = cgraph::run_daemon_server(event_root, event_options); });
    expect(ok, !wait_for_node_id(event_socket, "Target", "Target").empty(),
           "overlay-events: source-change restart fast-loaded deterministic code");
    expect(ok, wait_for_label(event_socket, "Overlay Original", "Overlay Original", false),
           "overlay-events: offline semantic-source change omitted cached overlay");
    const auto source_restart_memory = request_with_retry(
        event_socket, cgraph::make_request("recall", {}));
    expect(ok,
           source_restart_memory && source_restart_memory->value("ok", false) &&
               (*source_restart_memory)["result"].value("total", 0) == 1,
           "overlay-events: fast-load replayed memory from its sidecar");

    write_plan(0);
    write_overlay(drop_dir / "chunk_00.json", "doc:event-old", "Overlay Source Restored", target_id);
    expect(ok, wait_for_label(event_socket, "Overlay Source Restored", "Overlay Source Restored", true),
           "overlay-events: live source replacement restored a valid overlay");
    shutdown_server(
        source_restart,
        source_restart_rc,
        "overlay-events: source-recovery shutdown accepted",
        "overlay-events: source-recovery daemon exited cleanly");

    // Only the fragment bytes change offline this time. Fast-load must reconcile
    // its fingerprint before replay rather than publishing the prior overlay.
    write_overlay(drop_dir / "chunk_00.json", "doc:event-old", "Overlay Offline Tamper", target_id);
    int fragment_restart_rc = -1;
    std::thread fragment_restart(
        [&] { fragment_restart_rc = cgraph::run_daemon_server(event_root, event_options); });
    expect(ok, !wait_for_node_id(event_socket, "Target", "Target").empty(),
           "overlay-events: fragment-change restart fast-loaded deterministic code");
    expect(ok, wait_for_label(event_socket, "Overlay Source Restored", "Overlay Source Restored", false) &&
                   wait_for_label(event_socket, "Overlay Offline Tamper", "Overlay Offline Tamper", false),
           "overlay-events: offline fragment change omitted old and tampered overlays");

    write_overlay(drop_dir / "chunk_00.json", "doc:event-old", "Overlay Before Move", target_id);
    expect(ok, wait_for_label(event_socket, "Overlay Before Move", "Overlay Before Move", true),
           "overlay-events: valid same-path replacement merged live");

    // The host assigns the same semantic source a new fragment path while the
    // old file remains. Rebuilding from deterministic state must remove the old
    // node and its edge instead of leaving first-occurrence-wins residue.
    write_plan(1);
    write_overlay(drop_dir / "chunk_01.json", "doc:event-new", "Overlay After Move", target_id);
    expect(ok, wait_for_label(event_socket, "Overlay After Move", "Overlay After Move", true),
           "overlay-events: replacement in a new fragment path merged live");
    const auto old_explain = request_with_retry(
        event_socket, cgraph::make_request("explain", {{"id", "doc:event-old"}}));
    expect(ok,
           wait_for_label(event_socket, "Overlay Before Move", "Overlay Before Move", false) &&
               old_explain && !(*old_explain)["result"].value("found", true),
           "overlay-events: new-path replacement removed the old node and edge endpoint");

    // A live document edit invalidates its overlay immediately, even before the
    // asynchronous planner reports its next batch.
    write_file(guide_source, "# Guide\nLive edit target\n");
    expect(ok, wait_for_label(event_socket, "Overlay After Move", "Overlay After Move", false),
           "overlay-events: live document edit removed the invalid overlay");

    write_plan(2);
    write_overlay(drop_dir / "chunk_02.json", "doc:event-delete", "Overlay Before Delete", target_id);
    expect(ok, wait_for_label(event_socket, "Overlay Before Delete", "Overlay Before Delete", true),
           "overlay-events: post-edit replacement merged live");

    fs::remove(guide_source);
    expect(ok, wait_for_label(event_socket, "Overlay Before Delete", "Overlay Before Delete", false),
           "overlay-events: live document deletion removed the invalid overlay");
    bool deleted_stat_evicted = false;
    for (int attempt = 0; attempt < 500 && !deleted_stat_evicted; ++attempt) {
      std::ifstream input(stat_index_path, std::ios::binary);
      const auto stat_json = nlohmann::json::parse(input, nullptr, false);
      if (stat_json.is_object()) {
        deleted_stat_evicted = true;
        for (const auto& entry : stat_json.value("entries", nlohmann::json::array())) {
          if (entry.value("path", std::string{}) == guide_source.generic_string()) {
            deleted_stat_evicted = false;
          }
        }
      }
      if (!deleted_stat_evicted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    expect(ok, deleted_stat_evicted,
           "overlay-events: live document deletion evicted the persisted stat-index entry");
    const auto deletion_memory = request_with_retry(
        event_socket, cgraph::make_request("recall", {}));
    expect(ok,
           deletion_memory && deletion_memory->value("ok", false) &&
               (*deletion_memory)["result"].value("total", 0) == 1,
           "overlay-events: document deletion rebuilt semantic overlay and preserved memory");

    // A newly arriving unattributed drop may still validate live, but it has no
    // durable semantic-source attribution and therefore must not replay.
    write_file(
        drop_dir / "chunk_99.json",
        R"({"nodes":[{"id":"doc:orphan-live","label":"Orphan Live","type":"document"}],"edges":[],"hyperedges":[]})");
    expect(ok, wait_for_label(event_socket, "Orphan Live", "Orphan Live", true),
           "overlay-events: unattributed new drop validated live");
    shutdown_server(
        fragment_restart,
        fragment_restart_rc,
        "overlay-events: event daemon shutdown accepted",
        "overlay-events: event daemon exited cleanly");
    expect(ok, !persisted_graph_has_overlay(),
           "overlay-events: final graph.json remained deterministic-only");

    int orphan_restart_rc = -1;
    std::thread orphan_restart(
        [&] { orphan_restart_rc = cgraph::run_daemon_server(event_root, event_options); });
    expect(ok, !wait_for_node_id(event_socket, "Target", "Target").empty(),
           "overlay-events: orphan restart fast-loaded deterministic code");
    expect(ok, wait_for_label(event_socket, "Orphan Live", "Orphan Live", false),
           "overlay-events: replay skipped the unattributed orphan fragment");
    shutdown_server(
        orphan_restart,
        orphan_restart_rc,
        "overlay-events: orphan restart shutdown accepted",
        "overlay-events: orphan restart daemon exited cleanly");
    fs::remove_all(event_root);
  }

  // Coverage + refusal contract. Two regressions from review:
  //  - a cold-start rescan builds through a staging state; its unextracted
  //    coverage map must still reach the served status payload.
  //  - an update that finds a verified source unreadable must refuse visibly
  //    (accepted=false, warnings) and must NOT re-derive the deterministic
  //    persistence snapshot from the fused live graph -- graph.json stays
  //    overlay-free.
  {
    const auto cov_root = fs::temp_directory_path() / "cgraph_daemon_coverage_refusal_test";
    const auto cov_socket = cgraph::unix_socket_path(cgraph::daemon_identity_for(cov_root));
    fs::remove_all(cov_root);
    fs::remove(cov_socket);
    const auto cov_source = cov_root / "src" / "cov.py";
    write_file(cov_source, "class Covered:\n    pass\n");
    write_file(cov_root / "view.blade.php", "<div>{{ $x }}</div>\n");

    cgraph::DaemonServerOptions cov_options;
    cov_options.idle_timeout = std::chrono::seconds(60);
    cov_options.build_graph_on_start = true;
    cov_options.code_poll_interval = std::chrono::milliseconds(0);
    cov_options.drop_poll_interval = std::chrono::milliseconds(20);

    int cov_rc = -1;
    std::thread cov_server([&] { cov_rc = cgraph::run_daemon_server(cov_root, cov_options); });
    expect(ok, wait_for_label(cov_socket, "Covered", "Covered", true),
           "coverage: initial build published the deterministic graph");

    const auto cov_status = request_with_retry(cov_socket, cgraph::make_request("status"));
    expect(ok,
           cov_status && cov_status->value("ok", false) &&
               (*cov_status)["result"].value("unextracted", nlohmann::json::object())
                       .value("php-blade", 0) == 1,
           "coverage: status.unextracted reports php-blade after a cold-start rescan");

    // Overlay a session-memory checkpoint so the fused live graph and the
    // deterministic persistence snapshot genuinely differ.
    const auto remember = request_with_retry(
        cov_socket,
        cgraph::make_request(
            "remember", {{"title", "coverage checkpoint"}, {"summary", "refusal regression"}}));
    expect(ok, remember && remember->value("ok", false), "coverage: remember accepted");

    expect(ok, ::chmod(cov_source.c_str(), 0) == 0, "coverage: made verified source unreadable");
    const auto refused = request_with_retry(cov_socket, cgraph::make_request("update", {{"path", "."}}));
    expect(ok,
           refused && refused->value("ok", false) &&
               !(*refused)["result"].value("accepted", true) &&
               !(*refused)["result"].value("warnings", nlohmann::json::array()).empty(),
           "coverage: update refused with warnings while a verified source is unreadable");
    expect(ok, wait_for_label(cov_socket, "Covered", "Covered", true),
           "coverage: served graph kept the last verified code after the refusal");

    expect(ok, ::chmod(cov_source.c_str(), 0600) == 0, "coverage: restored source permissions");
    const auto cov_shutdown = request_with_retry(cov_socket, cgraph::make_request("shutdown"));
    expect(ok, cov_shutdown && cov_shutdown->value("ok", false), "coverage: shutdown accepted");
    cov_server.join();
    expect(ok, cov_rc == 0, "coverage: daemon exited cleanly");

    std::ifstream persisted(cov_root / "cgraph-out" / "graph.json", std::ios::binary);
    const auto persisted_graph = nlohmann::json::parse(persisted, nullptr, false);
    bool overlay_in_persisted = !persisted_graph.is_object();
    bool code_in_persisted = false;
    for (const auto& node : persisted_graph.value("nodes", nlohmann::json::array())) {
      const auto id = node.value("id", std::string{});
      if (id.starts_with("memory:")) {
        overlay_in_persisted = true;
      }
      if (node.value("label", std::string{}) == "Covered") {
        code_in_persisted = true;
      }
    }
    expect(ok, !overlay_in_persisted && code_in_persisted,
           "coverage: graph.json stayed deterministic-only across the refused update");

    fs::remove_all(cov_root);
  }

  fs::remove_all(root);
  return ok ? 0 : 1;
}
