#include "cgraph/daemon_ops.hpp"

#include "cgraph/protocol.hpp"
#include "cgraph/semantic_cache.hpp"

#include <iostream>

namespace {

void expect(bool& ok, bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

bool status_has_state(cgraph::DaemonState& state, cgraph::EnrichmentState enrichment_state, const char* expected) {
  state.enrichment_state = enrichment_state;
  const auto response = cgraph::handle_daemon_request(state, cgraph::make_request("status"));
  return response["ok"] == true && response["result"]["enrichment_state"] == expected;
}

}  // namespace

int main() {
  bool ok = true;
  cgraph::DaemonState state;
  state.enrichment_pending = 2;
  state.enrichment_running = 1;
  state.enrichment_stale = 3;
  state.enrichment_failed = 4;

  expect(ok, status_has_state(state, cgraph::EnrichmentState::Idle, "idle"), "state: idle");
  expect(ok, status_has_state(state, cgraph::EnrichmentState::Pending, "pending"), "state: pending");
  expect(ok, status_has_state(state, cgraph::EnrichmentState::Running, "running"), "state: running");
  expect(ok, status_has_state(state, cgraph::EnrichmentState::Stale, "stale"), "state: stale");
  expect(ok, status_has_state(state, cgraph::EnrichmentState::Failed, "failed"), "state: failed");

  const auto response = cgraph::handle_daemon_request(state, cgraph::make_request("status"));
  expect(ok, response["result"]["enrichment_pending"] == 2, "counter: pending");
  expect(ok, response["result"]["enrichment_running"] == 1, "counter: running");
  expect(ok, response["result"]["enrichment_stale"] == 3, "counter: stale");
  expect(ok, response["result"]["enrichment_failed"] == 4, "counter: failed");

  // Current-state derivation: counters drive state precedence
  // Failed > Pending > Idle when not running
  {
    cgraph::DaemonState s;
    s.enrichment_failed = 1;
    s.enrichment_pending = 2;
    s.enrichment_state = cgraph::EnrichmentState::Failed;
    const auto r = cgraph::handle_daemon_request(s, cgraph::make_request("status"));
    expect(ok, r["result"]["enrichment_state"] == "failed", "precedence: failed > pending");
  }

  // Pending when no failures
  {
    cgraph::DaemonState s;
    s.enrichment_failed = 0;
    s.enrichment_pending = 3;
    s.enrichment_state = cgraph::EnrichmentState::Pending;
    const auto r = cgraph::handle_daemon_request(s, cgraph::make_request("status"));
    expect(ok, r["result"]["enrichment_state"] == "pending", "precedence: pending when no failures");
  }

  // Idle when all clear
  {
    cgraph::DaemonState s;
    s.enrichment_failed = 0;
    s.enrichment_pending = 0;
    s.enrichment_stale = 0;
    s.enrichment_state = cgraph::EnrichmentState::Idle;
    const auto r = cgraph::handle_daemon_request(s, cgraph::make_request("status"));
    expect(ok, r["result"]["enrichment_state"] == "idle", "precedence: idle when all clear");
  }

  // Successful replacement should clear failure (tested via cache v2)
  {
    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord failed_rec;
    failed_rec.source_path = "/docs/test.md";
    failed_rec.state = cgraph::SemanticCacheState::Failed;
    failed_rec.last_error = "old error";
    cache.upsert(failed_rec);

    expect(ok, cache.count_failed() == 1, "pre-replace: 1 failed");

    cgraph::SemanticCacheRecord valid_rec;
    valid_rec.source_path = "/docs/test.md";
    valid_rec.state = cgraph::SemanticCacheState::Valid;
    valid_rec.last_error.clear();
    cache.upsert(valid_rec);

    expect(ok, cache.count_failed() == 0, "post-replace: 0 failed");
    expect(ok, cache.count_valid() == 1, "post-replace: 1 valid");
    const auto r = cache.find_for_source("/docs/test.md");
    expect(ok, r->last_error.empty(), "post-replace: error cleared");
  }

  return ok ? 0 : 1;
}
