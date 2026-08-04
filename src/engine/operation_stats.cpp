#include "cgraph/operation_stats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace cgraph {
namespace {

constexpr std::array<const char*, kDaemonOpCount> kOpNames = {
    "query", "path", "explain", "impact", "context", "update", "status", "shutdown", "remember", "recall",
};

// Human-readable duration: ms under a second, seconds under a minute, otherwise
// whole minutes. "340 ms", "1.2 s", "3 minutes" - never "12m 49s".
[[nodiscard]] std::string human_duration(double ms) {
  if (ms < 1000.0) {
    return std::to_string(static_cast<long long>(std::llround(ms))) + " ms";
  }
  const double seconds = ms / 1000.0;
  if (seconds < 60.0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f s", seconds);
    return buf;
  }
  const long long minutes = std::llround(seconds / 60.0);
  return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
}

// Mean latency over a lifetime total (total_ms / count). 0 when count is 0.
[[nodiscard]] double mean_ms(double total, std::size_t count) {
  return count == 0 ? 0.0 : total / static_cast<double>(count);
}

}  // namespace

const char* daemon_op_name(DaemonOp op) {
  const auto idx = static_cast<std::size_t>(op);
  return idx < kDaemonOpCount ? kOpNames[idx] : "unknown";
}

std::optional<DaemonOp> daemon_op_from_string(std::string_view name) {
  for (std::size_t i = 0; i < kDaemonOpCount; ++i) {
    if (name == kOpNames[i]) {
      return static_cast<DaemonOp>(i);
    }
  }
  return std::nullopt;
}

std::optional<double> modeled_cache_saved_ms(std::size_t files_cache_hit,
                                             std::size_t files_extracted,
                                             double extract_total_ms) {
  // No reuse, or no per-file mean to model from this build -> no honest estimate.
  if (files_cache_hit == 0 || files_extracted == 0 || extract_total_ms <= 0.0) {
    return std::nullopt;
  }
  const double mean = extract_total_ms / static_cast<double>(files_extracted);
  return static_cast<double>(files_cache_hit) * mean;
}

double cache_hit_rate(std::size_t files_cache_hit, std::size_t files_total) {
  return files_total == 0 ? 0.0
                          : static_cast<double>(files_cache_hit) / static_cast<double>(files_total);
}

double percentile(std::vector<double> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::ranges::sort(values);
  const double clamped = std::clamp(p, 0.0, 1.0);
  const double rank = clamped * static_cast<double>(values.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(rank));
  const auto hi = static_cast<std::size_t>(std::ceil(rank));
  if (lo == hi) {
    return values[lo];
  }
  const double frac = rank - static_cast<double>(lo);
  return values[lo] + (values[hi] - values[lo]) * frac;
}

double query_zero_hit_rate(std::size_t query_count, std::size_t query_zero_hits) {
  return query_count == 0 ? 0.0
                          : static_cast<double>(query_zero_hits) / static_cast<double>(query_count);
}

nlohmann::json build_stats_json(const BuildStats& stats) {
  nlohmann::json phases{
      {"extract_ms", stats.extract_ms},
      {"merge_ms", stats.merge_ms},
      {"resolve_ms", stats.resolve_ms},
      {"dedup_ms", stats.dedup_ms},
      {"communities_ms", stats.communities_ms},
      {"analyze_ms", stats.analyze_ms},
  };
  nlohmann::json body{
      {"phase_ms", std::move(phases)},
      {"total_ms", stats.total_ms()},
      {"files_total", stats.files_total},
      {"files_extracted", stats.files_extracted},
      {"files_cache_hit", stats.files_cache_hit},
      {"cache_hit_rate", cache_hit_rate(stats.files_cache_hit, stats.files_total)},
      {"node_count", stats.nodes},
      {"edge_count", stats.edges},
      {"unextracted", stats.unextracted},
      // An unresolved call leaves no trace in the graph, so the resolution rate
      // has to be reported or it cannot be known. The five outcomes partition
      // `total`.
      {"call_resolution",
       {
           {"total", stats.calls.total},
           {"resolved_same_file", stats.calls.resolved_same_file},
           {"resolved_project_unique", stats.calls.resolved_project_unique},
           {"resolved_overload_first", stats.calls.resolved_overload_first},
           {"dropped_unknown", stats.calls.dropped_unknown},
           {"dropped_ambiguous", stats.calls.dropped_ambiguous},
           {"dropped_self", stats.calls.dropped_self},
           {"resolved_rate", stats.calls.resolved_rate()},
           {"balances", stats.calls.balances()},
       }},
  };
  // Modeled, labeled, and omitted when it cannot be honestly formed.
  if (const auto saved = modeled_cache_saved_ms(stats.files_cache_hit, stats.files_extracted, stats.extract_ms)) {
    body["cache_saved_ms_estimate"] = *saved;
  }
  return body;
}

