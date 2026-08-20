#include "cgraph/daemon_server.hpp"

#include "cgraph/daemon_endpoint.hpp"
#include "cgraph/daemon_identity.hpp"
#include "cgraph/daemon_lifecycle.hpp"
#include "cgraph/configured_extractors.hpp"
#include "cgraph/daemon_ops.hpp"
#include "cgraph/file_cache.hpp"
#include "cgraph/incremental_update.hpp"
#include "cgraph/index_persistence.hpp"
#include "cgraph/operation_stats.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/semantic_cache.hpp"
#include "cgraph/semantic_chunk_plan.hpp"
#include "cgraph/semantic_drop.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/semantic_fragment_validation.hpp"
#include "cgraph/semantic_ingest.hpp"
#include "cgraph/semantic_orchestration.hpp"

#include "cgraph/file_watcher.hpp"

#include <unordered_map>
#include <unordered_set>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace cgraph {
namespace {

#ifndef _WIN32

// Per-connection I/O timeout. The daemon serves each connection inline on its
// single serve-loop thread, so a client that stalls mid-frame would otherwise
// freeze queries, the watcher poll, persistence, and idle shutdown until it
// disconnects. A few seconds is generous for the local one-shot request/response
// the thin client makes; a slower peer is dropped and the serve loop continues.
constexpr std::chrono::seconds kConnectionIoTimeout{5};

// Applies SO_RCVTIMEO + SO_SNDTIMEO to an accepted connection so a blocking
// read/write cannot hang the single serve thread indefinitely. Best-effort:
// a failed setsockopt only means the fallback (no timeout) for that one socket,
// which is logged; it never aborts serving.
void apply_connection_timeout(int conn) {
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(kConnectionIoTimeout.count());
  tv.tv_usec = 0;
  if (::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
      ::setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
    std::cerr << "graphd: setsockopt(timeout) failed: " << std::strerror(errno) << '\n';
  }
}

// Fills a sockaddr_un for `path`. Returns false if the path is too long for the
// platform's sun_path (a hard limit, ~104 bytes on macOS) — a clear failure
// rather than a silently truncated, wrong endpoint.
[[nodiscard]] bool fill_sockaddr(sockaddr_un& addr, const std::string& path) {
  if (path.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  return true;
}

[[nodiscard]] bool write_all(int fd, const std::uint8_t* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const auto written = ::write(fd, data + sent, size - sent);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

[[nodiscard]] bool read_exact(int fd, std::uint8_t* data, std::size_t size) {
  std::size_t received = 0;
  while (received < size) {
    const auto got = ::read(fd, data + received, size - received);
    if (got == 0) {
      return false;  // peer closed early
    }
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    received += static_cast<std::size_t>(got);
  }
  return true;
}

// Reads one length-prefixed frame (4-byte little-endian length + body) and
// decodes it, reusing the shared protocol codec.
[[nodiscard]] std::optional<nlohmann::json> read_frame(int fd) {
  std::array<std::uint8_t, 4> header{};
  if (!read_exact(fd, header.data(), header.size())) {
    return std::nullopt;
  }
  const auto length =
      static_cast<std::uint32_t>(header[0]) |
      (static_cast<std::uint32_t>(header[1]) << 8U) |
      (static_cast<std::uint32_t>(header[2]) << 16U) |
      (static_cast<std::uint32_t>(header[3]) << 24U);
  // Validate the declared body length BEFORE allocating: a hostile 4-byte
  // header (e.g. 0xFFFFFFFF) would otherwise force a ~4 GB allocation. Reject
  // an oversized frame as a protocol error and read no body, so a single bad
  // header cannot exhaust memory or wedge the single-threaded serve loop.
  if (length > kMaxFrameBodyBytes) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> frame(static_cast<std::size_t>(length) + 4);
  std::memcpy(frame.data(), header.data(), header.size());
  if (length > 0 && !read_exact(fd, frame.data() + 4, length)) {
    return std::nullopt;
  }
  return decode_frame(frame);
}

[[nodiscard]] bool write_frame(int fd, const nlohmann::json& payload) {
  const auto frame = encode_frame(payload);
  return write_all(fd, frame.data(), frame.size());
}

#endif  // !_WIN32

}  // namespace

#ifdef _WIN32

int run_daemon_server(const std::filesystem::path& root, DaemonServerOptions) {
  (void)root;
  std::cerr << "graphd: the Unix-socket server is not implemented on Windows yet\n";
  return 1;
}

std::optional<nlohmann::json> request_over_unix_socket(const std::filesystem::path&, const nlohmann::json&) {
  return std::nullopt;
}

#else

namespace {

// Sentinel returned by open_listen_socket when a healthy daemon already serves
// this root: the caller should exit cleanly (0) rather than treat it as an error.
constexpr int kEndpointAlreadyServed = -2;

// Open a Unix-domain listening socket at socket_path. If a healthy daemon already
// serves this root, returns kEndpointAlreadyServed without touching the endpoint.
// Otherwise clears any stale endpoint from a crashed daemon and binds. Returns the
// listen fd, or -1 on failure (already logged).
// Shared by the full build-and-watch server and the static seam server.
[[nodiscard]] int open_listen_socket(const std::filesystem::path& socket_path) {
  ensure_unix_socket_dir(socket_path);
  // Never unlink a live endpoint out from under a running daemon: probe first so
  // a start race (supervisor + MCP auto-spawn) defers instead of stealing it.
  if (unix_endpoint_is_live(socket_path)) {
    std::cerr << "graphd: already serving " << socket_path << ", deferring to the resident daemon\n";
    return kEndpointAlreadyServed;
  }
  ::unlink(socket_path.c_str());

  const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    std::cerr << "graphd: socket() failed: " << std::strerror(errno) << '\n';
    return -1;
  }
  sockaddr_un addr{};
  if (!fill_sockaddr(addr, socket_path.string())) {
    std::cerr << "graphd: socket path too long: " << socket_path << '\n';
    ::close(listen_fd);
    return -1;
  }
  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "graphd: bind() failed: " << std::strerror(errno) << '\n';
    ::close(listen_fd);
    return -1;
  }
  if (::listen(listen_fd, 16) != 0) {
    std::cerr << "graphd: listen() failed: " << std::strerror(errno) << '\n';
    ::close(listen_fd);
    (void)cleanup_daemon_endpoint(socket_path);
    return -1;
  }
  return listen_fd;
}

}  // namespace

