#pragma once

#include "cgraph/operation_stats.hpp"
#include "cgraph/types.hpp"
#include "cgraph/snapshot_source_reader.hpp"
#include <span>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace cgraph {

enum class EnrichmentState {
  Idle,
  Pending,
  Running,
  Stale,
  Failed,
};

struct DaemonState {
  std::shared_ptr<const GraphSnapshot> graph_snapshot = std::make_shared<GraphSnapshot>();
  mutable std::mutex snapshot_mutex;
  mutable std::mutex writer_mutex;
  // Guards the enrichment counters (enrichment_state / _pending / _running /
  // _stale / _failed / _plans_run) and the `unextracted` map below. These are
  // written by the enrichment-refresh worker, the drop-ingest path, and the
  // rescan/incremental-update path, and read concurrently by `status` (polled
  // constantly by the status-gated drainer). A dedicated mutex — separate from
  // snapshot_mutex/writer_mutex, which cover the graph snapshot — is taken by
  // every reader and writer so a status read never tears a counter or iterates
  // the map mid-mutation.
  mutable std::mutex enrichment_mutex;
  bool shutdown_requested = false;
  int pid = 0;
  // Set at construction (~daemon start); uptime is derived from it in status,
  // never stored as a stale scalar.
  StatsClock::time_point start_time = StatsClock::now();
  EnrichmentState enrichment_state = EnrichmentState::Idle;
  std::size_t enrichment_pending = 0;
  std::size_t enrichment_running = 0;
  std::size_t enrichment_stale = 0;
  std::size_t enrichment_failed = 0;
  // How many enrichment re-plans the daemon has run. A code-only build/rescan
  // must not increment this; only the initial plan and doc/media changes do.
  std::size_t enrichment_plans_run = 0;
  // Operation stats accumulated at the request-dispatch boundary: since-boot
  // lifetime totals plus a rolling recent window.
  DaemonOpStats op_stats;
  // Layer A inputs from the most recent (re)build, used to derive a modeled
  // cache-saving estimate in status. last_extract_mean_ms is 0 when the most
  // recent build extracted nothing (full cache hit), which suppresses the
  // estimate rather than fabricating one.
  double last_extract_mean_ms = 0.0;
  std::size_t last_files_cache_hit = 0;
  // Live code watching: whether the serve loop folds file changes into the
  // graph automatically, and how many incremental updates it has applied.
  bool watching = false;
  std::size_t incremental_updates = 0;
  // Detected files no registered extractor handles (language name -> count).
  // Populated by full rescans and the fast-load start, adjusted by incremental
  // updates, surfaced in `status` so a coverage hole is never silent.
  std::map<std::string, std::size_t> unextracted;
  // Performs a deterministic rebuild for an `update` op and returns its result
  // payload. Injected by the running daemon (which owns the file index and
  // project root); when unset, `update` is accepted as a no-op so in-process
  // callers and tests need no rebuild wiring.
  std::function<nlohmann::json(const nlohmann::json& params)> update_handler;
  // Directory the `remember` op writes checkpoint markdown bodies into (set by
  // the running daemon to <project>/cgraph-out/memory). Empty disables the write
  // op (returns an error) so in-process callers without a project dir are safe.
  std::filesystem::path memory_dir;
  // Canonical project root the daemon serves (set by the running daemon). The
  // report op names modules relative to it; empty leaves file paths as they are,
  // which in-process callers and tests use with already-relative paths.
  std::filesystem::path project_root;
  // Session-memory observability: recency of the last remember/recall (ms-epoch
  // strings, empty when never called) and the number of checkpoints re-applied by
  // the most recent memory re-overlay. Surfaced in the status `memory` block.
  std::string last_remember_at;
  std::string last_recall_at;
  std::size_t last_memory_overlay_count = 0;
};

// Marks an enrichment ingest as in-flight for the lifetime of the scope:
// enrichment_running carries the batch size and enrichment_state reports
// `running`, both observable by a concurrent `status` read. On scope exit the
// running count clears; the steady-state enrichment_state is recomputed by the
// next refresh. Mirrors ScopedTimer so the in-flight window is explicit at the
// call site rather than hand-managed.
class EnrichmentRunningScope {
 public:
  EnrichmentRunningScope(DaemonState& state, std::size_t batch_size) : state_(state) {
    const std::scoped_lock lock(state_.enrichment_mutex);
    state_.enrichment_running = batch_size;
    state_.enrichment_state = EnrichmentState::Running;
  }
  EnrichmentRunningScope(const EnrichmentRunningScope&) = delete;
  EnrichmentRunningScope& operator=(const EnrichmentRunningScope&) = delete;
  EnrichmentRunningScope(EnrichmentRunningScope&&) = delete;
  EnrichmentRunningScope& operator=(EnrichmentRunningScope&&) = delete;
  ~EnrichmentRunningScope() {
    const std::scoped_lock lock(state_.enrichment_mutex);
    state_.enrichment_running = 0;
    // Drop out of Running into the steady state implied by the current counts;
    // a subsequent refresh refines pending/stale, but Running never lingers.
    state_.enrichment_state = state_.enrichment_failed > 0  ? EnrichmentState::Failed
                              : state_.enrichment_stale > 0   ? EnrichmentState::Stale
                              : state_.enrichment_pending > 0 ? EnrichmentState::Pending
                                                             : EnrichmentState::Idle;
  }

 private:
  DaemonState& state_;
};

// Canonical directional multi-source traversal. Each reached node keeps a real
// predecessor edge and the originating changed symbol; roots have depth zero.
struct ImpactReach {
  int depth = 0;
  std::string via;
  std::string predecessor;
  std::string changed_id;
  Edge edge;
};
[[nodiscard]] std::unordered_map<std::string, ImpactReach> trace_impact(
    const GraphSnapshot& graph, std::span<const std::string> seeds,
    std::string_view direction, std::string_view relation, int max_depth);
// One union gather and one packing pass, never a per-seed budget multiplication.
[[nodiscard]] nlohmann::json pack_seed_context(
    const GraphSnapshot& graph, std::span<const std::string> seeds,
    std::size_t budget, int max_depth, SnapshotSourceReader& reader);
[[nodiscard]] std::size_t serialized_context_tokens(const nlohmann::json& value);

// Re-resolves a memory checkpoint fragment's `concerns` edges against the graph
// it is about to be overlaid on. An edge whose target id no longer exists is
// re-bound through the `touch` key it was written with (a node id or exact
// symbol name); one that cannot be re-bound is dropped, never left dangling.
// Returns the number of edges dropped.
std::size_t rebind_memory_concerns(const GraphSnapshot& graph, Fragment& fragment);

// Overlays memory checkpoint fragments in any order: every fragment's nodes are
// merged first, then every `concerns` edge is re-bound (rebind_memory_concerns)
// against a graph that already holds all the checkpoints and merged. A checkpoint
// that concerns an earlier checkpoint therefore keeps its edge whatever order the
// sidecars were read in. Returns the number of edges dropped.
std::size_t overlay_memory_fragments(GraphSnapshot& graph, std::vector<Fragment> fragments);

[[nodiscard]] std::shared_ptr<const GraphSnapshot> read_graph_snapshot(const DaemonState& state);
[[nodiscard]] nlohmann::json freshness_metadata(const GraphSnapshot& graph);
void publish_graph_snapshot(DaemonState& state, GraphSnapshot graph);
void mutate_graph_snapshot(DaemonState& state, const std::function<void(GraphSnapshot&)>& mutator);
[[nodiscard]] nlohmann::json handle_daemon_request(DaemonState& state, const nlohmann::json& request);

}  // namespace cgraph
