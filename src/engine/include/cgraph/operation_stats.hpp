#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// Layer A substrate: timings and counters measured at the pipeline and daemon
// orchestration seams. Layer B (the derivation functions at the bottom of this
// header) only interprets these numbers; it never measures anything itself and
// never invents a counterfactual.

using StatsClock = std::chrono::steady_clock;

// RAII scoped timer: writes elapsed milliseconds into *out on destruction.
// Monotonic clock; the wall clock is never read.
class ScopedTimer {
 public:
  explicit ScopedTimer(double* out) : out_(out), start_(StatsClock::now()) {}
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;
  ScopedTimer(ScopedTimer&&) = delete;
  ScopedTimer& operator=(ScopedTimer&&) = delete;
  ~ScopedTimer() {
    if (out_ != nullptr) {
      const std::chrono::duration<double, std::milli> elapsed = StatsClock::now() - start_;
      *out_ = elapsed.count();
    }
  }

 private:
  double* out_;
  StatsClock::time_point start_;
};

// What became of each call site the extractors saw. An unresolved call is
// dropped and leaves no trace in the graph, so without this the resolution rate
// is unmeasurable from anything committed -- which is how a ~98% drop rate for
// C/C++ went unnoticed for months. The five outcomes partition `total`, and
// `balances()` asserts that they do, so a future resolution path cannot be added
// without being counted.
struct CallResolution {
  std::size_t total = 0;
  std::size_t resolved_same_file = 0;
  std::size_t resolved_project_unique = 0;
  // A member call (`obj.method()`) that missed its own file but uniquely names
  // a method project-wide. Graded INFERRED: the receiver type is unknown, the
  // method-only index is what keeps the guess narrow.
  std::size_t resolved_member_method = 0;
  // An overload set (same-file, or project-wide with every candidate declared
  // in one file): the call edges to every member, all graded INFERRED, because
  // which overload a call means needs types we do not have. Counted once per
  // call, not per edge.
  std::size_t resolved_overload_first = 0;
  std::size_t dropped_unknown = 0;    // nothing callable bears the name
  std::size_t dropped_ambiguous = 0;  // candidates span more than one file
  std::size_t dropped_self = 0;       // resolved to the caller itself

  [[nodiscard]] bool balances() const {
    return resolved_same_file + resolved_project_unique + resolved_member_method + dropped_unknown +
               dropped_ambiguous + dropped_self ==
           total;  // resolved_overload_first is a subset of same_file + project_unique
  }

  [[nodiscard]] double resolved_rate() const {
    return total == 0 ? 0.0
                      : static_cast<double>(resolved_same_file + resolved_project_unique +
                                            resolved_member_method) /
                            static_cast<double>(total);
  }
};

// Per-phase timings and counters for one deterministic (re)build. Populated by
// run_one_shot and the daemon rescan.
struct BuildStats {
  double extract_ms = 0.0;
  double merge_ms = 0.0;
  double resolve_ms = 0.0;
  double dedup_ms = 0.0;
  double communities_ms = 0.0;
  double analyze_ms = 0.0;
  std::size_t files_total = 0;
  std::size_t files_extracted = 0;  // re-parsed this build
  std::size_t files_cache_hit = 0;  // reused from a warm index
  std::size_t nodes = 0;
  std::size_t edges = 0;
  CallResolution calls;
  // Detected files no registered extractor handles, per language name. Empty
  // when coverage is total; nonzero means part of the tree is invisible to the
  // graph (fail-loud, so a coverage hole never hides in a per-file warning).
  std::map<std::string, std::size_t> unextracted;

  [[nodiscard]] double total_ms() const {
    return extract_ms + merge_ms + resolve_ms + dedup_ms + communities_ms + analyze_ms;
  }
};

// The daemon request types, in dispatch order. Count is the array sentinel.
// Remember/Recall (session memory) are appended before Count so the durable
// ledger's kSubstantiveOps (Query..Context) and its schema stay unchanged.
enum class DaemonOp { Query, Path, Explain, Impact, Context, Update, Status, Shutdown, Remember, Recall, Count };

inline constexpr std::size_t kDaemonOpCount = static_cast<std::size_t>(DaemonOp::Count);

// --- Durable ledger schema (persist-op-stats-ledger) -------------------------
// The wall clock is read ONLY at the flush boundary to stamp a ledger line; the
// live substrate above stays monotonic. Changing the bucket layout is a
// versioned, migration-gated decision: bump kLedgerSchemaVersion and the rollup
// will refuse to merge histograms across versions.
using WallClock = std::chrono::system_clock;

inline constexpr int kLedgerSchemaVersion = 1;