// A static, read-only daemon serving a pre-fused seam graph: load graph.json,
// publish the snapshot, and answer the read ops via handle_daemon_request -- no
// build, no watcher, no persistence, no enrichment. `update` reloads graph.json
// (re-fuse -> update refreshes); writes are rejected (no memory_dir). Selected by
// graphd when the root carries a seam marker (see is_seam_directory).
int run_static_seam_server(const std::filesystem::path& root, DaemonServerOptions options) {
  const auto identity = daemon_identity_for(root);
  const auto socket_path = unix_socket_path(identity);
  const int listen_fd = open_listen_socket(socket_path);
  if (listen_fd == kEndpointAlreadyServed) {
    return 0;  // a resident daemon already serves this root; defer cleanly
  }
  if (listen_fd < 0) {
    return 1;
  }

  DaemonState state;
  state.pid = ::getpid();
  const auto graph_path = root / "graph.json";
  if (!load_graph_snapshot(state, graph_path)) {
    std::cerr << "graphd: failed to load seam graph: " << graph_path << '\n';
    ::close(listen_fd);
    (void)cleanup_daemon_endpoint(socket_path);
    return 1;
  }
  state.update_handler = [&state, graph_path](const nlohmann::json&) -> nlohmann::json {
    if (load_graph_snapshot(state, graph_path)) {
      const auto graph = read_graph_snapshot(state);
      return {{"reloaded", true},
              {"nodes", graph->nodes.size()},
              {"edges", graph->edges.size()},
              {"freshness", freshness_metadata(*graph)}};
    }
    return {{"reloaded", false}};
  };
  std::cerr << "graphd: serving static seam graph " << graph_path << " ("
            << read_graph_snapshot(state)->nodes.size() << " nodes)\n";

  auto last_activity = FileWatcherClock::now();
  while (!state.shutdown_requested) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listen_fd, &read_set);
    timeval timeout{};
    timeout.tv_sec = 1;
    const int ready = ::select(listen_fd + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      // A non-positive idle timeout disables idle shutdown entirely: the daemon
      // stays resident (and watching) until an explicit shutdown op or signal.
      if (options.idle_timeout > std::chrono::seconds::zero() &&
          FileWatcherClock::now() - last_activity >= options.idle_timeout) {
        std::cerr << "graphd: idle timeout, shutting down\n";
        break;
      }
      continue;
    }
    const int conn = ::accept(listen_fd, nullptr, nullptr);
    if (conn < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    last_activity = FileWatcherClock::now();
    apply_connection_timeout(conn);  // a stalled peer must not wedge the serve loop
    if (const auto request = read_frame(conn)) {
      const auto response = handle_daemon_request(state, *request);
      (void)write_frame(conn, response);
    }
    ::close(conn);
  }
  ::close(listen_fd);
  (void)cleanup_daemon_endpoint(socket_path);
  return 0;
}

