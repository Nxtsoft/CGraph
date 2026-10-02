#include "cgraph/operation_stats.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool approx(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

}  // namespace

int main() {
  using namespace cgraph;

  // --- contract tallies survive the stats.json round trip a fast-loading
  // daemon restores them through, the generic contract counts included ---
  {
    const nlohmann::json saved{{"routes", 1},         {"consumes", 2},           {"contract_facts", 13},
                               {"contract_facts_unresolved", 5}, {"contracts_provided", 2},
                               {"contracts_external", 4},        {"contract_consumes", 6}};
    const auto restored = contract_resolution_json(contract_resolution_from_json(saved));
    for (const auto& [key, value] : saved.items()) {
      if (restored.value(key, -1) != value.get<int>()) {
        return 1;
      }
    }
  }

  // --- modeled cache saving = files_cache_hit x mean(per-file extract time) ---
  // 8 files extracted in 80ms -> 10ms/file; 5 reused -> 50ms modeled saving.
  {
    const auto saved = modeled_cache_saved_ms(/*hits=*/5, /*extracted=*/8, /*extract_total_ms=*/80.0);
    if (!saved || !approx(*saved, 50.0)) {
      return 1;
    }
  }

  // --- estimate omitted when there is no reuse or no per-file mean ---
  if (modeled_cache_saved_ms(/*hits=*/0, /*extracted=*/8, 80.0).has_value()) {
    return 2;  // no reuse -> no estimate
  }
  if (modeled_cache_saved_ms(/*hits=*/5, /*extracted=*/0, 0.0).has_value()) {
    return 3;  // full cache hit, no mean available -> no estimate (not zero)
  }
  if (modeled_cache_saved_ms(/*hits=*/5, /*extracted=*/8, 0.0).has_value()) {
    return 4;  // zero measured extract time -> no estimate
  }

  // --- cache_hit_rate, including the divide-by-zero guard ---
  if (!approx(cache_hit_rate(3, 12), 0.25)) {
    return 5;
  }
  if (!approx(cache_hit_rate(0, 0), 0.0)) {
    return 6;  // empty tree must not divide by zero
  }

  // --- query zero-hit rate, including zero-query guard ---
  if (!approx(query_zero_hit_rate(10, 2), 0.2)) {
    return 7;
  }
  if (!approx(query_zero_hit_rate(0, 0), 0.0)) {
    return 8;  // no queries -> no divide by zero
  }

  // --- percentile from a known multiset {1,2,3,4,5}: p50 == 3, p0 == 1 ---
  {
    const std::vector<double> latencies{5.0, 1.0, 3.0, 2.0, 4.0};  // unsorted on purpose
    if (!approx(percentile(latencies, 0.5), 3.0)) {
      return 9;
    }
    if (!approx(percentile(latencies, 0.0), 1.0) || !approx(percentile(latencies, 1.0), 5.0)) {
      return 10;
    }
    if (!approx(percentile({}, 0.5), 0.0)) {
      return 11;  // empty sample -> 0, no UB
    }
  }

  // --- rolling window never exceeds capacity, and records lifetime + zero-hits ---
  {
    DaemonOpStats stats;
    for (std::size_t i = 0; i < kRecentWindowCapacity + 50; ++i) {
      stats.record(DaemonOp::Query, /*latency_ms=*/1.0, /*zero_hit=*/(i % 2 == 0));
    }
    if (stats.recent.size() != kRecentWindowCapacity) {
      return 12;  // ring buffer bounded
    }
    const std::size_t total = kRecentWindowCapacity + 50;
    if (stats.count[static_cast<std::size_t>(DaemonOp::Query)] != total) {
      return 13;  // lifetime total counts every op, not just the window
    }
    // Half were zero-hit (even indices: 0..total-1 -> ceil(total/2)).
    const std::size_t expected_zero = (total + 1) / 2;
    if (stats.query_zero_hits != expected_zero) {
      return 14;
    }
  }

  // --- op_stats_json surfaces lifetime totals, recent window, and zero-hit rate ---
  {
    DaemonOpStats stats;
    stats.record(DaemonOp::Query, 10.0, false);
    stats.record(DaemonOp::Query, 30.0, true);
    stats.record(DaemonOp::Status, 1.0, false);
    const auto json = op_stats_json(stats);
    if (json["query_count"] != 2 || json["query_zero_hits"] != 1) {
      return 15;
    }
    if (!approx(json["query_zero_hit_rate"].get<double>(), 0.5)) {
      return 16;
    }
    if (json["total_ops"] != 3) {
      return 17;
    }
    if (json["recent_window"]["size"] != 3 || json["recent_window"]["capacity"] != kRecentWindowCapacity) {
      return 18;
    }
    // Lifetime mean for query = (10+30)/2 = 20ms.
    if (!approx(json["lifetime"]["query"]["mean_ms"].get<double>(), 20.0)) {
      return 19;
    }
  }

  // ===== Durable ledger (persist-op-stats-ledger) =====

  // --- latency_bucket: pinned log2 layout, boundaries + overflow ---
  {
    if (kHistBucketCount != 12) {
      return 20;
    }
    if (latency_bucket(0.4) != 0 || latency_bucket(1.0) != 1 || latency_bucket(1.5) != 1 ||
        latency_bucket(2.0) != 2 || latency_bucket(1023.0) != 10 || latency_bucket(1024.0) != 11 ||
        latency_bucket(4000.0) != 11) {
      return 21;
    }
  }

  // --- substantive-op gate for the flush ---
  {
    DaemonOpStats only_status;
    only_status.record(DaemonOp::Status, 1.0, false);
    only_status.record(DaemonOp::Shutdown, 1.0, false);
    if (only_status.has_substantive_ops()) {
      return 22;  // status/shutdown-only lifetime must not flush
    }
    DaemonOpStats with_query;
    with_query.record(DaemonOp::Query, 1.0, false);
    if (!with_query.has_substantive_ops()) {
      return 23;
    }
  }

  // --- ISO-8601 UTC round trip ---
  {
    const auto tp = parse_iso8601_utc("2026-06-15T09:00:00Z");
    if (!tp || format_iso8601_utc(*tp) != "2026-06-15T09:00:00Z") {
      return 24;
    }
    if (parse_iso8601_utc("not-a-time").has_value()) {
      return 25;
    }
  }

  // --- ledger line: schema, timestamps, per-op counts + histogram ---
  {
    DaemonOpStats stats;
    stats.record(DaemonOp::Query, 0.5, true);   // bucket 0, zero-hit
    stats.record(DaemonOp::Query, 1.5, false);  // bucket 1
    stats.record(DaemonOp::Query, 1.5, false);  // bucket 1
    stats.record(DaemonOp::Query, 3.0, false);  // bucket 2
    const auto boot = parse_iso8601_utc("2026-06-15T09:00:00Z");
    const auto down = parse_iso8601_utc("2026-06-15T09:01:40Z");  // +100s
    const auto line = op_stats_ledger_line(stats, *boot, *down);
    if (line["schema_version"] != kLedgerSchemaVersion) {
      return 26;
    }
    if (line["boot"] != "2026-06-15T09:00:00Z" || line["shutdown"] != "2026-06-15T09:01:40Z") {
      return 27;
    }
    if (!approx(line["uptime_seconds"].get<double>(), 100.0)) {
      return 28;
    }
    if (line["query_zero_hits"] != 1) {
      return 29;
    }
    const auto& qo = line["ops"]["query"];
    if (qo["count"] != 4 || !approx(qo["total_ms"].get<double>(), 6.5)) {
      return 30;
    }
    const auto& h = qo["hist_ms"];
    if (h.size() != kHistBucketCount || h[0] != 1 || h[1] != 2 || h[2] != 1) {
      return 31;
    }
  }

  // --- histogram percentile (interpolated within the containing bucket) ---
  {
    std::array<std::size_t, kHistBucketCount> hist{};
    hist[0] = 1;
    hist[1] = 2;
    hist[2] = 1;  // total 4; p50 -> into bucket 1 [1,2) -> 1.5
    if (!approx(histogram_percentile(hist, 0.5), 1.5)) {
      return 32;
    }
    std::array<std::size_t, kHistBucketCount> empty{};
    if (!approx(histogram_percentile(empty, 0.5), 0.0)) {
      return 33;  // empty histogram -> 0, no UB
    }
  }

  // --- rollup: window filter, summed counts, weighted mean, zero-hit, mixed version ---
  {
    DaemonOpStats a;
    a.record(DaemonOp::Query, 2.0, false);
    a.record(DaemonOp::Query, 2.0, true);  // zero-hit
    DaemonOpStats b;
    b.record(DaemonOp::Query, 6.0, false);
    DaemonOpStats older;
    older.record(DaemonOp::Query, 99.0, false);

    const auto t0 = parse_iso8601_utc("2026-06-15T10:00:00Z");
    const auto t1 = parse_iso8601_utc("2026-06-15T11:00:00Z");
    const auto told = parse_iso8601_utc("2026-06-14T10:00:00Z");
    std::vector<nlohmann::json> lines = {
        op_stats_ledger_line(a, *t0, *t0),
        op_stats_ledger_line(b, *t1, *t1),
        op_stats_ledger_line(older, *told, *told),  // out of window
    };

    const auto since = parse_iso8601_utc("2026-06-15T00:00:00Z");
    const auto roll = aggregate_op_stats_ledger(lines, *since);
    if (roll["lifetimes"] != 2) {
      return 34;  // older lifetime excluded
    }
    if (roll["query"]["count"] != 3 || roll["query"]["zero_hits"] != 1) {
      return 35;
    }
    if (!approx(roll["query"]["zero_hit_rate"].get<double>(), 1.0 / 3.0)) {
      return 36;
    }
    if (!approx(roll["ops"]["query"]["mean_ms"].get<double>(), 10.0 / 3.0)) {
      return 37;  // weighted mean (2+2+6)/3
    }
    if (roll["mixed_schema_versions"] != false) {
      return 38;
    }

    // A differing schema_version flags mixed (histograms not merged across versions).
    auto future = op_stats_ledger_line(b, *t1, *t1);
    future["schema_version"] = kLedgerSchemaVersion + 1;
    lines.push_back(future);
    if (aggregate_op_stats_ledger(lines, *since)["mixed_schema_versions"] != true) {
      return 39;
    }
  }

  // --- append round-trip: write a line, read it back tolerantly, re-aggregate ---
  {
    const auto dir = std::filesystem::temp_directory_path() / "cgraph-op-stats-ledger-test";
    std::filesystem::remove_all(dir);
    const auto path = dir / "cgraph-out" / "op-stats-ledger.jsonl";

    DaemonOpStats s;
    s.record(DaemonOp::Query, 5.0, false);
    const auto t = parse_iso8601_utc("2026-06-15T12:00:00Z");
    if (!append_op_stats_ledger(path, op_stats_ledger_line(s, *t, *t))) {
      return 40;
    }

    std::vector<nlohmann::json> back;
    std::ifstream input(path);
    std::string ln;
    while (std::getline(input, ln)) {
      if (ln.empty()) {
        continue;
      }
      auto parsed = nlohmann::json::parse(ln, nullptr, false);
      if (!parsed.is_discarded()) {
        back.push_back(parsed);
      }
    }
    if (back.size() != 1) {
      return 41;
    }
    const auto since = parse_iso8601_utc("2026-06-15T00:00:00Z");
    if (aggregate_op_stats_ledger(back, *since)["query"]["count"] != 1) {
      return 42;
    }
    std::filesystem::remove_all(dir);
  }

  // --- context adaptive + zero-hit counters: record, persist, roll up, back-compat ---
  {
    DaemonOpStats s;
    s.record(DaemonOp::Context, 3.0, /*zero_hit=*/false, /*adaptive_context_call=*/true);
    s.record(DaemonOp::Context, 3.0, /*zero_hit=*/false, /*adaptive_context_call=*/true);
    s.record(DaemonOp::Context, 3.0, /*zero_hit=*/false, /*adaptive_context_call=*/false);  // fixed
    s.record(DaemonOp::Context, 3.0, /*zero_hit=*/true, /*adaptive_context_call=*/false);    // unresolved focus
    if (s.adaptive_context != 2 || s.context_zero_hits != 1) {
      return 43;  // counters routed only for the context op
    }

    const auto t = parse_iso8601_utc("2026-06-15T13:00:00Z");
    const auto line = op_stats_ledger_line(s, *t, *t);
    if (line["adaptive_context"] != 2 || line["context_zero_hits"] != 1) {
      return 44;  // persisted on the ledger line
    }

    const auto since = parse_iso8601_utc("2026-06-15T00:00:00Z");
    const auto roll = aggregate_op_stats_ledger({line}, *since);
    const auto& ctx = roll["ops"]["context"];
    if (ctx["adaptive_count"] != 2 || ctx["zero_hits"] != 1) {
      return 45;  // surfaced on the context op in the rollup
    }
    if (ctx["count"] != 4) {
      return 46;  // adaptive count is independent of the total context count
    }

    // A line written before these fields existed rolls up as zero, no error.
    auto legacy = op_stats_ledger_line(s, *t, *t);
    legacy.erase("adaptive_context");
    legacy.erase("context_zero_hits");
    const auto roll2 = aggregate_op_stats_ledger({legacy}, *since)["ops"]["context"];
    if (roll2["adaptive_count"] != 0 || roll2["zero_hits"] != 0) {
      return 47;
    }
  }

  // --- session-memory: remember/recall + recall_zero_hits in the durable ledger,
  //     additive and backward compatible (no schema bump, old lines parse) ---
  {
    DaemonOpStats s;
    s.record(DaemonOp::Remember, 2.0, /*zero_hit=*/false);
    s.record(DaemonOp::Recall, 1.0, /*zero_hit=*/false);  // hit
    s.record(DaemonOp::Recall, 1.0, /*zero_hit=*/true);   // miss
    if (s.recall_zero_hits != 1) {
      return 48;  // routed only for the recall op
    }

    const auto t = parse_iso8601_utc("2026-06-16T13:00:00Z");
    const auto line = op_stats_ledger_line(s, *t, *t);
    // schema unchanged; remember/recall now carried in the per-op object + a
    // top-level recall_zero_hits.
    if (line["schema_version"] != kLedgerSchemaVersion || line["recall_zero_hits"] != 1 ||
        line["ops"]["remember"]["count"] != 1 || line["ops"]["recall"]["count"] != 2) {
      return 49;
    }

    const auto since = parse_iso8601_utc("2026-06-16T00:00:00Z");
    const auto roll = aggregate_op_stats_ledger({line}, *since);
    if (roll["ops"]["recall"]["count"] != 2 || roll["ops"]["recall"]["zero_hits"] != 1 ||
        roll["ops"]["remember"]["count"] != 1) {
      return 50;  // summed + surfaced in the rollup
    }

    // A pre-change line lacking remember/recall op entries and recall_zero_hits
    // still parses and rolls up, contributing zero (no schema bump means merge OK).
    auto legacy = op_stats_ledger_line(s, *t, *t);
    legacy.erase("recall_zero_hits");
    legacy["ops"].erase("remember");
    legacy["ops"].erase("recall");
    const auto roll2 = aggregate_op_stats_ledger({legacy}, *since);
    if (roll2["mixed_schema_versions"] != false || roll2["ops"]["recall"]["zero_hits"] != 0 ||
        roll2["ops"]["recall"]["count"] != 0 || roll2["ops"]["remember"]["count"] != 0) {
      return 51;
    }
  }

  // --- not-ready reads are excluded from op/zero-hit/recent accounting ---
  {
    DaemonOpStats stats;
    stats.record(DaemonOp::Query, 1.0, /*zero_hit=*/false);                         // ready, real
    stats.record(DaemonOp::Query, 1.0, /*zero_hit=*/true, false, /*not_ready=*/true);  // building
    stats.record(DaemonOp::Query, 1.0, /*zero_hit=*/true, false, /*not_ready=*/true);  // building
    if (stats.count[static_cast<std::size_t>(DaemonOp::Query)] != 1) {
      return 60;  // building reads excluded from the op count
    }
    if (stats.query_zero_hits != 0) {
      return 61;  // a building read is never a miss
    }
    if (stats.not_ready != 2 || stats.recent.size() != 1) {
      return 62;  // tracked separately; kept out of the recent window
    }
  }

  // --- query route bucketing (entity / structural / search) ---
  {
    DaemonOpStats stats;
    stats.note_query_route("entity");
    stats.note_query_route("callers");      // structural intent name
    stats.note_query_route("references");   // structural intent name
    stats.note_query_route("search");
    stats.note_query_route("");             // absent -> search
    if (stats.query_route_entity != 1 || stats.query_route_structural != 2 ||
        stats.query_route_search != 2) {
      return 63;
    }
  }

  // --- not_ready + route counts round-trip through the durable ledger ---
  {
    DaemonOpStats stats;
    stats.record(DaemonOp::Query, 1.0, false);
    stats.record(DaemonOp::Query, 1.0, false, false, /*not_ready=*/true);
    stats.note_query_route("entity");
    stats.note_query_route("callers");
    const auto boot = parse_iso8601_utc("2026-06-18T00:00:00Z");
    const auto line = op_stats_ledger_line(stats, *boot, *boot);
    if (line.value("not_ready", std::size_t{0}) != 1 ||
        line.value("query_route_entity", std::size_t{0}) != 1 ||
        line.value("query_route_structural", std::size_t{0}) != 1) {
      return 64;
    }
    const auto roll = aggregate_op_stats_ledger({line}, WallClock::time_point{});
    if (roll.value("not_ready", std::size_t{0}) != 1 ||
        roll["ops"]["query"]["routes"]["entity"] != 1 ||
        roll["ops"]["query"]["routes"]["structural"] != 1) {
      return 65;
    }
  }

  return 0;
}