std::string build_stats_summary(const BuildStats& stats) {
  return std::to_string(stats.files_total) + " files, " + std::to_string(stats.nodes) + " nodes, " +
         std::to_string(stats.edges) + " edges in " + human_duration(stats.total_ms());
}

nlohmann::json op_stats_json(const DaemonOpStats& stats) {
  // Lifetime totals: count + mean latency per op.
  nlohmann::json lifetime = nlohmann::json::object();
  std::size_t total_ops = 0;
  for (std::size_t i = 0; i < kDaemonOpCount; ++i) {
    if (stats.count[i] == 0) {
      continue;
    }
    total_ops += stats.count[i];
    lifetime[daemon_op_name(static_cast<DaemonOp>(i))] = {
        {"count", stats.count[i]},
        {"mean_ms", mean_ms(stats.total_ms[i], stats.count[i])},
    };
  }

  // Recent window: count + p50 latency over the individual recent samples.
  std::vector<double> recent_latencies;
  recent_latencies.reserve(stats.recent.size());
  for (const auto& sample : stats.recent.samples()) {
    recent_latencies.push_back(sample.latency_ms);
  }

  const std::size_t query_count = stats.count[static_cast<std::size_t>(DaemonOp::Query)];

  return {
      {"total_ops", total_ops},
      {"lifetime", std::move(lifetime)},
      {"recent_window",
       {{"size", stats.recent.size()},
        {"capacity", kRecentWindowCapacity},
        {"p50_latency_ms", percentile(recent_latencies, 0.5)}}},
      {"query_count", query_count},
      {"query_zero_hits", stats.query_zero_hits},
      {"query_zero_hit_rate", query_zero_hit_rate(query_count, stats.query_zero_hits)},
      {"context_adaptive_count", stats.adaptive_context},
      {"context_zero_hits", stats.context_zero_hits},
      {"recall_zero_hits", stats.recall_zero_hits},
      {"not_ready", stats.not_ready},
      {"query_routes",
       {{"entity", stats.query_route_entity},
        {"structural", stats.query_route_structural},
        {"search", stats.query_route_search}}},
  };
}

// --- Durable ledger (persist-op-stats-ledger) --------------------------------

namespace {

// The substantive ops the ledger records and the rollup headlines.
constexpr std::array<DaemonOp, 7> kSubstantiveOps = {
    DaemonOp::Query,   DaemonOp::Path,     DaemonOp::Explain, DaemonOp::Impact,
    DaemonOp::Context, DaemonOp::Remember, DaemonOp::Recall};

}  // namespace

std::string format_iso8601_utc(WallClock::time_point tp) {
  const std::time_t t = WallClock::to_time_t(tp);
  std::tm tm{};
#ifdef _WIN32
  ::gmtime_s(&tm, &t);
#else
  ::gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::optional<WallClock::time_point> parse_iso8601_utc(std::string_view text) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  // Tolerant of a trailing fractional second / offset variations is out of scope;
  // we emit and read the canonical "Z" form only.
  if (std::sscanf(std::string(text).c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &s) != 6) {
    return std::nullopt;
  }
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
#ifdef _WIN32
  const std::time_t t = ::_mkgmtime(&tm);
#else
  const std::time_t t = ::timegm(&tm);
#endif
  if (t == static_cast<std::time_t>(-1)) {
    return std::nullopt;
  }
  return WallClock::from_time_t(t);
}