int run_daemon_server(const std::filesystem::path& root, DaemonServerOptions options) {
  const auto identity = daemon_identity_for(root);
  const auto socket_path = unix_socket_path(identity);
  const int listen_fd = open_listen_socket(socket_path);
  if (listen_fd == kEndpointAlreadyServed) {
    return 0;  // a resident daemon already serves this root; defer cleanly,
               // leaving its socket and snapshots untouched
  }
  if (listen_fd < 0) {
    return 1;
  }

  DaemonState state;
  state.pid = ::getpid();

  // Serve immediately. Publish an empty graph so status/query answer right away
  // (build_state Empty, node_count 0) while the initial build runs on a worker
  // thread — otherwise a large repo's multi-minute cold build blocks every
  // connection, including a trivial `status`, until it finishes.
  publish_graph_snapshot(state, GraphSnapshot{});

  // Serializes graph-mutating work (the initial build, `update` rescans, and
  // live fragment ingestion) so the build thread and the serve loop never race
  // on the file index or semantic cache. Read-only ops (status/query/explain/
  // path/context) take only the snapshot lock and stay responsive during a build.
  std::mutex graph_mutex;

  // Semantic enrichment: the daemon watches a drop directory for host-computed
  // chunk_NN.json fragments, merges valid ones into the live snapshot through
  // the single-writer path, and surfaces progress via the enrichment_* status
  // fields. Source attribution for each fragment comes from the plan manifest.
  const auto drop_dir = options.drop_dir.empty()
                            ? default_semantic_drop_dir(identity.project_root / "cgraph-out")
                            : options.drop_dir;
  const auto cache_path = drop_dir / "semantic-cache.json";
  // Session-memory checkpoint sidecars live here, separate from semantic-drop, so
  // the two host-authored layers are overlaid and managed independently.
  const auto memory_dir = identity.project_root / "cgraph-out" / "memory";
  SemanticCache cache = read_semantic_cache(cache_path);
  // Stat cache for the enrichment plan walk: lets a refresh reuse stored hashes
  // for unchanged docs/media instead of re-reading and re-hashing every file.
  // Persisted next to the semantic cache so a restart stays cheap.
  const auto stat_index_path = drop_dir / "semantic-stat-index.json";
  SemanticStatIndex stat_index = read_semantic_stat_index(stat_index_path);
  // Guards refresh snapshots against a live doc/media event that evicts an
  // entry while the planner is hashing off-lock. A stale worker result is
  // discarded instead of restoring the evicted hash after the event rebuild.
  std::uint64_t stat_index_revision = 0;
  SemanticFragmentDropWatcher drop_watcher(drop_dir);

  // Enrichment planning (plan_semantic_chunks) walks the whole project and
  // hashes every doc/media file — seconds on a large repo. It only produces
  // informational status counts, so it must not block the build/update response.
  // It runs on a dedicated worker: callers signal request_refresh() (cheap) and
  // return; the worker snapshots the cache under graph_mutex, plans OFF-lock
  // (never blocking builds), then writes the counts back under graph_mutex.
  std::mutex refresh_mutex;
  std::condition_variable refresh_cv;
  bool refresh_requested = false;
  bool refresh_stop = false;

  const auto run_enrichment_refresh = [&]() {
    SemanticCache snapshot;
    SemanticStatIndex index_snapshot;
    std::uint64_t index_revision = 0;
    {
      const std::scoped_lock lock(graph_mutex);
      snapshot = cache;              // quick copies; release before the slow walk
      index_snapshot = stat_index;
      index_revision = stat_index_revision;
    }
    SemanticChunkPlanOptions plan_options;
    plan_options.excluded_dirs = {drop_dir};
    if (drop_dir.has_parent_path()) {
      plan_options.excluded_dirs.push_back(drop_dir.parent_path());
    }
    // The plan reuses stored hashes for unchanged files (StatHit) and updates the
    // snapshot index in place with what it saw this pass.
    const auto plan = plan_semantic_chunks(identity.project_root, snapshot, plan_options, &index_snapshot);
    std::size_t pending = 0;
    for (const auto& chunk : plan.chunks) {
      pending += chunk.inputs.size();
    }
    {
      const std::scoped_lock lock(graph_mutex);
      if (index_revision != stat_index_revision) {
        // A watcher event evicted an entry while this plan ran. Requeue from the
        // newer index and do not publish stale counts or stale persisted hashes.
        {
          const std::scoped_lock refresh_lock(refresh_mutex);
          refresh_requested = true;
        }
        refresh_cv.notify_one();
        return;
      }
      stat_index = index_snapshot;
      // Serialize the file write with watcher eviction. Otherwise an older
      // worker could overwrite the just-evicted index after releasing the lock.
      if (plan.files_hashed > 0) {
        write_semantic_stat_index(stat_index, stat_index_path);
      }
    }
    {
      // Derive enrichment health from current cache state + plan, replacing
      // cumulative counters with a point-in-time snapshot.
      const std::scoped_lock lock(state.enrichment_mutex);
      ++state.enrichment_plans_run;
      state.enrichment_pending = pending;
      state.enrichment_stale = plan.stale_inputs;
      state.enrichment_failed = plan.failed_inputs;
      state.enrichment_state = state.enrichment_failed > 0  ? EnrichmentState::Failed
                               : state.enrichment_stale > 0 ? EnrichmentState::Stale
                               : pending > 0                ? EnrichmentState::Pending
                                                            : EnrichmentState::Idle;
    }
  };

  const auto request_refresh = [&]() {
    {
      const std::scoped_lock lock(refresh_mutex);
      refresh_requested = true;
    }
    refresh_cv.notify_one();
  };

  // The incremental index is also the canonical source of exact code hashes
  // used to fingerprint semantic dependencies during ingest and replay.
  IncrementalGraphIndex index;

  // Canonical code-only snapshot. Every deterministic rebuild/load replaces it;
  // every semantic or memory publication starts from a copy. graph.json is
  // written only from this snapshot, never from the fused live graph.
  GraphSnapshot deterministic_graph;

  const auto live_sources_for = [&](const SemanticFragmentDrop& drop,
                                    const std::unordered_map<std::size_t,
                                                             std::vector<SemanticSourceInput>>& sources) {
    if (const auto entry = sources.find(drop.chunk_index);
        entry != sources.end() && !entry->second.empty()) {
      return entry->second;
    }

    std::vector<SemanticSourceInput> source_inputs;
    for (const auto& record : cache.find_for_fragment(drop.path)) {
      source_inputs.push_back(SemanticSourceInput{
          .path = record.source_path,
          .content_sha256 = record.content_hash,
      });
    }
    if (source_inputs.empty()) {
      // A genuinely new live drop may validate before the host has published a
      // plan. Its self-attribution is intentionally non-replayable below.
      source_inputs.push_back(SemanticSourceInput{
          .path = drop.path,
          .content_sha256 = {},
      });
    }
    return source_inputs;
  };

  // Replays only a fragment with durable source attribution whose entire cache
  // unit is currently valid. A plan-attributed first ingest remains allowed;
  // a self-attributed live orphan never becomes restart-replayable.
  const auto replay_drop = [&](DaemonState& target,
                               const SemanticFragmentDrop& drop,
                               const std::unordered_map<std::size_t,
                                                        std::vector<SemanticSourceInput>>& sources) {
    std::vector<SemanticSourceInput> source_inputs;
    const auto records = cache.find_for_fragment(drop.path);
    if (!records.empty()) {
      source_inputs.reserve(records.size());
      for (const auto& record : records) {
        if (record.state != SemanticCacheState::Valid ||
            normalize_semantic_source_path(record.source_path) ==
                normalize_semantic_source_path(drop.path)) {
          return false;
        }
        source_inputs.push_back(SemanticSourceInput{
            .path = record.source_path,
            .content_sha256 = record.content_hash,
        });
      }
    } else {
      const auto entry = sources.find(drop.chunk_index);
      if (entry == sources.end() || entry->second.empty()) {
        return false;
      }
      source_inputs = entry->second;
      for (const auto& source : entry->second) {
        const auto rec = cache.find_for_source(source.path);
        if (rec.has_value() && rec->state != SemanticCacheState::Valid) {
          return false;
        }
      }
    }

    return ingest_semantic_fragment(target, cache, source_inputs, drop.path).merged;
  };

  // Re-overlays every session-memory checkpoint sidecar (cgraph-out/memory/*.json).
  // Memory nodes are snapshot-only and a rebuild (from index.files) drops them; the
  // sidecars are the durable source of truth, re-merged here after every rebuild so
  // checkpoints survive restarts, incremental edits, and full rescans. merge_fragment
  // is first-occurrence-wins, so re-applying an already-present checkpoint is a no-op.
  const auto ingest_all_memory = [&](DaemonState& target) {
    std::error_code ec;
    std::size_t applied = 0;
    if (!std::filesystem::exists(memory_dir, ec)) {
      target.last_memory_overlay_count = 0;
      return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(memory_dir, ec)) {
      if (ec) {
        break;
      }
      if (entry.path().extension() != ".json") {
        continue;
      }
      auto validation = validate_semantic_fragment_file(entry.path());
      if (!validation.valid) {
        continue;
      }
      std::unordered_map<std::string, std::string> overlay_hashes;
      overlay_hashes[entry.path().lexically_normal().generic_string()] = validation.source_sha256;
      for (const auto& node : validation.fragment.nodes) {
        if (node.source_file.empty()) {
          continue;
        }
        const auto source_path = std::filesystem::path{node.source_file};
        std::error_code source_error;
        if (std::filesystem::exists(source_path, source_error) && !source_error) {
          overlay_hashes[source_path.lexically_normal().generic_string()] = sha256_file_hex(source_path);
        }
      }
      mutate_graph_snapshot(target, [&](GraphSnapshot& graph) {
        merge_fragment(graph, validation.fragment);
        for (const auto& [path, hash] : overlay_hashes) {
          graph.source_hashes[path] = hash;
        }
      });
      ++applied;
    }
    target.last_memory_overlay_count = applied;  // observability: size of the last re-overlay pass
  };

  // Publish one coherent fused graph from the canonical deterministic snapshot.
  // Live replacements first detach their source records from any prior fragment,
  // so replay cannot reintroduce the old path before the new drop is validated.
  const auto rebuild_final_overlay = [&] (
      const std::vector<SemanticFragmentDropEvent>* live_events = nullptr) {
    const auto sources = load_chunk_sources(drop_dir);
    struct LiveDrop {
      SemanticFragmentDrop drop;
      std::vector<SemanticSourceInput> source_inputs;
    };
    std::vector<LiveDrop> pending_live;
    std::unordered_set<std::string> event_paths;
    if (live_events != nullptr) {
      pending_live.reserve(live_events->size());
      event_paths.reserve(live_events->size());
      for (const auto& event : *live_events) {
        event_paths.insert(normalize_semantic_source_path(event.drop.path));
        if (event.change == SemanticFragmentDropChange::Deleted) {
          continue;
        }
        auto source_inputs = live_sources_for(event.drop, sources);
        for (const auto& source : source_inputs) {
          cache.remove(source.path);
        }
        pending_live.push_back(LiveDrop{
            .drop = event.drop,
            .source_inputs = std::move(source_inputs),
        });
      }
    }

    // Stage the fused graph invisibly: seed a private state with the canonical
    // deterministic snapshot, merge every overlay into it, and publish the
    // result to the served state exactly once. A reader never observes a
    // code-only or half-overlaid intermediate -- during the initial build it
    // keeps seeing the empty/building snapshot until the fused one lands.
    DaemonState overlay_state;
    publish_graph_snapshot(overlay_state, deterministic_graph);
    (void)reconcile_semantic_cache(cache, deterministic_graph);
    for (const auto& drop : discover_semantic_fragment_drops(drop_dir)) {
      if (!event_paths.contains(normalize_semantic_source_path(drop.path))) {
        (void)replay_drop(overlay_state, drop, sources);
      }
    }
    for (const auto& live : pending_live) {
      (void)ingest_semantic_fragment(overlay_state, cache, live.source_inputs, live.drop.path);
    }
    ingest_all_memory(overlay_state);
    write_semantic_cache(cache, cache_path);
    publish_graph_snapshot(state, *read_graph_snapshot(overlay_state));
    {
      // enrichment_mutex so a concurrent status read never tears the counter.
      const std::scoped_lock enrichment_lock(state.enrichment_mutex);
      state.last_memory_overlay_count = overlay_state.last_memory_overlay_count;
    }
  };

  // The daemon owns the incremental file index: startup and every `update` op
  // rebuild the graph the same way (a full stat-index rescan), then re-overlay
  // semantic fragments, so `update .` keeps the resident snapshot current
  // without discarding enrichment. Wired through the injectable handler so all
  // op dispatch stays in handle_daemon_request.
  // Persisted artifacts under the project output dir. After every full rescan we
  // write graph.json plus a version-stamped manifest of the file set it was built
  // from, so a later restart with an unchanged tree can serve straight from disk.
  const auto out_dir = identity.project_root / "cgraph-out";
  const auto graph_path = out_dir / "graph.json";
  const auto manifest_path = out_dir / "index-manifest.json";

  // Whether index.files holds extractions for the whole tree. A full rescan
  // hydrates it; the Tier-1 fast path (persisted graph load) does NOT — it only
  // publishes the snapshot. Incremental updates rebuild the graph from
  // index.files, so applying one against a non-hydrated index would replace the
  // full graph with just the changed files.
  std::atomic<bool> index_hydrated{false};

  // Periodic persistence of deterministic watcher state. Semantic and memory
  // changes are durable in their own cache/sidecars and never enter graph.json.
  DaemonLifecycleState lifecycle;
  DaemonLifecycleConfig lifecycle_config;
  lifecycle_config.endpoint_path = socket_path;
  lifecycle_config.graph_path = graph_path;
  lifecycle_config.idle_timeout = options.idle_timeout;
  lifecycle_config.persist_interval = options.persist_interval;

  // Writes the file manifest the persisted graph was built from. Logged, never
  // fatal: a failed persist only means the next restart rebuilds, not fast-loads.
  const auto persist_manifest = [&]() {
    IndexManifest manifest;
    manifest.version_key = index_version_key();
    manifest.content_root = deterministic_graph.content_root;
    manifest.files.reserve(index.cache.size());
    for (const auto& [_, entry] : index.cache) {
      manifest.files.push_back(entry);
    }
    if (!write_index_manifest(manifest, manifest_path)) {
      std::cerr << "graphd: failed to persist " << manifest_path << '\n';
    }
  };

  // Writes graph.json + the file manifest atomically.
  const auto persist_graph_and_manifest = [&]() {
    if (!persist_graph_snapshot(deterministic_graph, graph_path)) {
      std::cerr << "graphd: failed to persist " << graph_path << '\n';
      return;
    }
    persist_manifest();
  };

  const auto rescan = [&]() {
    const std::scoped_lock lock(graph_mutex);
    // Build into a private state so readers keep the current (or, during the
    // initial build, the empty/building) snapshot until rebuild_final_overlay
    // publishes the fused deterministic+overlay result in one step.
    DaemonState scan_state;
    const auto result = full_stat_index_rescan(scan_state, index, identity.project_root);
    if (result.applied) {
      {
        // enrichment_mutex covers unextracted and the modeled-saving scalars so
        // a concurrent `status` read never tears them. scan_state is private to
        // this call, so reading it unlocked is safe.
        const std::scoped_lock enrichment_lock(state.enrichment_mutex);
        state.unextracted = scan_state.unextracted;
        state.last_files_cache_hit = scan_state.last_files_cache_hit;
        state.last_extract_mean_ms = scan_state.last_extract_mean_ms;
      }
      index_hydrated.store(true);
      deterministic_graph = *read_graph_snapshot(scan_state);
      rebuild_final_overlay();
      // Persist the code-only snapshot before
      // enrichment planning, which walks the whole project and can take seconds on
      // a large repo. The Tier-1 cache should land promptly, not behind planning.
      persist_graph_and_manifest();
    }
    const auto graph = read_graph_snapshot(state);
    nlohmann::json response{
        {"accepted", result.applied},
        {"full_rescan", result.full_rescan},
        {"files_hashed", result.files_hashed},
        {"bytes_hashed", result.bytes_hashed},
        {"files_reextracted", result.files_reextracted},
        {"files_cache_hit", result.files_cache_hit},
        {"files_removed", result.files_removed},
        {"node_count", graph->nodes.size()},
        {"edge_count", graph->edges.size()},
        {"freshness", freshness_metadata(*graph)},
    };
    if (!result.warnings.empty()) {
      response["warnings"] = result.warnings;
    }
    return response;
  };

  // Tier-1 fast path: if the persisted manifest's version key still matches this
  // binary and a full-content read reproduces its file hashes and root, load the
  // persisted graph and overlay semantic drops instead of re-extracting. Hold
  // the writer lock across verification and publication so a concurrent explicit
  // update cannot be overwritten by an older startup snapshot.
  const auto try_load_persisted = [&]() -> bool {
    const std::scoped_lock lock(graph_mutex);
    const auto manifest = read_index_manifest(manifest_path);
    if (!manifest || manifest->version_key != index_version_key()) {
      return false;
    }
    const auto detected = detect_project_files(identity.project_root);
    if (!tree_matches_manifest(*manifest, detected, identity.project_root)) {
      return false;
    }
    // Parse through an isolated state, then attach the verified manifest root
    // before the loaded snapshot becomes visible to daemon reads. Publishing
    // directly and mutating afterward would briefly expose a rootless snapshot.
    DaemonState persisted_state;
    if (!load_graph_snapshot(persisted_state, graph_path)) {
      return false;
    }
    auto graph = *read_graph_snapshot(persisted_state);
    graph.content_root = manifest->content_root;
    graph.source_hashes.reserve(manifest->files.size());
    for (const auto& entry : manifest->files) {
      if (!entry.sha256.empty()) {
        graph.source_hashes[incremental_file_key(entry.path)] = entry.sha256;
      }
    }
    deterministic_graph = std::move(graph);
    // The persisted graph does not hydrate extraction fragments, but its file
    // entries were just content-verified and remain the canonical manifest
    // source until the first full rescan. Without this cache hydration, a later
    // deterministic persist would overwrite the manifest with an empty file set
    // and waste the next restart's fast path.
    index.project_root = identity.project_root;
    index.cache.clear();
    index.cache.reserve(manifest->files.size());
    for (const auto& entry : manifest->files) {
      index.cache.emplace(incremental_file_key(entry.path), entry);
    }
    // The fast path skips the rescan that normally populates the coverage map;
    // compute it from the detection walk this path already did.
    {
      const std::scoped_lock enrichment_lock(state.enrichment_mutex);  // status reads unextracted
      state.unextracted = unextracted_counts(detected);
    }
    // Reconcile from the deterministic fast-loaded graph before publishing any
    // cache-valid semantic drops or memory sidecars.
    rebuild_final_overlay();
    // Log the fast-path load immediately — before enrichment planning, which
    // walks the whole project and can take seconds.
    std::cerr << "graphd: loaded persisted graph (" << detected.size() << " files unchanged)\n";
    return true;
  };
  state.update_handler = [&](const nlohmann::json&) {
    auto result = rescan();
    // Dependency reconciliation can make semantic sources stale even when the
    // doc/media files themselves did not change. Re-plan from the reconciled
    // cache so status and the host-visible queue describe current work.
    request_refresh();
    // rescan persisted graph + manifest itself; nothing is memory-only now.
    // Runs on the serve-loop thread, the same thread that marks/persists.
    lifecycle.graph_dirty = false;
    return result;
  };
  // Session-memory checkpoint bodies are written under cgraph-out/memory by the
  // `remember` op; the node points at the markdown via source_file.
  state.memory_dir = memory_dir;

  // Live code watching: the serve loop polls the project tree on its own cadence
  // and folds changed files into the graph incrementally. The watcher is primed
  // (baseline snapshot, no events) on the build thread right before the initial
  // build, so changes that land while the build runs still surface as events on
  // the first post-build poll; the loop only polls once the baseline graph
  // exists (initial_build_done), so an event can never rebuild from a
  // half-populated index.
  const bool watch_code = options.code_poll_interval.count() > 0;
  FileWatcher code_watcher(
      identity.project_root,
      FileWatcherOptions{.debounce = options.watch_debounce, .max_pending_events = options.watch_max_pending});
  std::atomic<bool> initial_build_done{false};
  state.watching = watch_code && options.build_graph_on_start;

  // Build on a worker thread so the accept loop below starts serving at once.
  // rescan() locks graph_mutex; status/query answer from the empty snapshot until
  // the build publishes the real one.
  std::thread build_thread;
  if (options.build_graph_on_start) {
    build_thread = std::thread([&] {
      if (watch_code) {
        (void)code_watcher.poll(FileWatcherClock::now());  // prime: baseline only, no events
      }
      if (!try_load_persisted()) {
        (void)rescan();
      }
      // Plan enrichment after the initial build/load to populate current health.
      // Later doc/media changes, drop ingests, and code-dependency reconciliation
      // request another plan from the persisted stat/hash index.
      request_refresh();
      initial_build_done.store(true);  // hands the watcher to the serve loop
    });
  }

  // Enrichment-refresh worker: coalesces refresh requests (many builds/drops in a
  // burst trigger one re-plan) and runs the slow walk off the build/serve path.
  std::thread enrichment_worker([&] {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(refresh_mutex);
        refresh_cv.wait(lock, [&] { return refresh_requested || refresh_stop; });
        if (refresh_stop) {
          return;
        }
        refresh_requested = false;
      }
      run_enrichment_refresh();
    }
  });

  (void)drop_watcher.poll(FileWatcherClock::now());  // prime: existing drops are already overlaid

  std::cerr << "graphd listening on " << socket_path << " for root " << identity.project_root << '\n';

  auto last_activity = FileWatcherClock::now();
  auto last_code_poll = FileWatcherClock::now();
  while (!state.shutdown_requested) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listen_fd, &read_set);
    timeval timeout{};
    timeout.tv_usec = static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(options.drop_poll_interval).count() % 1'000'000);
    timeout.tv_sec = static_cast<time_t>(
        std::chrono::duration_cast<std::chrono::seconds>(options.drop_poll_interval).count());

    const int ready = ::select(listen_fd + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "graphd: select() failed: " << std::strerror(errno) << '\n';
      break;
    }

    // Ingest any debounced fragment drops discovered since the last tick. Not
    // polled until the initial build publishes: the build thread holds
    // graph_mutex across semantic replay (which can block on a source read), so
    // taking it here would wedge the accept loop -- the exact hang the persist
    // step's try_to_lock avoids. Drops that land mid-build stay pending in the
    // watcher and are picked up on the first post-build poll; the build's own
    // rebuild_final_overlay already discovers on-disk drops directly.
    if (const auto events = (!options.build_graph_on_start || initial_build_done.load())
                                ? drop_watcher.poll(FileWatcherClock::now())
                                : std::vector<SemanticFragmentDropEvent>{};
        !events.empty()) {
      const std::scoped_lock lock(graph_mutex);  // serialize with the build/rescan thread
      // Mark the batch in-flight so a concurrent `status` reports enrichment as
      // running; the scope clears the running count on exit and request_refresh
      // recomputes the steady state below.
      const EnrichmentRunningScope running(state, events.size());
      rebuild_final_overlay(&events);
      request_refresh();
      last_activity = FileWatcherClock::now();
    }

    // Fold live code changes into the graph. Each watcher poll walks the project
    // tree, so it runs on its own (slower) cadence than the drop poll, and only
    // once the initial build has published a baseline.
    if (watch_code && initial_build_done.load() &&
        FileWatcherClock::now() - last_code_poll >= options.code_poll_interval) {
      last_code_poll = FileWatcherClock::now();
      const auto events = code_watcher.poll(last_code_poll);
      bool code_changed = false;
      std::vector<FileWatchEvent> semantic_source_events;
      for (const auto& event : events) {
        if (event.change == FileWatchChange::Overflow || event.kind == WatchedFileKind::Code) {
          code_changed = true;
        } else {
          semantic_source_events.push_back(event);
        }
      }
      if (code_changed) {
        // Neighborhood dedup keeps each incremental update fast, but it skips
        // the fuzzy-duplicate merges a full pass makes elsewhere in the graph,
        // so the node set drifts above the canonical build's. Reconciling with
        // a full dedup every Nth update bounds that drift; an explicit `update`
        // op or a restart rescan also reconverges it.
        constexpr std::size_t kFullDedupReconcileEvery = 5;
        const std::scoped_lock lock(graph_mutex);
        IncrementalUpdateResult result;
        DaemonState hydration_state;
        const bool hydration = !index_hydrated.load();
        if (!hydration) {
          result = apply_incremental_code_updates(
              state, index, events, IncrementalDedupPolicy{.full_reconcile_every = kFullDedupReconcileEvery});
        } else {
          // First edit after a fast-load restart: hydrate the index with one
          // full rescan (a per-file rebuild here would wipe the graph), staged
          // like every rescan so readers never see an intermediate; subsequent
          // events go incremental.
          result = full_stat_index_rescan(hydration_state, index, identity.project_root);
        }
        for (const auto& warning : result.warnings) {
          std::cerr << "graphd: " << warning << '\n';
        }
        if (result.applied) {
          if (hydration) {
            {
              const std::scoped_lock enrichment_lock(state.enrichment_mutex);
              state.unextracted = hydration_state.unextracted;
              state.last_files_cache_hit = hydration_state.last_files_cache_hit;
              state.last_extract_mean_ms = hydration_state.last_extract_mean_ms;
            }
            index_hydrated.store(true);
            deterministic_graph = *read_graph_snapshot(hydration_state);
          } else {
            deterministic_graph = *read_graph_snapshot(state);
          }
          rebuild_final_overlay();
          request_refresh();    // code dependencies may have requeued semantic sources
          ++state.incremental_updates;
          mark_graph_dirty(lifecycle, DaemonClock::now());
          std::cerr << "graphd: incremental update (" << result.files_reextracted << " re-extracted, "
                    << result.files_removed << " removed" << (result.full_rescan ? ", full rescan" : "") << ")\n";
        }
        last_activity = FileWatcherClock::now();  // active editing keeps the daemon alive
      }
      if (!semantic_source_events.empty()) {
        const std::scoped_lock lock(graph_mutex);
        for (const auto& event : semantic_source_events) {
          stat_index.erase(normalize_semantic_source_path(event.path));
        }
        ++stat_index_revision;
        write_semantic_stat_index(stat_index, stat_index_path);
        rebuild_final_overlay();
        request_refresh();
        last_activity = FileWatcherClock::now();
      }
    }

    // Re-persist graph + manifest once incremental changes have aged past the
    // persist interval, so a crash loses at most that window. try_to_lock: the
    // initial build (or a long overlay replay) holds graph_mutex, and the serve
    // loop must keep answering from the current snapshot while it runs -- a
    // skipped persist is retried on the next loop pass.
    bool persisted_incremental = false;
    {
      const std::unique_lock<std::mutex> lock(graph_mutex, std::try_to_lock);
      if (lock.owns_lock()) {
        persisted_incremental =
            persist_if_due(deterministic_graph, lifecycle, lifecycle_config, DaemonClock::now());
        if (persisted_incremental) {
          persist_manifest();
        }
      }
    }
    if (persisted_incremental) {
      std::cerr << "graphd: persisted incremental graph state\n";
    }

    if (ready == 0) {
      // A non-positive idle timeout disables idle shutdown entirely: the daemon
      // stays resident (and watching) until an explicit shutdown op or signal.
      if (options.idle_timeout > std::chrono::seconds::zero() &&
          FileWatcherClock::now() - last_activity >= options.idle_timeout) {
        std::cerr << "graphd: idle timeout, shutting down\n";
        break;
      }
      continue;  // no connection this tick; keep polling drops
    }

    const int conn = ::accept(listen_fd, nullptr, nullptr);
    if (conn < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    last_activity = FileWatcherClock::now();
    apply_connection_timeout(conn);  // a stalled peer must not wedge the serve loop
    // The thin client uses one connection per request: read it, answer it, close.
    if (const auto request = read_frame(conn)) {
      const auto response = handle_daemon_request(state, *request);
      (void)write_frame(conn, response);
    }
    ::close(conn);
  }

  if (build_thread.joinable()) {
    build_thread.join();  // captures locals by reference: join before they leave scope
  }
  // Flush any deterministic incremental state whose persist interval had not
  // elapsed. Semantic cache and memory sidecars are written on their own paths.
  if (lifecycle.graph_dirty) {
    const std::scoped_lock lock(graph_mutex);
    persist_graph_and_manifest();
    std::cerr << "graphd: persisted incremental graph state on exit\n";
  }
  // Best-effort durable op-stats flush: append this lifetime's op-stats to a JSONL
  // ledger so query activity survives idle-shutdown and aggregates across restarts.
  // Gated on >=1 substantive op so idle status-only spawns write nothing. The wall
  // clock is read once here; boot is derived from the monotonic uptime, so the live
  // daemon stayed purely monotonic. Never blocks, throws, or fails the shutdown.
  if (state.op_stats.has_substantive_ops()) {
    const auto shutdown_wall = WallClock::now();
    const auto boot_wall = shutdown_wall - std::chrono::duration_cast<WallClock::duration>(
                                               StatsClock::now() - state.start_time);
    if (!append_op_stats_ledger(out_dir / "op-stats-ledger.jsonl",
                                op_stats_ledger_line(state.op_stats, boot_wall, shutdown_wall))) {
      std::cerr << "graphd: op-stats ledger flush failed (non-fatal)\n";
    }
  }
  {
    const std::scoped_lock lock(refresh_mutex);
    refresh_stop = true;
  }
  refresh_cv.notify_one();
  enrichment_worker.join();  // also captures locals by reference; join before scope exit
  ::close(listen_fd);
  (void)cleanup_daemon_endpoint(socket_path);
  return 0;
}

std::optional<nlohmann::json> request_over_unix_socket(
    const std::filesystem::path& socket_path,
    const nlohmann::json& request) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return std::nullopt;
  }
  sockaddr_un addr{};
  if (!fill_sockaddr(addr, socket_path.string())) {
    ::close(fd);
    return std::nullopt;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);  // ECONNREFUSED / ENOENT -> caller spawns and retries
    return std::nullopt;
  }
  std::optional<nlohmann::json> response;
  if (write_frame(fd, request)) {
    response = read_frame(fd);
  }
  ::close(fd);
  return response;
}

#endif  // _WIN32

}  // namespace cgraph