// Pinned, log2-spaced latency bucket upper bounds (ms). A latency lands in the
// first bucket whose bound it is strictly below; latencies >= the top bound fall
// in the overflow bucket. 11 bounded buckets + 1 overflow = 12 counts per op.
inline constexpr std::array<double, 11> kHistBucketUpperMs = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
inline constexpr std::size_t kHistBucketCount = kHistBucketUpperMs.size() + 1;

// Bucket index in [0, kHistBucketCount): 0.4 ms -> 0, 1 ms -> 1, 1.5 ms -> 1,
// 4000 ms -> overflow (11). Inline so DaemonOpStats::record stays self-contained.
[[nodiscard]] inline std::size_t latency_bucket(double ms) {
  for (std::size_t i = 0; i < kHistBucketUpperMs.size(); ++i) {
    if (ms < kHistBucketUpperMs[i]) {
      return i;
    }
  }
  return kHistBucketUpperMs.size();  // overflow
}

[[nodiscard]] const char* daemon_op_name(DaemonOp op);
[[nodiscard]] std::optional<DaemonOp> daemon_op_from_string(std::string_view name);

// Fixed-capacity ring buffer of the most recent operations. O(1) insert, no
// allocation after construction. Answers "useful right now" alongside the
// lifetime totals, and holds individual latencies so a percentile is derivable
// (the lifetime totals only give a mean).
struct RecentOp {
  DaemonOp op = DaemonOp::Status;
  double latency_ms = 0.0;
  bool zero_hit = false;
};

inline constexpr std::size_t kRecentWindowCapacity = 256;

class RollingWindow {
 public:
  void record(RecentOp sample) {
    if (buffer_.size() < kRecentWindowCapacity) {
      buffer_.push_back(sample);
    } else {
      buffer_[next_] = sample;
    }
    next_ = (next_ + 1) % kRecentWindowCapacity;
  }

  [[nodiscard]] std::size_t size() const { return buffer_.size(); }
  [[nodiscard]] const std::vector<RecentOp>& samples() const { return buffer_; }

 private:
  std::vector<RecentOp> buffer_;
  std::size_t next_ = 0;
};

// Since-boot lifetime totals plus the rolling recent window.
struct DaemonOpStats {
  std::array<std::size_t, kDaemonOpCount> count{};
  std::array<double, kDaemonOpCount> total_ms{};
  // Full-lifetime per-op latency histogram (the recent ring only holds the last
  // N mixed-op samples, so it cannot answer a per-op lifetime percentile). Binned
  // on the pinned layout; the ledger reads these directly at flush.
  std::array<std::array<std::size_t, kHistBucketCount>, kDaemonOpCount> latency_hist{};
  std::size_t query_zero_hits = 0;
  // Context-op usefulness + mode counters, persisted alongside query_zero_hits so
  // the durable ledger can report adaptive adoption and the context zero-result
  // rate (the data a default-flip decision consumes). adaptive_context counts
  // context calls served with gather="adaptive"; context_zero_hits counts context
  // calls whose focal node did not resolve.
  std::size_t context_zero_hits = 0;
  std::size_t adaptive_context = 0;
  // Recall (session memory) usefulness: recalls that returned no checkpoints. Kept
  // beside context_zero_hits so the durable ledger can report the recall miss rate.
  std::size_t recall_zero_hits = 0;
  // Reads served against the still-building empty snapshot. Counted here and
  // EXCLUDED from op counts/zero-hits/latency so a query during a (re)build is a
  // "not ready", not a retrieval miss -- otherwise the zero-hit rate lies during
  // the rebuild that every logic-version bump triggers.
  std::size_t not_ready = 0;
  // Query route adoption (route-query-by-intent): how query-op responses resolved.
  std::size_t query_route_entity = 0;
  std::size_t query_route_structural = 0;
  std::size_t query_route_search = 0;
  RollingWindow recent;

  void record(DaemonOp op, double latency_ms, bool zero_hit, bool adaptive_context_call = false,
              bool not_ready_read = false) {
    if (not_ready_read) {
      not_ready += 1;  // building snapshot: not a real op against the graph, never a miss
      return;
    }
    const auto idx = static_cast<std::size_t>(op);
    count[idx] += 1;
    total_ms[idx] += latency_ms;
    latency_hist[idx][latency_bucket(latency_ms)] += 1;
    if (op == DaemonOp::Query && zero_hit) {
      query_zero_hits += 1;
    }
    if (op == DaemonOp::Context) {
      if (zero_hit) {
        context_zero_hits += 1;
      }
      if (adaptive_context_call) {
        adaptive_context += 1;
      }
    }
    if (op == DaemonOp::Recall && zero_hit) {
      recall_zero_hits += 1;
    }
    recent.record(RecentOp{op, latency_ms, zero_hit});
  }