nlohmann::json op_stats_ledger_line(const DaemonOpStats& stats,
                                    WallClock::time_point boot,
                                    WallClock::time_point shutdown) {
  nlohmann::json ops = nlohmann::json::object();
  for (const auto op : kSubstantiveOps) {
    const auto idx = static_cast<std::size_t>(op);
    nlohmann::json hist = nlohmann::json::array();
    for (std::size_t b = 0; b < kHistBucketCount; ++b) {
      hist.push_back(stats.latency_hist[idx][b]);
    }
    ops[daemon_op_name(op)] = {
        {"count", stats.count[idx]},
        {"total_ms", stats.total_ms[idx]},
        {"hist_ms", std::move(hist)},
    };
  }
  return {
      {"schema_version", kLedgerSchemaVersion},
      {"boot", format_iso8601_utc(boot)},
      {"shutdown", format_iso8601_utc(shutdown)},
      {"uptime_seconds", std::chrono::duration<double>(shutdown - boot).count()},
      {"query_zero_hits", stats.query_zero_hits},
      {"context_zero_hits", stats.context_zero_hits},
      {"adaptive_context", stats.adaptive_context},
      {"recall_zero_hits", stats.recall_zero_hits},
      {"not_ready", stats.not_ready},
      {"query_route_entity", stats.query_route_entity},
      {"query_route_structural", stats.query_route_structural},
      {"query_route_search", stats.query_route_search},
      {"ops", std::move(ops)},
  };
}

double histogram_percentile(const std::array<std::size_t, kHistBucketCount>& hist, double p) {
  std::size_t total = 0;
  for (const auto c : hist) {
    total += c;
  }
  if (total == 0) {
    return 0.0;
  }
  const double target = std::clamp(p, 0.0, 1.0) * static_cast<double>(total);
  std::size_t cum = 0;
  for (std::size_t b = 0; b < kHistBucketCount; ++b) {
    const std::size_t c = hist[b];
    if (c == 0) {
      continue;
    }
    if (static_cast<double>(cum + c) >= target) {
      // Overflow bucket is unbounded above; report its lower edge as a floor.
      if (b >= kHistBucketUpperMs.size()) {
        return kHistBucketUpperMs.back();
      }
      const double lower = (b == 0) ? 0.0 : kHistBucketUpperMs[b - 1];
      const double upper = kHistBucketUpperMs[b];
      const double into = target - static_cast<double>(cum);  // [0, c]
      const double frac = std::clamp(into / static_cast<double>(c), 0.0, 1.0);
      return lower + (upper - lower) * frac;
    }
    cum += c;
  }
  return kHistBucketUpperMs.back();
}

