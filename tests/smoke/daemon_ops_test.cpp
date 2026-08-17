#include "cgraph/daemon_ops.hpp"

#include "cgraph/daemon_lifecycle.hpp"
#include "cgraph/file_cache.hpp"
#include "cgraph/fragment_json.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/semantic_fragment_validation.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>

int main() {
  namespace fs = std::filesystem;
  // A real source file so the explain snippet exercises actual file reads, not a
  // stub. Node "a" points at line 2 (the class), so the snippet must start there.
  const auto src = fs::temp_directory_path() / "cgraph-daemon-ops-test" / "alpha.cpp";
  fs::remove_all(src.parent_path());
  fs::create_directories(src.parent_path());
  const std::string source_contents =
      "// header line\n"
      "class Alpha {\n"
      "  void run();\n"
      "};\n";
  std::ofstream(src, std::ios::binary) << source_contents;

  cgraph::DaemonState state;
  state.pid = 123;

  // Before any build publishes, the daemon serves the default Empty snapshot;
  // read ops must say so, or an agent can't tell "no match" from "still building".
  const auto during_build = cgraph::handle_daemon_request(state, cgraph::make_request("query", {{"q", "Alpha"}}));
  if (during_build["result"].value("graph_state", std::string{}) != "building") {
    return 1;
  }
  const auto empty_status = cgraph::handle_daemon_request(state, cgraph::make_request("status"));
  if (empty_status["result"].value("build_state", std::string{}) != "building") {
    return 1;
  }

  cgraph::GraphSnapshot graph;
  graph.build_state = cgraph::BuildState::DeterministicReady;
  graph.content_root = cgraph::ContentRoot{
      .algorithm = "sha256-merkle-v1",
      .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      .leaf_count = 1,
  };
  // "a" is the central hub (centrality 1.0, a god node); "AlphaLeaf" also matches
  // the "Alpha" query but is peripheral. Edges c -> a -> b form a chain so the
  // blast radius has two depths to walk.
  graph.nodes.push_back(cgraph::Node{
      .id = "a",
      .label = "Alpha",
      .source_file = src.generic_string(),
      .source_location = cgraph::SourceLocation{.start_line = 2, .start_column = 0, .end_line = 4, .end_column = 1},
      .kind = "class",
      .properties = {{"degree_centrality", "1.000000"}, {"god_node", "true"}}});
  graph.nodes.push_back(cgraph::Node{.id = "b", .label = "Beta", .kind = "function"});
  graph.nodes.push_back(cgraph::Node{
      .id = "c", .label = "AlphaLeaf", .kind = "function", .properties = {{"degree_centrality", "0.100000"}}});
  // "d" carries a full-signature label, the shape real extractors emit.
  graph.nodes.push_back(cgraph::Node{.id = "d", .label = "gamma_run(int x, bool dry)", .kind = "function"});
  graph.edges.push_back(cgraph::Edge{.source = "a", .target = "b", .relation = "CALLS"});
  graph.edges.push_back(cgraph::Edge{.source = "c", .target = "a", .relation = "CALLS"});
  const auto source_sha256 = cgraph::sha256_hex(source_contents);
  graph.source_hashes[src.lexically_normal().generic_string()] = source_sha256;
  cgraph::publish_graph_snapshot(state, std::move(graph));

  const auto status = cgraph::handle_daemon_request(state, cgraph::make_request("status"));
  if (!status["ok"].get<bool>() || status["result"]["node_count"] != 4 ||
      status["result"].value("build_state", std::string{}) != "ready") {
    return 1;
  }

  // Verified freshness: status and each graph read identify the immutable
  // snapshot selected for the request. A caller can pin that identity and gets
  // no graph data if a later snapshot does not match it.
  if (!status["result"].contains("freshness")) {
    return 200;
  }
  const auto& status_freshness = status["result"]["freshness"];
  const auto content_root = status_freshness.value("content_root", std::string{});
  if (!status_freshness.value("verified", false) ||
      status_freshness.value("algorithm", std::string{}) != "sha256-merkle-v1" ||
      content_root != "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" ||
      status_freshness.value("leaf_count", 0U) != 1U) {
    return 200;
  }
  const auto has_pinned_freshness = [&content_root](const nlohmann::json& response) {
    if (!response.value("ok", false) || !response.contains("result")) {
      return false;
    }
    const auto& freshness = response["result"]["freshness"];
    return freshness.value("verified", false) &&
           freshness.value("algorithm", std::string{}) == "sha256-merkle-v1" &&
           freshness.value("content_root", std::string{}) == content_root && freshness.contains("leaf_count");
  };

  const auto pinned_query = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "pha"}, {"expected_content_root", content_root}}));
  const auto pinned_path = cgraph::handle_daemon_request(
      state, cgraph::make_request("path", {{"source", "a"}, {"target", "b"}, {"expected_content_root", content_root}}));
  const auto pinned_explain = cgraph::handle_daemon_request(
      state, cgraph::make_request("explain", {{"id", "a"}, {"expected_content_root", content_root}}));
  const auto pinned_impact = cgraph::handle_daemon_request(
      state, cgraph::make_request("impact", {{"id", "b"}, {"expected_content_root", content_root}}));
  const auto pinned_context = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"id", "a"}, {"budget", 5000}, {"expected_content_root", content_root}}));
  if (!has_pinned_freshness(pinned_query) || !has_pinned_freshness(pinned_path) ||
      !has_pinned_freshness(pinned_explain) || !has_pinned_freshness(pinned_impact) ||
      !has_pinned_freshness(pinned_context)) {
    return 201;
  }
  if (pinned_explain["result"].value("source_sha256", std::string{}) != source_sha256 ||
      pinned_context["result"]["focus"].value("source_sha256", std::string{}) != source_sha256) {
    return 204;
  }

  const auto stale_pin = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "pha"}, {"expected_content_root", "stale-content-root"}}));
  if (stale_pin.value("ok", true) || stale_pin.contains("result")) {
    return 202;
  }

  cgraph::DaemonState rootless_state;
  const auto empty_pin = cgraph::handle_daemon_request(
      rootless_state, cgraph::make_request("query", {{"q", "anything"}, {"expected_content_root", ""}}));
  if (empty_pin.value("ok", true) || empty_pin.contains("result")) {
    return 203;
  }

  // Pinned snippet reads verify every returned source against the selected
  // snapshot ledger. A later byte change aborts an otherwise partially-built
  // context response; unpinned reads keep serving the current working-tree bytes.
  {
    const auto verified_source = src.parent_path() / "verified.cpp";
    const auto selected_source = src.parent_path() / "selected.cpp";
    const auto changed_source = src.parent_path() / "changed.cpp";
    const std::string verified_bytes =
        "void Verified() {}\n"
        "void SelectedSame() {}\n";
    const std::string selected_bytes = "void SelectedDistinct() {}\n";
    const std::string old_changed_bytes = "void ChangedOld() {}\n";
    const std::string new_changed_bytes = "void ChangedNew() {}\n";
    std::ofstream(verified_source, std::ios::binary) << verified_bytes;
    std::ofstream(selected_source, std::ios::binary) << selected_bytes;
    std::ofstream(changed_source, std::ios::binary) << old_changed_bytes;

    cgraph::GraphSnapshot pinned_graph;
    pinned_graph.build_state = cgraph::BuildState::DeterministicReady;
    pinned_graph.content_root = cgraph::ContentRoot{
        .algorithm = "sha256-merkle-v1",
        .sha256 = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        .leaf_count = 3,
    };
    const auto pinned_content_root = pinned_graph.content_root.sha256;
    pinned_graph.nodes.push_back(cgraph::Node{
        .id = "verified",
        .label = "Verified",
        .source_file = verified_source.generic_string(),
        .source_location = cgraph::SourceLocation{.start_line = 1, .end_line = 1, .end_column = 18},
        .kind = "function"});
    pinned_graph.nodes.push_back(cgraph::Node{
        .id = "selected_same",
        .label = "SelectedSame",
        .source_file = verified_source.generic_string(),
        .source_location = cgraph::SourceLocation{.start_line = 2, .end_line = 2, .end_column = 22},
        .kind = "function",
        .properties = {{"degree_centrality", "0.300000"}}});
    pinned_graph.nodes.push_back(cgraph::Node{
        .id = "selected_distinct",
        .label = "SelectedDistinct",
        .source_file = selected_source.generic_string(),
        .source_location = cgraph::SourceLocation{.start_line = 1, .end_line = 1, .end_column = 26},
        .kind = "function",
        .properties = {{"degree_centrality", "0.200000"}}});
    pinned_graph.nodes.push_back(cgraph::Node{
        .id = "changed",
        .label = "Changed",
        .source_file = changed_source.generic_string(),
        .source_location = cgraph::SourceLocation{.start_line = 1, .end_line = 40, .end_column = 500},
        .kind = "function"});
    pinned_graph.edges.push_back(
        cgraph::Edge{.source = "verified", .target = "changed", .relation = "CALLS"});
    pinned_graph.edges.push_back(
        cgraph::Edge{.source = "verified", .target = "selected_same", .relation = "CALLS"});
    pinned_graph.edges.push_back(
        cgraph::Edge{.source = "verified", .target = "selected_distinct", .relation = "CALLS"});
    pinned_graph.source_hashes[verified_source.lexically_normal().generic_string()] =
        cgraph::sha256_hex(verified_bytes);
    pinned_graph.source_hashes[selected_source.lexically_normal().generic_string()] =
        cgraph::sha256_hex(selected_bytes);
    pinned_graph.source_hashes[changed_source.lexically_normal().generic_string()] =
        cgraph::sha256_hex(old_changed_bytes);

    cgraph::DaemonState pinned_state;
    cgraph::publish_graph_snapshot(pinned_state, pinned_graph);
    std::ofstream(changed_source, std::ios::binary | std::ios::trunc) << new_changed_bytes;

    const auto emitted_entry_tokens = [](const nlohmann::json& result) {
      // Mirrors the engine: the ARRAY is measured, not a per-entry sum, so the
      // framing bytes count too.
      return (result.at("focus").dump().size() + 3U) / 4U +
             (result.at("included").dump().size() + 3U) / 4U;
    };
    // Small enough that the `changed` candidate (a ~5000-slice-token span) can
    // never be selected, large enough that the measured response holding both
    // small selected entries fits -- the budget is a real ceiling now.
    constexpr std::size_t kKnapsackSelectionBudget = 400;

    // Knapsack selection is metadata-only: the changed candidate is too large to
    // select, so its stale bytes cannot abort the pinned response. The focus and
    // one included node share a file, while the other included node is distinct;
    // the observable read count must therefore be exactly two.
    const auto selected_only_context = cgraph::handle_daemon_request(
        pinned_state,
        cgraph::make_request(
            "context",
            {{"id", "verified"},
             {"q", "Selected"},
             {"budget", kKnapsackSelectionBudget},
             {"packing", "knapsack"},
             {"max_depth", 1},
             {"gather", "fixed"},
             {"expected_content_root", pinned_content_root}}));
    std::set<std::string> selected_ids;
    std::set<std::string> selected_source_files;
    std::set<std::string> selected_source_hashes;
    std::size_t selected_emitted_tokens = 0;
    if (selected_only_context.contains("result")) {
      selected_emitted_tokens = emitted_entry_tokens(selected_only_context["result"]);
      const auto& selected_focus = selected_only_context["result"]["focus"];
      selected_source_files.insert(selected_focus.value("source_file", std::string{}));
      selected_source_hashes.insert(selected_focus.value("source_sha256", std::string{}));
      for (const auto& item : selected_only_context["result"]["included"]) {
        selected_ids.insert(item.value("id", std::string{}));
        selected_source_files.insert(item.value("source_file", std::string{}));
        selected_source_hashes.insert(item.value("source_sha256", std::string{}));
      }
    }
    if (!selected_only_context.value("ok", false) ||
        selected_source_files.size() != 2U ||
        selected_only_context["result"].value("source_files_read", 0U) !=
            selected_source_files.size() ||
        selected_only_context["result"].value("tokens_used", 0U) !=
            selected_emitted_tokens ||
        selected_only_context["result"].value("selection_tokens_used", 0U) >
            kKnapsackSelectionBudget ||
        selected_only_context["result"].value("budget_basis", std::string{}) !=
            "measured_serialized_tokens" ||
        selected_emitted_tokens > kKnapsackSelectionBudget ||
        selected_ids != std::set<std::string>{"selected_distinct", "selected_same"} ||
        selected_source_hashes !=
            std::set<std::string>{cgraph::sha256_hex(selected_bytes),
                                  cgraph::sha256_hex(verified_bytes)}) {
      return 210;
    }

    // Fixed/greedy packing also classifies candidates from metadata first. The
    // stale oversized candidate is returned brief-only without being verified;
    // only the two distinct files that actually supply snippets are read.
    const auto selected_only_greedy_context = cgraph::handle_daemon_request(
        pinned_state,
        cgraph::make_request(
            "context",
            {{"id", "verified"},
             {"budget", 500},
             {"packing", "greedy"},
             {"max_depth", 1},
             {"gather", "fixed"},
             {"expected_content_root", pinned_content_root}}));
    std::set<std::string> greedy_snippet_ids;
    std::set<std::string> greedy_snippet_files;
    std::set<std::string> greedy_source_hashes;
    std::size_t greedy_emitted_tokens = 0;
    bool changed_is_brief_only = false;
    if (selected_only_greedy_context.contains("result")) {
      greedy_emitted_tokens = emitted_entry_tokens(selected_only_greedy_context["result"]);
      const auto observe_snippet = [&](const nlohmann::json& item) {
        if (!item.contains("snippet")) {
          return;
        }
        greedy_snippet_ids.insert(item.value("id", std::string{}));
        greedy_snippet_files.insert(item.value("source_file", std::string{}));
        greedy_source_hashes.insert(item.value("source_sha256", std::string{}));
      };
      observe_snippet(selected_only_greedy_context["result"]["focus"]);
      for (const auto& item : selected_only_greedy_context["result"]["included"]) {
        observe_snippet(item);
        if (item.value("id", std::string{}) == "changed") {
          changed_is_brief_only = item.value("snippet_omitted", false) &&
                                  !item.contains("snippet") &&
                                  !item.contains("source_sha256");
        }
      }
    }
    if (!selected_only_greedy_context.value("ok", false) ||
        selected_only_greedy_context["result"].value("packing", std::string{}) != "greedy" ||
        selected_only_greedy_context["result"].value("tokens_used", 0U) !=
            greedy_emitted_tokens ||
        selected_only_greedy_context["result"].value(
            "selection_tokens_used", 0U) > 500U ||
        selected_only_greedy_context["result"].value(
            "budget_basis", std::string{}) != "measured_serialized_tokens" ||
        !changed_is_brief_only ||
        greedy_snippet_ids !=
            std::set<std::string>{"selected_distinct", "selected_same", "verified"} ||
        greedy_snippet_files.size() != 2U ||
        selected_only_greedy_context["result"].value("source_files_read", 0U) !=
            greedy_snippet_files.size() ||
        greedy_source_hashes !=
            std::set<std::string>{cgraph::sha256_hex(selected_bytes),
                                  cgraph::sha256_hex(verified_bytes)}) {
      return 211;
    }

    const auto mismatched_context = cgraph::handle_daemon_request(
        pinned_state,
        cgraph::make_request(
            "context",
            {{"id", "verified"},
             {"budget", 20000},
             {"max_depth", 1},
             {"gather", "fixed"},
             {"expected_content_root", pinned_content_root}}));
    if (mismatched_context.value("ok", true) || mismatched_context.contains("result") ||
        mismatched_context.value("error", std::string{}).find("source-snapshot-mismatch") == std::string::npos ||
        mismatched_context.value("error", std::string{}).find("synchronize again") == std::string::npos) {
      return 205;
    }

    const auto mismatched_entity = cgraph::handle_daemon_request(
        pinned_state,
        cgraph::make_request(
            "query", {{"q", "Changed"}, {"expected_content_root", pinned_content_root}}));
    if (mismatched_entity.value("ok", true) || mismatched_entity.contains("result")) {
      return 206;
    }

    const auto eventual_explain = cgraph::handle_daemon_request(
        pinned_state, cgraph::make_request("explain", {{"id", "changed"}}));
    if (!eventual_explain.value("ok", false) ||
        eventual_explain["result"].value("snippet", std::string{}).find("ChangedNew") == std::string::npos ||
        eventual_explain["result"].value("source_sha256", std::string{}) != cgraph::sha256_hex(new_changed_bytes)) {
      return 207;
    }

    auto missing_evidence_graph = pinned_graph;
    missing_evidence_graph.source_hashes.erase(verified_source.lexically_normal().generic_string());
    cgraph::DaemonState missing_evidence_state;
    cgraph::publish_graph_snapshot(missing_evidence_state, std::move(missing_evidence_graph));
    const auto missing_evidence = cgraph::handle_daemon_request(
        missing_evidence_state,
        cgraph::make_request(
            "explain", {{"id", "verified"}, {"expected_content_root", pinned_content_root}}));
    if (missing_evidence.value("ok", true) || missing_evidence.contains("result") ||
        missing_evidence.value("error", std::string{}).find("source-snapshot-mismatch") == std::string::npos) {
      return 208;
    }

    fs::remove(verified_source);
    cgraph::DaemonState missing_file_state;
    cgraph::publish_graph_snapshot(missing_file_state, std::move(pinned_graph));
    const auto missing_file = cgraph::handle_daemon_request(
        missing_file_state,
        cgraph::make_request(
            "explain", {{"id", "verified"}, {"expected_content_root", pinned_content_root}}));
    if (missing_file.value("ok", true) || missing_file.contains("result") ||
        missing_file.value("error", std::string{}).find("source-snapshot-mismatch") == std::string::npos) {
      return 209;
    }
  }

  // A bare symbol name resolves against the label's leading token, so an agent
  // never needs the full signature ("gamma_run" -> "gamma_run(int x, bool dry)").
  const auto by_symbol = cgraph::handle_daemon_request(state, cgraph::make_request("explain", {{"id", "gamma_run"}}));
  if (by_symbol["result"].value("id", std::string{}) != "d") {
    return 1;
  }

  // A substring shared by several symbols but equal to none ("pha" -> "Alpha",
  // "AlphaLeaf") is a lexical search, case-insensitive. A ready graph carries no
  // "building" annotation.
  const auto query = cgraph::handle_daemon_request(state, cgraph::make_request("query", {{"q", "pha"}}));
  const auto& query_result = query["result"];
  const auto& query_nodes = query_result["nodes"];
  // Both "Alpha" and "AlphaLeaf" match; total reflects the full match count.
  if (query_nodes.size() != 2 || query_result.value("total", 0U) != 2U || query_result.contains("graph_state")) {
    return 1;
  }
  // Ranked by centrality: the hub "a" must lead, carrying file:line, its
  // centrality, and the god_node flag so the agent can open and prioritize it.
  const auto& hit = query_nodes[0];
  if (hit.value("source_file", std::string{}) != src.generic_string() || hit.value("line", 0U) != 2U ||
      hit.value("centrality", 0.0) < 0.99 || !hit.value("god_node", false)) {
    return 1;
  }
  if (query_nodes[1].value("label", std::string{}) != "AlphaLeaf") {
    return 1;  // peripheral node ranked below the hub
  }

  // kind/file filters narrow the match set.
  const auto by_kind = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "pha"}, {"kind", "class"}}));
  if (by_kind["result"]["nodes"].size() != 1 ||
      by_kind["result"]["nodes"][0].value("id", std::string{}) != "a") {
    return 1;
  }
  const auto by_file = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "pha"}, {"file", "alpha.cpp"}}));
  if (by_file["result"]["nodes"].size() != 1 ||
      by_file["result"]["nodes"][0].value("id", std::string{}) != "a") {
    return 1;
  }

  // A near-miss ("Alpa") returns did-you-mean suggestions instead of a bare
  // empty result, so an agent can self-correct.
  const auto miss = cgraph::handle_daemon_request(state, cgraph::make_request("query", {{"q", "Alpa"}}));
  if (miss["result"].value("total", 1U) != 0U || miss["result"]["suggestions"].empty() ||
      miss["result"]["suggestions"][0].value("label", std::string{}) != "Alpha") {
    return 1;
  }

  // Intent routing (route-query-by-intent): "who calls X" routes to incoming CALLS
  // and returns the caller symbols directly, not a name search over the words.
  // c -> a CALLS, so AlphaLeaf calls Alpha.
  const auto callers = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "who calls Alpha"}}));
  const auto& callers_r = callers["result"];
  if (callers_r.value("route", std::string{}) != "callers" || callers_r.value("of", std::string{}) != "a" ||
      callers_r["nodes"].size() != 1 || callers_r["nodes"][0].value("label", std::string{}) != "AlphaLeaf") {
    return 1;
  }
  // "what does X call" routes to outgoing CALLS. a -> b, so Alpha calls Beta.
  const auto callees = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "what does Alpha call"}}));
  const auto& callees_r = callees["result"];
  if (callees_r.value("route", std::string{}) != "callees" || callees_r["nodes"].size() != 1 ||
      callees_r["nodes"][0].value("label", std::string{}) != "Beta") {
    return 1;
  }
  // A unique exact symbol routes to the entity result with a typed-neighbor summary.
  // "Beta" is matched only by node b; its sole caller is "a" (a -> b CALLS).
  const auto entity = cgraph::handle_daemon_request(state, cgraph::make_request("query", {{"q", "Beta"}}));
  const auto& entity_r = entity["result"];
  if (entity_r.value("route", std::string{}) != "entity" || entity_r["nodes"].size() != 1 ||
      entity_r["nodes"][0].value("label", std::string{}) != "Beta" ||
      entity_r["neighbors"]["callers"].value("count", 0U) != 1U ||
      entity_r["neighbors"]["callers"]["top"][0].get<std::string>() != "a" ||
      entity_r["neighbors"]["callees"].value("count", 1U) != 0U) {
    return 1;
  }
  // A structural phrase whose operand does not resolve falls through to lexical
  // search rather than forcing an empty typed result -- worst case is the status quo.
  const auto fallthrough = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "callers of nope"}}));
  if (fallthrough["result"].value("route", std::string{}) != "search") {
    return 1;
  }
  // A precise symbol routes to entity even when its token collides as a substring of
  // other symbols: "alpha" equals the class "Alpha" exactly (one exact match) while
  // also being a substring of "AlphaLeaf". The substring collision no longer forces a
  // search -- this is the regression guard for route-query-by-intent on large graphs,
  // where exact symbol names routinely collide as substrings of many other ids/labels.
  const auto entity_collide = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "alpha"}}));
  const auto& entity_collide_r = entity_collide["result"];
  if (entity_collide_r.value("route", std::string{}) != "entity" ||
      entity_collide_r["nodes"].size() != 1 ||
      entity_collide_r["nodes"][0].value("id", std::string{}) != "a" ||
      entity_collide_r["neighbors"]["callees"].value("count", 0U) != 1U) {
    return 1;  // a -> b CALLS, so Alpha's sole callee is Beta
  }
  // A needle that equals no symbol but is a substring of several stays a search: "pha"
  // matches Alpha and AlphaLeaf but is neither, so there is no unique entity to pin.
  const auto still_search = cgraph::handle_daemon_request(
      state, cgraph::make_request("query", {{"q", "pha"}}));
  if (still_search["result"].value("route", std::string{}) != "search" ||
      still_search["result"]["nodes"].size() != 2) {
    return 1;
  }

  // Blast radius: what breaks if "b" changes? Its dependents are "a" (depth 1,
  // calls b) then "c" (depth 2, calls a). Ordered by depth.
  const auto impact = cgraph::handle_daemon_request(
      state, cgraph::make_request("impact", {{"id", "b"}, {"direction", "dependents"}, {"max_depth", 3}}));
  const auto& impact_nodes = impact["result"]["nodes"];
  if (impact["result"].value("total", 0U) != 2U || impact_nodes.size() != 2) {
    return 1;
  }
  if (impact_nodes[0].value("id", std::string{}) != "a" || impact_nodes[0].value("depth", 0) != 1 ||
      impact_nodes[1].value("id", std::string{}) != "c" || impact_nodes[1].value("depth", 0) != 2) {
    return 1;
  }
  // Depth bound is honored: max_depth 1 reaches only the direct dependent.
  const auto shallow = cgraph::handle_daemon_request(
      state, cgraph::make_request("impact", {{"id", "b"}, {"direction", "dependents"}, {"max_depth", 1}}));
  if (shallow["result"]["nodes"].size() != 1 || shallow["result"]["nodes"][0].value("id", std::string{}) != "a") {
    return 1;
  }
  // Opposite direction: what does "c" depend on? a (depth 1) then b (depth 2).
  // The seed is given by label ("AlphaLeaf") and the echo carries the canonical id.
  const auto deps = cgraph::handle_daemon_request(
      state, cgraph::make_request("impact", {{"id", "AlphaLeaf"}, {"direction", "dependencies"}, {"max_depth", 3}}));
  const auto& deps_nodes = deps["result"]["nodes"];
  if (deps["result"].value("id", std::string{}) != "c" || deps_nodes.size() != 2 ||
      deps_nodes[0].value("id", std::string{}) != "a" || deps_nodes[1].value("id", std::string{}) != "b") {
    return 1;
  }
  // An unknown seed reports found:false plus suggestions, not a silent empty.
  const auto impact_miss = cgraph::handle_daemon_request(
      state, cgraph::make_request("impact", {{"id", "AlphaLeef"}}));
  if (impact_miss["result"].value("found", true) || impact_miss["result"]["suggestions"].empty()) {
    return 1;
  }

  // Endpoints resolve by label too ("Alpha" -> id "a").
  const auto path = cgraph::handle_daemon_request(state, cgraph::make_request("path", {{"source", "Alpha"}, {"target", "b"}}));
  if (path["result"]["path"].size() != 2) {
    return 1;
  }
  // path_nodes enriches each hop; the first hop is "a" with its label + line.
  const auto& path_nodes = path["result"]["path_nodes"];
  if (path_nodes.size() != 2 || path_nodes[0].value("label", std::string{}) != "Alpha" ||
      path_nodes[0].value("line", 0U) != 2U) {
    return 1;
  }
  // A missing endpoint is flagged with suggestions; an empty path alone is
  // indistinguishable from "no route exists".
  const auto path_miss = cgraph::handle_daemon_request(
      state, cgraph::make_request("path", {{"source", "a"}, {"target", "Betb"}}));
  if (!path_miss["result"]["path"].empty() || path_miss["result"].value("target_found", true) ||
      path_miss["result"]["target_suggestions"].empty() || path_miss["result"].contains("source_found")) {
    return 1;
  }

  const auto explain = cgraph::handle_daemon_request(state, cgraph::make_request("explain", {{"id", "a"}}));
  const auto& explain_result = explain["result"];
  if (explain_result["neighbors"].empty()) {
    return 1;
  }
  // The focal node carries a location block and a snippet read from disk; the
  // snippet must begin at the class declaration (start_line 2), not the header.
  if (explain_result.value("line", 0U) != 2U || !explain_result.contains("location") ||
      explain_result["location"].value("start_line", 0U) != 2U) {
    return 1;
  }
  const auto snippet = explain_result.value("snippet", std::string{});
  if (snippet.rfind("class Alpha {", 0) != 0 || snippet.find("header line") != std::string::npos) {
    return 1;
  }
  // Neighbors are ordered most-central-first: AlphaLeaf (0.1) before Beta (no
  // centrality), and each entry describes the node on the other end with a
  // direction, so the agent can navigate without a second lookup.
  if (explain_result.value("neighbor_count", 0U) != 2U) {
    return 1;
  }
  const auto& neighbor = explain_result["neighbors"][0];
  if (neighbor.value("direction", std::string{}) != "in" ||
      neighbor["node"].value("label", std::string{}) != "AlphaLeaf") {
    return 1;
  }
  // direction filter keeps only the requested side; limit caps and flags.
  const auto outgoing = cgraph::handle_daemon_request(
      state, cgraph::make_request("explain", {{"id", "a"}, {"direction", "out"}}));
  if (outgoing["result"]["neighbors"].size() != 1 ||
      outgoing["result"]["neighbors"][0].value("direction", std::string{}) != "out" ||
      outgoing["result"]["neighbors"][0]["node"].value("label", std::string{}) != "Beta") {
    return 1;
  }
  const auto capped = cgraph::handle_daemon_request(
      state, cgraph::make_request("explain", {{"id", "a"}, {"limit", 1}}));
  if (capped["result"]["neighbors"].size() != 1 || !capped["result"].value("truncated", false) ||
      capped["result"].value("neighbor_count", 0U) != 2U) {
    return 1;
  }
  // Unknown symbol: found:false plus suggestions ("Alpah" ~ "Alpha").
  const auto explain_miss = cgraph::handle_daemon_request(
      state, cgraph::make_request("explain", {{"id", "Alpah"}}));
  if (explain_miss["result"].value("found", true) || explain_miss["result"]["suggestions"].empty() ||
      explain_miss["result"]["suggestions"][0].value("label", std::string{}) != "Alpha") {
    return 1;
  }

  // Context packing: a generous budget bundles the focal node (with its snippet)
  // plus its neighbors, staying within budget. `gather:"fixed"` is the explicit
  // opt-out of the adaptive default and must stay byte-for-byte the old behavior.
  const auto context = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"id", "a"}, {"budget", 5000}, {"max_depth", 2}, {"gather", "fixed"}}));
  const auto& ctx = context["result"];
  if (ctx["focus"].value("label", std::string{}) != "Alpha" ||
      ctx["focus"].value("snippet", std::string{}).rfind("class Alpha {", 0) != 0) {
    return 1;  // focal node leads, carrying its source
  }
  // Explicit fixed gather self-describes greedy/fixed and carries no reach summary.
  if (ctx.value("gather", std::string{}) != "fixed" || ctx.value("packing", std::string{}) != "greedy") {
    return 1;
  }
  if (ctx.contains("reach")) {
    return 1;  // reach is adaptive-only
  }
  if (ctx.value("tokens_used", 0U) == 0U || ctx.value("tokens_used", 0U) > 5000U) {
    return 1;  // packed something, and stayed within budget
  }
  // Both neighbors (b via a->b, c via c->a) are reachable undirected at depth 1.
  if (ctx["included"].size() != 2) {
    return 1;
  }
  for (const auto& item : ctx["included"]) {
    if (item.value("depth", 0) != 1) {
      return 1;
    }
  }

  // Default gather (no `gather` param) is now adaptive: it self-describes
  // adaptive/knapsack and carries the reach summary. `fixed` is opt-in only.
  const auto def_ctx = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"id", "a"}, {"budget", 5000}}));
  const auto& dctx = def_ctx["result"];
  if (dctx["focus"].value("label", std::string{}) != "Alpha" ||
      dctx.value("gather", std::string{}) != "adaptive" ||
      dctx.value("packing", std::string{}) != "knapsack" || !dctx.contains("reach")) {
    return 1;
  }

  // A budget too small for any neighbor still returns the focal node and flags
  // truncation rather than dropping everything.
  const auto tight = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"id", "a"}, {"budget", 1}}));
  const auto& tight_ctx = tight["result"];
  if (tight_ctx["focus"].value("label", std::string{}) != "Alpha" ||
      !tight_ctx.value("truncated", false) || !tight_ctx["included"].empty()) {
    return 1;
  }

  // The budget is a measured ceiling in BOTH packing modes (openspec:
  // honest-context-budget): the serialized response never exceeds it except
  // when the never-dropped focal alone does not fit (then `included` must be
  // empty), `tokens_used` IS the measured cost (mirroring estimate_tokens'
  // chars/4), and every snippet-less row carries an explicit marker so a
  // failed read is distinguishable from a structurally sourceless kind.
  // 6000 is the shipped default budget.
  for (const auto budget : {50U, 200U, 1000U, 5000U, 6000U}) {
    for (const bool greedy_mode : {false, true}) {
      auto sweep_params = nlohmann::json{{"id", "a"}, {"budget", budget}};
      if (greedy_mode) {
        sweep_params["gather"] = "fixed";
        sweep_params["packing"] = "greedy";
      }
      const auto packed =
          cgraph::handle_daemon_request(state, cgraph::make_request("context", sweep_params));
      const auto& pctx = packed["result"];
      const auto focus_tokens = (pctx["focus"].dump().size() + 3) / 4;
      const auto array_tokens = (pctx["included"].dump().size() + 3) / 4;
      const auto measured = focus_tokens + array_tokens;
      if (measured > budget && !pctx["included"].empty()) {
        return 1;  // ceiling: over budget only when the focal alone cannot fit
      }
      if (pctx.value("tokens_used", 0U) != measured) {
        return 1;  // the report is the measured cost, never an estimate
      }
      for (const auto& item : pctx["included"]) {
        if (!item.contains("snippet") && !item.value("snippet_omitted", false) &&
            !item.value("snippet_unavailable", false)) {
          return 1;  // every snippet-less row is explicitly marked
        }
      }
    }
  }

  // status carries the build identity so a stale installed binary is
  // detectable from the daemon surface.
  const auto identity_status = cgraph::handle_daemon_request(state, cgraph::make_request("status"));
  if (identity_status["result"].value("engine_version", std::string{}).empty() ||
      identity_status["result"].value("engine_revision", std::string{}).empty()) {
    return 1;
  }

  // Hostile parameters must yield error frames, never an uncaught throw that
  // kills the resident daemon (a mistyped param reached graphd verbatim via
  // MCP), and a negative budget floors at zero instead of wrapping unsigned.
  const auto mistyped = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"id", "a"}, {"packing", 7}}));
  if (mistyped.value("ok", true) || mistyped.value("error", std::string{}).empty()) {
    return 1;
  }
  const auto negative = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"id", "a"}, {"budget", -1}}));
  const auto& negative_ctx = negative["result"];
  if (!negative.value("ok", false) || !negative_ctx["included"].empty() ||
      !negative_ctx.value("truncated", false)) {
    return 1;
  }

  // Query-based focal resolution picks the highest-centrality match ("a").
  const auto by_query = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"query", "Alpha"}, {"budget", 5000}}));
  if (by_query["result"]["focus"].value("id", std::string{}) != "a") {
    return 1;
  }
  // An unresolvable focus comes back with suggestions, not just a null focus.
  const auto context_miss = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"id", "Alpah"}}));
  if (!context_miss["result"]["focus"].is_null() || context_miss["result"]["suggestions"].empty()) {
    return 1;
  }

  // Lexical focal fallback: a natural-language query that is NOT a substring of any
  // label still resolves, via query-term overlap. "gamma" overlaps only
  // "gamma_run" ("d"); the old substring-only match would have returned focus:null.
  const auto nl_context = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"q", "compute the gamma value"}, {"budget", 5000}}));
  if (nl_context["result"]["focus"].is_null() ||
      nl_context["result"]["focus"].value("id", std::string{}) != "d") {
    return 1;
  }

  // Off-topic query (shares no lexical term with any label) stays an honest zero
  // hit — null focus, not a misleading bundle forced by the fallback.
  const auto offtopic = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"q", "xylophone zebra quux"}, {"budget", 5000}}));
  if (!offtopic["result"]["focus"].is_null()) {
    return 1;
  }

  // Multi-seed gather: "alpha gamma" is no label's substring, so the lexical
  // fallback seeds from the top matches (a/c via "alpha", d via "gamma"). "d" is
  // disconnected (no edges) and reachable ONLY as its own seed, so its presence in
  // the bundle proves the gather unions several ego graphs, not just the focal's.
  const auto multi = cgraph::handle_daemon_request(
      state, cgraph::make_request("context", {{"q", "alpha gamma"}, {"budget", 50000}}));
  if (multi["result"]["focus"].is_null()) {
    return 1;
  }
  bool multi_found_d = false;
  for (const auto& item : multi["result"]["included"]) {
    if (item.value("id", std::string{}) == "d") {
      multi_found_d = true;
    }
  }
  if (!multi_found_d) {
    return 1;
  }

  fs::remove_all(src.parent_path());

  const auto update = cgraph::handle_daemon_request(state, cgraph::make_request("update"));
  if (!update["result"]["accepted"].get<bool>()) {
    return 1;
  }

  const auto shutdown = cgraph::handle_daemon_request(state, cgraph::make_request("shutdown"));
  if (!shutdown["result"]["shutdown"].get<bool>() || !state.shutdown_requested) {
    return 1;
  }

  auto bad = cgraph::make_request("status");
  bad["protocol_version"] = 999;
  if (cgraph::handle_daemon_request(state, bad)["ok"].get<bool>()) {
    return 1;
  }

  // --- operation stats: counts, latency, zero-hit rate, uptime, window ---
  {
    cgraph::DaemonState s;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{.id = "x", .label = "Widget", .kind = "class"});
    cgraph::publish_graph_snapshot(s, std::move(g));

    for (int i = 0; i < 3; ++i) {
      cgraph::handle_daemon_request(s, cgraph::make_request("query", {{"q", "widget"}}));  // 3 hits
    }
    for (int i = 0; i < 2; ++i) {
      cgraph::handle_daemon_request(s, cgraph::make_request("query", {{"q", "zzznomatch"}}));  // 2 zero-hit
    }

    const auto st = cgraph::handle_daemon_request(s, cgraph::make_request("status"))["result"];
    const auto& ops = st["ops"];
    if (ops["query_count"] != 5 || ops["query_zero_hits"] != 2) {
      return 30;
    }
    if (std::fabs(ops["query_zero_hit_rate"].get<double>() - 0.4) > 1e-9) {
      return 31;
    }
    if (ops["lifetime"]["query"]["mean_ms"].get<double>() <= 0.0) {
      return 32;  // a served op took measurable time
    }
    if (ops["recent_window"]["size"].get<std::size_t>() > ops["recent_window"]["capacity"].get<std::size_t>()) {
      return 33;
    }
    if (st["uptime_seconds"].get<double>() <= 0.0) {
      return 34;
    }
    // Uptime advances on a later query.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto st2 = cgraph::handle_daemon_request(s, cgraph::make_request("status"))["result"];
    if (st2["uptime_seconds"].get<double>() <= st["uptime_seconds"].get<double>()) {
      return 35;
    }
  }

  // --- enrichment running is observable in-flight and clears after ---
  {
    cgraph::DaemonState s;
    cgraph::publish_graph_snapshot(s, cgraph::GraphSnapshot{});
    {
      const cgraph::EnrichmentRunningScope running(s, 3);
      const auto in_flight = cgraph::handle_daemon_request(s, cgraph::make_request("status"))["result"];
      if (in_flight["enrichment_running"] != 3 ||
          in_flight.value("enrichment_state", std::string{}) != "running") {
        return 40;
      }
    }
    const auto after = cgraph::handle_daemon_request(s, cgraph::make_request("status"))["result"];
    if (after["enrichment_running"] != 0 || after.value("enrichment_state", std::string{}) == "running") {
      return 41;  // both the count and the running state clear
    }
  }

  // --- status reports semantic connectivity ---
  {
    cgraph::DaemonState s;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{.id = "code:fn", .label = "doThing", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "doc:guide", .label = "Guide", .kind = "document"});
    g.edges.push_back(cgraph::Edge{.source = "doc:guide", .target = "code:fn", .relation = "DOCUMENTS"});
    cgraph::publish_graph_snapshot(s, std::move(g));
    const auto sem = cgraph::handle_daemon_request(s, cgraph::make_request("status"))["result"]["semantic"];
    if (sem["doc_nodes"] != 1 || sem["connected_docs"] != 1 || sem["doc_code_edges"] != 1) {
      return 50;
    }
    if (sem["connectivity_rate"].get<double>() <= 0.0) {
      return 51;
    }

    // A pure-code snapshot reports zero semantic nodes.
    cgraph::DaemonState s2;
    cgraph::GraphSnapshot g2;
    g2.nodes.push_back(cgraph::Node{.id = "code:a", .label = "a", .kind = "function"});
    cgraph::publish_graph_snapshot(s2, std::move(g2));
    const auto sem2 = cgraph::handle_daemon_request(s2, cgraph::make_request("status"))["result"]["semantic"];
    if (sem2["doc_nodes"] != 0 || sem2["concept_nodes"] != 0 ||
        sem2["connectivity_rate"].get<double>() != 0.0) {
      return 52;
    }
  }

  // --- context zero-hit + adaptive usage are observable in status ops ---
  {
    cgraph::DaemonState s;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{.id = "k", .label = "Kettle", .kind = "class"});
    cgraph::publish_graph_snapshot(s, std::move(g));

    // Resolved focus -> not a zero hit.
    cgraph::handle_daemon_request(s, cgraph::make_request("context", {{"id", "k"}, {"budget", 5000}}));
    // Unresolvable focus -> zero hit (the id matched nothing).
    cgraph::handle_daemon_request(s, cgraph::make_request("context", {{"id", "nope"}, {"budget", 5000}}));
    // Adaptive call -> counted distinctly from fixed.
    cgraph::handle_daemon_request(
        s, cgraph::make_request("context", {{"id", "k"}, {"q", "kettle"}, {"gather", "adaptive"}, {"budget", 5000}}));

    const auto ops = cgraph::handle_daemon_request(s, cgraph::make_request("status"))["result"]["ops"];
    if (ops.value("context_zero_hits", 0) != 1) {
      return 36;  // exactly the unresolved-focus call is a context zero hit
    }
    if (ops.value("context_adaptive_count", 0) != 1) {
      return 37;  // exactly the gather=adaptive call is counted
    }
  }

  // --- adaptive relevance-gated gather: the 3rd hop is taken only from a query-
  //     relevant depth-2 node; the 2-hop core is always preserved ---
  {
    cgraph::DaemonState s;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    // F -> n1 -> {rel "PaymentValidator", irr "LoggingHelper"} -> {d3a, d3b}.
    // Only "rel" matches the query "Payment", so adaptive should reach d3a (via rel)
    // but gate out d3b (via irr).
    g.nodes.push_back(cgraph::Node{.id = "F", .label = "Root", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "n1", .label = "Mid", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "rel", .label = "PaymentValidator", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "irr", .label = "LoggingHelper", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "d3a", .label = "ChargeCard", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "d3b", .label = "FormatString", .kind = "function"});
    g.edges.push_back(cgraph::Edge{.source = "F", .target = "n1", .relation = "CALLS"});
    g.edges.push_back(cgraph::Edge{.source = "n1", .target = "rel", .relation = "CALLS"});
    g.edges.push_back(cgraph::Edge{.source = "n1", .target = "irr", .relation = "CALLS"});
    g.edges.push_back(cgraph::Edge{.source = "rel", .target = "d3a", .relation = "CALLS"});
    g.edges.push_back(cgraph::Edge{.source = "irr", .target = "d3b", .relation = "CALLS"});
    cgraph::publish_graph_snapshot(s, std::move(g));

    const auto ids_of = [](const nlohmann::json& result) {
      std::set<std::string> ids;
      for (const auto& item : result["included"]) {
        ids.insert(item.value("id", std::string{}));
      }
      return ids;
    };

    // Fixed gather at depth 3 (knapsack) reaches BOTH third-hop nodes. Explicit
    // gather:"fixed" — the default is now adaptive, which would gate the 3rd hop.
    const auto fixed = cgraph::handle_daemon_request(
        s, cgraph::make_request(
               "context", {{"id", "F"}, {"q", "Payment"}, {"packing", "knapsack"}, {"max_depth", 3}, {"budget", 100000}, {"gather", "fixed"}}));
    const auto fixed_ids = ids_of(fixed["result"]);
    if (fixed_ids.count("d3a") == 0 || fixed_ids.count("d3b") == 0) {
      return 60;  // fixed 3-hop must reach both third-hop nodes
    }
    if (fixed["result"].value("gather", std::string{}) != "fixed") {
      return 61;
    }
    if (fixed["result"].contains("reach")) {
      return 66;  // reach summary is adaptive-only
    }

    // Adaptive keeps the 2-hop core (rel, irr) but expands the 3rd hop ONLY from the
    // query-relevant depth-2 node -> d3a present (via PaymentValidator), d3b absent.
    const auto adaptive = cgraph::handle_daemon_request(
        s, cgraph::make_request(
               "context", {{"id", "F"}, {"q", "Payment"}, {"gather", "adaptive"}, {"gather_theta", 0.5}, {"budget", 100000}}));
    const auto& ares = adaptive["result"];
    const auto adaptive_ids = ids_of(ares);
    if (adaptive_ids.count("rel") == 0 || adaptive_ids.count("irr") == 0) {
      return 62;  // 2-hop core preserved regardless of relevance
    }
    if (adaptive_ids.count("d3a") == 0) {
      return 63;  // relevant third hop reached
    }
    if (adaptive_ids.count("d3b") != 0) {
      return 64;  // irrelevant third hop gated out
    }
    if (ares.value("gather", std::string{}) != "adaptive") {
      return 65;
    }
    // Reach summary: the gate admitted the relevant third hop (d3a, depth 3) and
    // rejected the irrelevant depth-2 frontier node (irr).
    const auto& reach = ares["reach"];
    if (reach.value("expanded_past_core", 0) < 1) {
      return 67;  // relevant third hop was reached
    }
    if (reach.value("gated_at_core", 0) < 1) {
      return 68;  // an irrelevant frontier node was gated
    }
    if (reach.value("candidates", 0) < 1) {
      return 69;
    }

    // A query that matches no depth-2 frontier node collapses adaptive to the
    // 2-hop core: nothing expands past the core, but the core is still gathered.
    const auto none = cgraph::handle_daemon_request(
        s, cgraph::make_request(
               "context",
               {{"id", "F"}, {"q", "zzznomatch"}, {"gather", "adaptive"}, {"gather_theta", 0.5}, {"budget", 100000}}));
    const auto& nres = none["result"];
    if (nres["reach"].value("expanded_past_core", -1) != 0) {
      return 70;  // gate found nothing relevant past 2 hops -> no expansion
    }
    const auto none_ids = ids_of(nres);
    if (none_ids.count("rel") == 0 || none_ids.count("irr") == 0) {
      return 71;  // 2-hop core preserved even when nothing is relevant
    }
    if (none_ids.count("d3a") != 0 || none_ids.count("d3b") != 0) {
      return 72;  // no third hop taken
    }
  }

  // --- adaptive same-file candidates: query-ranked focal-file siblings are admitted
  //     directly, capped deterministically, and never alter fixed gathering ---
  {
    cgraph::DaemonState s;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{
        .id = "seed", .label = "CheckoutRoot", .source_file = "checkout.cpp", .kind = "function"});
    // Already graph-reachable: same-file admission must not replace its CALLS reach.
    g.nodes.push_back(cgraph::Node{
        .id = "payment_17", .label = "PaymentHelper17", .source_file = "checkout.cpp", .kind = "function",
        .properties = {{"degree_centrality", "0.17"}}});
    g.edges.push_back(cgraph::Edge{.source = "seed", .target = "payment_17", .relation = "CALLS"});
    for (int i = 0; i < 17; ++i) {
      g.nodes.push_back(cgraph::Node{
          .id = "payment_" + std::to_string(i),
          .label = "PaymentHelper" + std::to_string(i),
          .source_file = "checkout.cpp",
          .kind = "function",
          .properties = {{"degree_centrality", std::to_string(static_cast<double>(i) / 100.0)}}});
    }
    g.nodes.push_back(cgraph::Node{
        .id = "logging", .label = "LoggingHelper", .source_file = "checkout.cpp", .kind = "function"});
    g.nodes.push_back(cgraph::Node{
        .id = "empty_source", .label = "PaymentEmpty", .source_file = "", .kind = "function"});
    g.nodes.push_back(cgraph::Node{
        .id = "memory:payment", .label = "PaymentMemory", .source_file = "checkout.cpp", .kind = "checkpoint"});
    cgraph::publish_graph_snapshot(s, std::move(g));

    const auto included_by_id = [](const nlohmann::json& result) {
      std::map<std::string, nlohmann::json> items;
      for (const auto& item : result["included"]) {
        items.emplace(item.value("id", std::string{}), item);
      }
      return items;
    };

    const auto fixed = cgraph::handle_daemon_request(
        s, cgraph::make_request(
               "context", {{"id", "seed"}, {"q", "Payment"}, {"gather", "fixed"}, {"budget", 100000}}));
    const auto fixed_items = included_by_id(fixed["result"]);
    if (fixed_items.size() != 1 || fixed_items.count("payment_17") == 0) {
      return 130;  // only the persisted CALLS edge is visible in fixed mode
    }

    const auto adaptive = cgraph::handle_daemon_request(
        s, cgraph::make_request(
               "context", {{"id", "seed"}, {"q", "Payment"}, {"gather", "adaptive"},
                           {"gather_theta", 0.05}, {"budget", 100000}}));
    const auto items = included_by_id(adaptive["result"]);
    if (items.size() != 6) {
      return 131;  // one graph neighbor plus five newly admitted same-file siblings
    }
    if (items.at("payment_17").value("via", std::string{}) != "CALLS") {
      return 132;  // real-edge reach wins deduplication
    }
    for (int i = 12; i < 17; ++i) {
      const auto id = "payment_" + std::to_string(i);
      if (items.count(id) == 0 || items.at(id).value("depth", 0) != 2 ||
          items.at(id).value("via", std::string{}) != "same_file") {
        return 133;  // top five un-reached siblings, centrality descending
      }
    }
    if (items.count("payment_11") != 0 || items.count("logging") != 0 ||
        items.count("empty_source") != 0 || items.count("memory:payment") != 0) {
      return 134;  // cap, relevance, empty-source, and memory exclusions
    }
  }

  {
    cgraph::DaemonState s;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{.id = "seed", .label = "PaymentRoot", .kind = "function"});
    g.nodes.push_back(cgraph::Node{
        .id = "sibling", .label = "PaymentSibling", .source_file = "payment.cpp", .kind = "function"});
    cgraph::publish_graph_snapshot(s, std::move(g));
    const auto empty_seed_context = cgraph::handle_daemon_request(
        s, cgraph::make_request(
               "context", {{"id", "seed"}, {"q", "Payment"}, {"gather", "adaptive"}, {"budget", 100000}}));
    if (!empty_seed_context["result"]["included"].empty()) {
      return 135;  // an empty-source seed cannot open another file's siblings
    }
  }

  {
    cgraph::DaemonState s;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{
        .id = "seed", .label = "PaymentRoot", .source_file = "payment.cpp", .kind = "function"});
    g.nodes.push_back(cgraph::Node{
        .id = "logging", .label = "LoggingHelper", .source_file = "payment.cpp", .kind = "function"});
    cgraph::publish_graph_snapshot(s, std::move(g));
    const auto ranked_context = cgraph::handle_daemon_request(
        s, cgraph::make_request(
               "context", {{"id", "seed"}, {"q", "Payment"}, {"gather", "adaptive"}, {"budget", 100000}}));
    if (!ranked_context["result"]["included"].empty() ||
        ranked_context["result"]["reach"].value("candidates", 0) != 1) {
      return 136;  // zero-overlap siblings are gathered but receive no knapsack value
    }
  }

  // --- session memory: remember writes a sandboxed checkpoint; recall returns
  //     checkpoints newest-first with body + linked code briefs; memory nodes are
  //     inert to code query/context ---
  {
    const auto memdir = fs::temp_directory_path() / "cgraph-memory-test";
    fs::remove_all(memdir);

    cgraph::DaemonState s;
    s.memory_dir = memdir;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{
        .id = "fn:charge", .label = "charge_card", .kind = "function",
        .properties = {{"degree_centrality", "0.500000"}}});
    g.nodes.push_back(cgraph::Node{.id = "fn:log", .label = "log_event", .kind = "function"});
    cgraph::publish_graph_snapshot(s, std::move(g));

    // A daemon without a memory_dir rejects remember rather than writing anywhere.
    {
      cgraph::DaemonState off;
      cgraph::publish_graph_snapshot(off, cgraph::GraphSnapshot{});
      const auto r = cgraph::handle_daemon_request(
          off, cgraph::make_request("remember", {{"title", "x"}, {"body", "y"}}));
      if (r["ok"].get<bool>()) {
        return 90;
      }
    }

    // First checkpoint: one resolvable touch (charge_card) + one unresolved.
    const auto rem = cgraph::handle_daemon_request(
        s, cgraph::make_request("remember", {{"title", "Refactor charge"},
                                             {"body", "did X; next Y"},
                                             {"touches", {"charge_card", "nope_missing"}},
                                             {"tags", {"auth"}}}));
    const auto& rr = rem["result"];
    if (!rem["ok"].get<bool>() || rr.value("written", false) != true ||
        rr.value("id", std::string{}).rfind("memory:checkpoint:", 0) != 0) {
      return 91;
    }
    if (rr.value("concerns", 0) != 1 || rr["unresolved"].size() != 1 ||
        rr["unresolved"][0].get<std::string>() != "nope_missing") {
      return 92;
    }
    // Body file landed inside the sandbox dir; the node points at it.
    const auto body_path = fs::path(rr.value("source_file", std::string{}));
    if (body_path.parent_path() != memdir || !fs::exists(body_path)) {
      return 93;
    }
    // The checkpoint node + a concerns edge to the resolved node ONLY are in the graph.
    {
      const auto snap = cgraph::read_graph_snapshot(s);
      bool has_checkpoint = false;
      bool concerns_resolved = false;
      bool concerns_unresolved = false;
      for (const auto& node : snap->nodes) {
        if (node.id == rr["id"].get<std::string>()) {
          has_checkpoint = node.kind == "checkpoint";
        }
      }
      for (const auto& edge : snap->edges) {
        if (edge.relation == "concerns" && edge.target == "fn:charge") {
          concerns_resolved = true;
        }
        if (edge.relation == "concerns" && edge.target == "nope_missing") {
          concerns_unresolved = true;
        }
      }
      if (!has_checkpoint || !concerns_resolved || concerns_unresolved) {
        return 94;
      }
    }

    // An oversized body is rejected with no node added.
    const auto count_before = cgraph::read_graph_snapshot(s)->nodes.size();
    const auto oversize = cgraph::handle_daemon_request(
        s, cgraph::make_request("remember", {{"title", "big"}, {"body", std::string(20000, 'x')}}));
    if (oversize["ok"].get<bool>() || cgraph::read_graph_snapshot(s)->nodes.size() != count_before) {
      return 95;
    }

    // Second checkpoint (later timestamp) so recall ordering is observable.
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    cgraph::handle_daemon_request(
        s, cgraph::make_request("remember", {{"title", "Second task"}, {"body", "later work"}}));

    // Recall: newest-first, body snippet readable, linked code brief present.
    const auto rec = cgraph::handle_daemon_request(s, cgraph::make_request("recall", {}));
    const auto& cps = rec["result"]["checkpoints"];
    if (cps.size() != 2 || cps[0].value("label", std::string{}) != "Second task" ||
        cps[1].value("label", std::string{}) != "Refactor charge") {
      return 96;  // newest-first
    }
    if (cps[1].value("snippet", std::string{}).find("did X") == std::string::npos) {
      return 97;  // body is snippet-readable through source_file
    }
    if (cps[1]["concerns"].size() != 1 ||
        cps[1]["concerns"][0].value("label", std::string{}) != "charge_card") {
      return 97;  // linked code brief
    }
    // query filter over title narrows recall.
    const auto rec_q = cgraph::handle_daemon_request(s, cgraph::make_request("recall", {{"query", "second"}}));
    if (rec_q["result"]["checkpoints"].size() != 1) {
      return 98;
    }
    // query filter reaches body content: "next Y" appears only in the first
    // checkpoint's body, never in a title or tag; a term found nowhere matches nothing.
    const auto rec_body = cgraph::handle_daemon_request(s, cgraph::make_request("recall", {{"query", "next y"}}));
    if (rec_body["result"]["checkpoints"].size() != 1 ||
        rec_body["result"]["checkpoints"][0].value("label", std::string{}) != "Refactor charge") {
      return 120;
    }
    const auto rec_none = cgraph::handle_daemon_request(s, cgraph::make_request("recall", {{"query", "zzqx"}}));
    if (!rec_none["result"]["checkpoints"].empty()) {
      return 121;
    }

    // Memory is inert to code retrieval: a checkpoint title is not a code query
    // match, and a checkpoint is never packed into code context.
    const auto code_q = cgraph::handle_daemon_request(s, cgraph::make_request("query", {{"q", "Refactor"}}));
    if (code_q["result"].value("total", 1U) != 0U) {
      return 99;
    }
    const auto ctx = cgraph::handle_daemon_request(
        s, cgraph::make_request("context", {{"id", "charge_card"}, {"budget", 5000}}));
    for (const auto& item : ctx["result"]["included"]) {
      if (cgraph::is_memory_node_id(item.value("id", std::string{}))) {
        return 99;
      }
    }

    fs::remove_all(memdir);
  }

  // --- session-memory overlay: the on-disk sidecar is the durable source of
  //     truth; it survives a snapshot rebuild via re-overlay, is idempotent,
  //     graph.json omits memory, and recall tolerates a dangling concerns edge ---
  {
    const auto memdir = fs::temp_directory_path() / "cgraph-memory-overlay-test";
    fs::remove_all(memdir);

    cgraph::DaemonState s;
    s.memory_dir = memdir;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{.id = "fn:charge", .label = "charge_card", .kind = "function"});
    cgraph::publish_graph_snapshot(s, std::move(g));

    const auto rem = cgraph::handle_daemon_request(
        s, cgraph::make_request("remember",
                                {{"title", "Refund"}, {"body", "wip refund"}, {"touches", {"charge_card"}}}));
    const auto cp_id = rem["result"].value("id", std::string{});
    const auto body_path = fs::path(rem["result"].value("source_file", std::string{}));
    const auto sidecar = fs::path(body_path).replace_extension(".json");

    // Sidecar exists beside the body and parses into a fragment with the
    // checkpoint node + its concerns edge.
    if (!fs::exists(sidecar)) {
      return 100;
    }
    {
      std::ifstream in(sidecar);
      const auto parsed = nlohmann::json::parse(in, nullptr, false);
      cgraph::Fragment frag;
      std::vector<std::string> errs;
      if (parsed.is_discarded() || !cgraph::parse_fragment(parsed, frag, errs)) {
        return 101;
      }
      bool has_node = false;
      bool has_edge = false;
      for (const auto& node : frag.nodes) {
        if (node.id == cp_id) {
          has_node = true;
        }
      }
      for (const auto& edge : frag.edges) {
        if (edge.relation == "concerns" && edge.target == "fn:charge") {
          has_edge = true;
        }
      }
      if (!has_node || !has_edge) {
        return 101;
      }
    }

    // The daemon-persisted graph.json omits memory nodes (sidecar is sole truth).
    const auto gpath = memdir / "graph.json";
    if (!cgraph::persist_graph_snapshot(s, gpath)) {
      return 102;
    }
    {
      std::ifstream in(gpath);
      const auto j = nlohmann::json::parse(in, nullptr, false);
      for (const auto& node : j["nodes"]) {
        if (cgraph::is_memory_node_id(node.value("id", std::string{}))) {
          return 102;
        }
      }
    }

    // Re-overlay from the sidecar = the ingest_all_memory mechanism.
    const auto overlay = [&]() {
      auto validation = cgraph::validate_semantic_fragment_file(sidecar);
      if (!validation.valid) {
        return false;
      }
      cgraph::mutate_graph_snapshot(
          s, [&](cgraph::GraphSnapshot& graph) { cgraph::merge_fragment(graph, validation.fragment); });
      return true;
    };

    // Simulate a rebuild: publish a fresh extraction-only snapshot (no memory).
    {
      cgraph::GraphSnapshot rebuilt;
      rebuilt.build_state = cgraph::BuildState::DeterministicReady;
      rebuilt.nodes.push_back(cgraph::Node{.id = "fn:charge", .label = "charge_card", .kind = "function"});
      cgraph::publish_graph_snapshot(s, std::move(rebuilt));
    }
    if (cgraph::handle_daemon_request(s, cgraph::make_request("recall", {}))["result"].value("total", 1) != 0) {
      return 103;  // the rebuild dropped the checkpoint
    }
    if (!overlay()) {
      return 104;
    }
    const auto rec = cgraph::handle_daemon_request(s, cgraph::make_request("recall", {}))["result"];
    if (rec.value("total", 0) != 1 || rec["checkpoints"][0].value("label", std::string{}) != "Refund" ||
        rec["checkpoints"][0]["concerns"].size() != 1) {
      return 104;  // re-overlay restores the checkpoint and its resolvable link
    }

    // Idempotent: a second overlay duplicates nothing.
    const auto before = cgraph::read_graph_snapshot(s);
    const auto nodes_before = before->nodes.size();
    const auto edges_before = before->edges.size();
    overlay();
    const auto after = cgraph::read_graph_snapshot(s);
    if (after->nodes.size() != nodes_before || after->edges.size() != edges_before) {
      return 105;
    }

    // Dangling concerns: rebuild WITHOUT the code target, re-overlay, recall skips
    // the missing link without erroring.
    {
      cgraph::GraphSnapshot no_target;
      no_target.build_state = cgraph::BuildState::DeterministicReady;  // charge_card gone
      cgraph::publish_graph_snapshot(s, std::move(no_target));
    }
    overlay();
    const auto rec2 = cgraph::handle_daemon_request(s, cgraph::make_request("recall", {}))["result"];
    if (rec2.value("total", 0) != 1 || rec2["checkpoints"][0]["concerns"].size() != 0) {
      return 106;
    }

    fs::remove_all(memdir);
  }

  // --- session-memory observability: status reports the memory inventory, and
  //     recall zero-hits are counted without touching query/context counters ---
  {
    const auto memdir = fs::temp_directory_path() / "cgraph-memory-obs-test";
    fs::remove_all(memdir);
    cgraph::DaemonState s;
    s.memory_dir = memdir;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{.id = "fn:a", .label = "alpha", .kind = "function"});
    cgraph::publish_graph_snapshot(s, std::move(g));

    cgraph::handle_daemon_request(s, cgraph::make_request("remember", {{"title", "one"}, {"body", "b"}}));
    cgraph::handle_daemon_request(s, cgraph::make_request("recall", {}));                  // hit (1 checkpoint)
    cgraph::handle_daemon_request(s, cgraph::make_request("recall", {{"query", "zzz"}}));  // miss

    const auto st = cgraph::handle_daemon_request(s, cgraph::make_request("status"))["result"];
    const auto& mem = st["memory"];
    if (mem.value("checkpoint_count", 0) != 1 || mem.value("sidecar_count", 0) != 1) {
      return 110;
    }
    if (mem.value("recall_count", 0) != 2 || mem.value("recall_zero_hits", 0) != 1) {
      return 111;
    }
    if (mem.value("last_remember_at", std::string{}).empty() ||
        mem.value("last_recall_at", std::string{}).empty()) {
      return 112;
    }
    // recall zero-hits do not bleed into query/context counters.
    const auto& ops = st["ops"];
    if (ops.value("query_zero_hits", 1) != 0 || ops.value("context_zero_hits", 1) != 0 ||
        ops.value("recall_zero_hits", 0) != 1) {
      return 113;
    }
    // A second checkpoint bumps the inventory.
    cgraph::handle_daemon_request(s, cgraph::make_request("remember", {{"title", "two"}, {"body", "b2"}}));
    const auto mem2 = cgraph::handle_daemon_request(s, cgraph::make_request("status"))["result"]["memory"];
    if (mem2.value("checkpoint_count", 0) != 2) {
      return 114;
    }
    fs::remove_all(memdir);
  }

  // --- typed explain: a relation filter narrows neighbors to one edge type and
  //     composes with direction; an absent relation returns the full set ---
  {
    cgraph::DaemonState s;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    // Hub "h" is reached by four distinct relation types so a relation filter has
    // something to exclude: an incoming and an outgoing CALLS, an incoming
    // reference, and an incoming import.
    g.nodes.push_back(cgraph::Node{.id = "h", .label = "Hub", .kind = "class"});
    g.nodes.push_back(cgraph::Node{.id = "caller", .label = "Caller", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "callee", .label = "Callee", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "refer", .label = "Referer", .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "mod", .label = "Importer", .kind = "module"});
    g.edges.push_back(cgraph::Edge{.source = "caller", .target = "h", .relation = "CALLS"});
    g.edges.push_back(cgraph::Edge{.source = "h", .target = "callee", .relation = "CALLS"});
    g.edges.push_back(cgraph::Edge{.source = "refer", .target = "h", .relation = "references"});
    g.edges.push_back(cgraph::Edge{.source = "mod", .target = "h", .relation = "imports"});
    cgraph::publish_graph_snapshot(s, std::move(g));

    const auto rel_count = [](const nlohmann::json& result, const std::string& want) {
      std::size_t n = 0;
      for (const auto& neighbor : result["neighbors"]) {
        if (neighbor.value("relation", std::string{}) == want) {
          ++n;
        }
      }
      return n;
    };

    // No relation -> every adjacent edge (the parity case): four mixed-relation
    // neighbors, including the lone reference edge.
    const auto all = cgraph::handle_daemon_request(s, cgraph::make_request("explain", {{"id", "h"}}));
    if (all["result"].value("neighbor_count", 0U) != 4U || rel_count(all["result"], "references") != 1) {
      return 80;
    }

    // relation=CALLS -> only the two CALLS edges; the reference and import drop,
    // and no off-relation edge leaks through.
    const auto calls = cgraph::handle_daemon_request(
        s, cgraph::make_request("explain", {{"id", "h"}, {"relation", "CALLS"}}));
    const auto& calls_result = calls["result"];
    if (calls_result["neighbors"].size() != 2 || calls_result.value("neighbor_count", 0U) != 2U) {
      return 81;
    }
    if (rel_count(calls_result, "CALLS") != 2 || rel_count(calls_result, "references") != 0 ||
        rel_count(calls_result, "imports") != 0) {
      return 82;
    }

    // relation + direction compose: incoming CALLS only -> just the caller.
    const auto in_calls = cgraph::handle_daemon_request(
        s, cgraph::make_request("explain", {{"id", "h"}, {"direction", "in"}, {"relation", "CALLS"}}));
    if (in_calls["result"]["neighbors"].size() != 1 ||
        in_calls["result"]["neighbors"][0].value("source", std::string{}) != "caller" ||
        in_calls["result"]["neighbors"][0].value("direction", std::string{}) != "in") {
      return 83;
    }

    // A single non-CALLS relation isolates that edge type.
    const auto refs = cgraph::handle_daemon_request(
        s, cgraph::make_request("explain", {{"id", "h"}, {"relation", "references"}}));
    if (refs["result"]["neighbors"].size() != 1 ||
        refs["result"]["neighbors"][0].value("relation", std::string{}) != "references") {
      return 84;
    }

    // A relation no adjacent edge carries -> a found node with an empty neighbor
    // list, not an error or a not-found result.
    const auto nomatch = cgraph::handle_daemon_request(
        s, cgraph::make_request("explain", {{"id", "h"}, {"relation", "inherits"}}));
    if (nomatch["result"].value("found", true) == false || !nomatch["result"]["neighbors"].empty() ||
        nomatch["result"].value("neighbor_count", 1U) != 0U) {
      return 85;
    }
  }

  // status() reads the enrichment counters and the `unextracted` map while the
  // enrichment/update path writes them from another thread. Both sides now take
  // state.enrichment_mutex, so status must return well-formed, in-range counters
  // and never crash iterating the map mid-mutation. A single-run test cannot
  // PROVE absence of a race, but this exercises the shared lock under real
  // concurrency and is the case TSan flags if the synchronization regresses
  // (run under the sanitizers preset).
  {
    cgraph::DaemonState s;
    cgraph::publish_graph_snapshot(s, cgraph::GraphSnapshot{});

    std::atomic<bool> stop{false};
    std::thread writer([&] {
      for (std::size_t iteration = 0; !stop.load(); ++iteration) {
        // Mirror the real writers: counters + map, all under enrichment_mutex.
        {
          const std::scoped_lock lock(s.enrichment_mutex);
          s.enrichment_pending = iteration % 7;
          s.enrichment_stale = iteration % 3;
          s.enrichment_failed = iteration % 2;
          s.enrichment_plans_run = iteration;
          if (iteration % 2 == 0) {
            s.unextracted["toml"] = iteration % 5;
          } else {
            s.unextracted.erase("toml");
          }
        }
        // The in-flight scope flips running/state under the same lock.
        { const cgraph::EnrichmentRunningScope running(s, iteration % 4); }
      }
    });

    for (int poll = 0; poll < 5000; ++poll) {
      const auto concurrent_status = cgraph::handle_daemon_request(s, cgraph::make_request("status"));
      if (!concurrent_status.value("ok", false)) {
        stop.store(true);
        writer.join();
        return 86;
      }
      const auto& result = concurrent_status["result"];
      // Every counter must be a readable unsigned in its writer's range -- a torn
      // read would surface as a wild value or a type error here.
      if (!result["enrichment_pending"].is_number_unsigned() || result["enrichment_pending"].get<std::size_t>() > 6 ||
          !result["enrichment_stale"].is_number_unsigned() || result["enrichment_stale"].get<std::size_t>() > 2 ||
          !result["enrichment_failed"].is_number_unsigned() || result["enrichment_failed"].get<std::size_t>() > 1 ||
          !result["enrichment_running"].is_number_unsigned() || result["enrichment_running"].get<std::size_t>() > 3 ||
          !result["unextracted"].is_object()) {
        stop.store(true);
        writer.join();
        return 87;
      }
    }
    stop.store(true);
    writer.join();
  }

  // Enrichment concepts must not out-rank structural code in search (issue #16):
  // a `concept:` node whose prose mentions a symbol is host-authored text, not a
  // code symbol, so a code search surfaces structural results first while the
  // concept still appears (coexist, not eclipse) and `total` reflects all.
  {
    cgraph::DaemonState rank_state;
    cgraph::GraphSnapshot g;
    g.build_state = cgraph::BuildState::DeterministicReady;
    g.nodes.push_back(cgraph::Node{.id = "f_resolve", .label = "resolveRepoId", .source_file = "a/mem.ts",
                                   .source_location = cgraph::SourceLocation{.start_line = 1}, .kind = "function"});
    g.nodes.push_back(cgraph::Node{.id = "f_resolve_cached", .label = "resolveRepoIdCached", .source_file = "a/cache.ts",
                                   .source_location = cgraph::SourceLocation{.start_line = 2}, .kind = "function"});
    // Label chosen to sort BEFORE the function labels alphabetically (leading
    // "Economics" < "resolveRepoId") so that without the structural-first
    // partition the concept would rank first at equal centrality -- the exact
    // eclipse-adjacent ordering issue #16 reports.
    g.nodes.push_back(cgraph::Node{.id = "concept:known-selector",
                                   .label = "Economics-tab workspace selector unions repoIds (resolveRepoId-deduped)",
                                   .kind = "concept"});
    cgraph::publish_graph_snapshot(rank_state, std::move(g));

    // "resolveRepo" is a substring of two functions AND the concept, and is an
    // exact match for none, so it takes the search route.
    const auto res = cgraph::handle_daemon_request(rank_state, cgraph::make_request("query", {{"q", "resolveRepo"}}))["result"];
    if (res.value("route", std::string{}) != "search") {
      return 90;
    }
    if (res.value("total", 0) != 3) {  // all three coexist; the concept is not dropped
      return 91;
    }
    const auto& nodes = res["nodes"];
    if (nodes.size() != 3) {
      return 92;
    }
    // Structural nodes first, the concept last.
    if (nodes[0].value("kind", std::string{}) != "function" ||
        nodes[1].value("kind", std::string{}) != "function" ||
        nodes[2].value("kind", std::string{}) != "concept") {
      return 93;
    }
    // The exact symbol still routes to entity and returns the structural node.
    const auto exact = cgraph::handle_daemon_request(rank_state, cgraph::make_request("query", {{"q", "resolveRepoId"}}))["result"];
    if (exact.value("route", std::string{}) != "entity" ||
        exact["nodes"][0].value("kind", std::string{}) != "function") {
      return 94;
    }
  }

  return 0;
}