  // Bucket a query op's `route` (route-query-by-intent) for adoption telemetry:
  // "entity" -> entity, "search"/absent -> search, any structural intent name
  // (callers/callees/references/...) -> structural. Call only for ready reads.
  void note_query_route(std::string_view route) {
    if (route == "entity") {
      query_route_entity += 1;
    } else if (route.empty() || route == "search") {
      query_route_search += 1;
    } else {
      query_route_structural += 1;
    }
  }

  // True when >=1 substantive op (query/path/explain/impact/context/remember/recall)
  // was served. Gates the durable flush so idle status-only spawns write no ledger
  // line; memory ops count, so a memory-only lifetime is still recorded.
  [[nodiscard]] bool has_substantive_ops() const {
    return count[static_cast<std::size_t>(DaemonOp::Query)] +
               count[static_cast<std::size_t>(DaemonOp::Path)] +
               count[static_cast<std::size_t>(DaemonOp::Explain)] +
               count[static_cast<std::size_t>(DaemonOp::Impact)] +
               count[static_cast<std::size_t>(DaemonOp::Context)] +
               count[static_cast<std::size_t>(DaemonOp::Remember)] +
               count[static_cast<std::size_t>(DaemonOp::Recall)] >
           0;
  }
};

// --- Layer B: pure derivation over the substrate above. No I/O, no clocks. ---

// Modeled cache saving: files_cache_hit x mean(per-file extract time). Returns
// nullopt when no estimate can be honestly formed (no reuse, or no file was
// extracted this build so there is no per-file mean). Never fabricated.
[[nodiscard]] std::optional<double> modeled_cache_saved_ms(std::size_t files_cache_hit,
                                                           std::size_t files_extracted,
                                                           double extract_total_ms);

// Reused / total. 0 when total is 0.
[[nodiscard]] double cache_hit_rate(std::size_t files_cache_hit, std::size_t files_total);

// Linear-interpolated percentile of a latency sample (p in [0,1]). 0 for empty.
[[nodiscard]] double percentile(std::vector<double> values, double p);

// Zero-result queries / total queries. 0 when no queries. ("revealed" stat.)
[[nodiscard]] double query_zero_hit_rate(std::size_t query_count, std::size_t query_zero_hits);

// stats.json body for a one-shot build, including the modeled saving when one
// can be formed.
[[nodiscard]] nlohmann::json build_stats_json(const BuildStats& stats);

// One-line human-readable summary for stderr (file/node/edge counts + total time
// in human units).
[[nodiscard]] std::string build_stats_summary(const BuildStats& stats);

// Daemon op-stats body for the status payload: lifetime totals, recent window
// (count + p50/mean latency), and the query zero-hit rate.
[[nodiscard]] nlohmann::json op_stats_json(const DaemonOpStats& stats);

// --- Durable ledger: line builder, rollup, and append (persist-op-stats-ledger) ---

// ISO-8601 UTC ("2026-06-15T11:20:12Z") <-> wall-clock time point. parse returns
// nullopt on a malformed string. Used only at the ledger boundary.
[[nodiscard]] std::string format_iso8601_utc(WallClock::time_point tp);
[[nodiscard]] std::optional<WallClock::time_point> parse_iso8601_utc(std::string_view text);

// One JSONL ledger line for a daemon lifetime: schema version, wall-clock boot/
// shutdown, uptime, query zero-hits, and per substantive op {count, total_ms,
// hist_ms[kHistBucketCount]}. Pure: timestamps are inputs, no clock is read here.
[[nodiscard]] nlohmann::json op_stats_ledger_line(const DaemonOpStats& stats,
                                                  WallClock::time_point boot,
                                                  WallClock::time_point shutdown);

// Approximate percentile (ms) read off a merged per-op histogram by in-bucket
// linear interpolation. The overflow bucket reports its lower bound (the top
// bounded edge) as a floor. 0 for an empty histogram.
[[nodiscard]] double histogram_percentile(const std::array<std::size_t, kHistBucketCount>& hist, double p);

// Roll up ledger lines whose `shutdown` is at/after `since`: summed per-op counts
// and total_ms (exact), weighted-mean and merged-histogram p50/p90 latency
// (approximate), and the overall query zero-hit rate. Lines whose schema_version
// differs from the current version are counted but their histograms are not
// merged; the result flags `mixed_schema_versions` when that happens. Pure.
[[nodiscard]] nlohmann::json aggregate_op_stats_ledger(const std::vector<nlohmann::json>& lines,
                                                       WallClock::time_point since);

// Best-effort append of one ledger line to `path` (creates parent dirs, opens in
// append mode, writes line + newline). Returns false on any failure; never throws.
[[nodiscard]] bool append_op_stats_ledger(const std::filesystem::path& path, const nlohmann::json& line);

}  // namespace cgraph