nlohmann::json aggregate_op_stats_ledger(const std::vector<nlohmann::json>& lines,
                                         WallClock::time_point since) {
  std::array<std::size_t, kDaemonOpCount> sum_count{};
  std::array<double, kDaemonOpCount> sum_total_ms{};
  std::array<std::array<std::size_t, kHistBucketCount>, kDaemonOpCount> merged_hist{};
  std::size_t lifetimes = 0;
  std::size_t query_zero_hits = 0;
  std::size_t context_zero_hits = 0;
  std::size_t adaptive_context = 0;
  std::size_t recall_zero_hits = 0;
  std::size_t not_ready = 0;
  std::size_t query_route_entity = 0;
  std::size_t query_route_structural = 0;
  std::size_t query_route_search = 0;
  bool mixed = false;

  for (const auto& line : lines) {
    if (!line.is_object()) {
      continue;  // tolerate a malformed/skipped line
    }
    const auto shutdown = parse_iso8601_utc(line.value("shutdown", std::string{}));
    if (!shutdown || *shutdown < since) {
      continue;  // out of window (boundary inclusive at `since`)
    }
    ++lifetimes;
    const bool mergeable = line.value("schema_version", 0) == kLedgerSchemaVersion;
    if (!mergeable) {
      mixed = true;
    }
    query_zero_hits += line.value("query_zero_hits", static_cast<std::size_t>(0));
    // Additive fields; absent in pre-existing ledger lines, read as 0 so older
    // ledgers still roll up without migration.
    context_zero_hits += line.value("context_zero_hits", static_cast<std::size_t>(0));
    adaptive_context += line.value("adaptive_context", static_cast<std::size_t>(0));
    recall_zero_hits += line.value("recall_zero_hits", static_cast<std::size_t>(0));
    not_ready += line.value("not_ready", static_cast<std::size_t>(0));
    query_route_entity += line.value("query_route_entity", static_cast<std::size_t>(0));
    query_route_structural += line.value("query_route_structural", static_cast<std::size_t>(0));
    query_route_search += line.value("query_route_search", static_cast<std::size_t>(0));
    const auto ops = line.value("ops", nlohmann::json::object());
    for (const auto op : kSubstantiveOps) {
      const auto* name = daemon_op_name(op);
      if (!ops.contains(name)) {
        continue;
      }
      const auto& entry = ops[name];
      const auto idx = static_cast<std::size_t>(op);
      sum_count[idx] += entry.value("count", static_cast<std::size_t>(0));
      sum_total_ms[idx] += entry.value("total_ms", 0.0);
      if (mergeable && entry.contains("hist_ms") && entry["hist_ms"].is_array()) {
        const auto& hist = entry["hist_ms"];
        for (std::size_t b = 0; b < kHistBucketCount && b < hist.size(); ++b) {
          merged_hist[idx][b] += hist[b].get<std::size_t>();
        }
      }
    }
  }

  nlohmann::json ops_out = nlohmann::json::object();
  for (const auto op : kSubstantiveOps) {
    const auto idx = static_cast<std::size_t>(op);
    ops_out[daemon_op_name(op)] = {
        {"count", sum_count[idx]},
        {"mean_ms", mean_ms(sum_total_ms[idx], sum_count[idx])},
        {"p50_ms_approx", histogram_percentile(merged_hist[idx], 0.5)},
        {"p90_ms_approx", histogram_percentile(merged_hist[idx], 0.9)},
    };
  }
  // Surface adaptive adoption and the context zero-result rate on the context op,
  // beside its summed count -- the telemetry a default-flip decision reads.
  const std::size_t context_count = sum_count[static_cast<std::size_t>(DaemonOp::Context)];
  ops_out["context"]["adaptive_count"] = adaptive_context;
  ops_out["context"]["zero_hits"] = context_zero_hits;
  ops_out["context"]["zero_hit_rate"] = query_zero_hit_rate(context_count, context_zero_hits);

  // Recall (session memory) miss rate, beside its summed count -- the durable view
  // of how often recall came back empty across lifetimes.
  const std::size_t recall_count = sum_count[static_cast<std::size_t>(DaemonOp::Recall)];
  ops_out["recall"]["zero_hits"] = recall_zero_hits;
  ops_out["recall"]["zero_hit_rate"] = query_zero_hit_rate(recall_count, recall_zero_hits);

  // Route adoption across lifetimes, beside the query summary.
  ops_out["query"]["routes"] = {{"entity", query_route_entity},
                                {"structural", query_route_structural},
                                {"search", query_route_search}};

  const std::size_t query_count = sum_count[static_cast<std::size_t>(DaemonOp::Query)];
  return {
      {"since", format_iso8601_utc(since)},
      {"lifetimes", lifetimes},
      {"mixed_schema_versions", mixed},
      {"not_ready", not_ready},
      {"query",
       {{"count", query_count},
        {"zero_hits", query_zero_hits},
        {"zero_hit_rate", query_zero_hit_rate(query_count, query_zero_hits)}}},
      {"ops", std::move(ops_out)},
  };
}

bool append_op_stats_ledger(const std::filesystem::path& path, const nlohmann::json& line) {
  try {
    if (path.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::app);
    if (!out) {
      return false;
    }
    out << line.dump() << '\n';
    return static_cast<bool>(out);
  } catch (...) {
    return false;  // best-effort: never propagate
  }
}

}  // namespace cgraph
