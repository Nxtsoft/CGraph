#include "cgraph/mcp_server.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>

int main() {
  nlohmann::json forwarded;
  cgraph::McpForwarder forwarder = [&](const nlohmann::json& request) {
    forwarded = request;
    return nlohmann::json{{"ok", true}, {"result", {{"nodes", {{{"id", "a"}, {"label", "Alpha"}}}}}}};
  };

  const auto initialized = cgraph::handle_mcp_request(
      nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", nlohmann::json::object()}},
      forwarder);
  if (initialized["jsonrpc"] != "2.0" || initialized["id"] != 1 || !initialized["result"].contains("capabilities")) {
    return 1;
  }

  const auto listed = cgraph::handle_mcp_request(
      nlohmann::json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", nlohmann::json::object()}},
      forwarder);
  if (listed["result"]["tools"].empty() || listed["result"]["tools"][0]["name"] != "graph_query") {
    return 1;
  }

  // The graph_context tool advertises the adaptive gather mode so an agent can find it.
  bool context_doc_mentions_adaptive = false;
  for (const auto& tool : listed["result"]["tools"]) {
    if (tool.value("name", std::string{}) == "graph_context") {
      context_doc_mentions_adaptive =
          tool.value("description", std::string{}).find("adaptive") != std::string::npos;
    }
  }
  if (!context_doc_mentions_adaptive) {
    return 1;
  }

  // graph_explain advertises the relation filter and the named typed-traversal
  // patterns (callers/callees/references) so an agent can ask one structural
  // question instead of post-filtering a mixed neighbor dump.
  bool explain_doc_mentions_typed = false;
  for (const auto& tool : listed["result"]["tools"]) {
    if (tool.value("name", std::string{}) == "graph_explain") {
      const auto desc = tool.value("description", std::string{});
      const bool has_relation_param = tool["inputSchema"]["properties"].contains("relation");
      explain_doc_mentions_typed = has_relation_param && desc.find("caller") != std::string::npos &&
                                   desc.find("callee") != std::string::npos &&
                                   desc.find("reference") != std::string::npos;
    }
  }
  if (!explain_doc_mentions_typed) {
    return 1;
  }

  // Arguments forward verbatim so optional params (kind, file, limit) reach the
  // daemon instead of being silently dropped.
  const auto called = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 3},
          {"method", "tools/call"},
          {"params", {{"name", "graph_query"}, {"arguments", {{"query", "Alpha"}, {"kind", "class"}, {"limit", 5}}}}}},
      forwarder);
  if (forwarded["op"] != "query" || forwarded["params"]["query"] != "Alpha" ||
      forwarded["params"]["kind"] != "class" || forwarded["params"]["limit"] != 5 ||
      called["result"]["content"][0]["type"] != "text" ||
      called["result"]["content"][0]["text"].get<std::string>().find("Alpha") == std::string::npos) {
    return 1;
  }

  const auto explained = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 5},
          {"method", "tools/call"},
          {"params", {{"name", "graph_explain"}, {"arguments", {{"id", "a"}, {"direction", "in"}, {"limit", 3}}}}}},
      forwarder);
  if (explained.contains("error") || forwarded["op"] != "explain" || forwarded["params"]["id"] != "a" ||
      forwarded["params"]["direction"] != "in" || forwarded["params"]["limit"] != 3) {
    return 1;
  }

  // graph_explain forwards the relation filter verbatim to the explain op, so an
  // agent can ask "who calls this" (relation=CALLS) and reach typed traversal.
  const auto explain_typed = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 7},
          {"method", "tools/call"},
          {"params",
           {{"name", "graph_explain"}, {"arguments", {{"id", "a"}, {"direction", "in"}, {"relation", "CALLS"}}}}}},
      forwarder);
  if (explain_typed.contains("error") || forwarded["op"] != "explain" ||
      forwarded["params"]["relation"] != "CALLS") {
    return 1;
  }

  // graph_context forwards the adaptive gather params verbatim to the context op,
  // so an agent can actually reach the adaptive mode through MCP.
  const auto ctx_called = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 6},
          {"method", "tools/call"},
          {"params",
           {{"name", "graph_context"},
            {"arguments", {{"id", "a"}, {"q", "alpha"}, {"gather", "adaptive"}, {"gather_theta", 0.1}}}}}},
      forwarder);
  if (ctx_called.contains("error") || forwarded["op"] != "context" ||
      forwarded["params"]["gather"] != "adaptive" ||
      std::fabs(forwarded["params"]["gather_theta"].get<double>() - 0.1) > 1e-9) {
    return 1;
  }

  // graph_remember / graph_recall forward to the session-memory ops so an agent
  // can checkpoint before /clear and recall after it.
  const auto remembered = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 8},
          {"method", "tools/call"},
          {"params",
           {{"name", "graph_remember"},
            {"arguments", {{"title", "T"}, {"body", "B"}, {"touches", {"a"}}}}}}},
      forwarder);
  if (remembered.contains("error") || forwarded["op"] != "remember" ||
      forwarded["params"]["title"] != "T" || forwarded["params"]["touches"][0] != "a") {
    return 1;
  }
  const auto recalled = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 9},
          {"method", "tools/call"},
          {"params", {{"name", "graph_recall"}, {"arguments", {{"limit", 3}}}}}},
      forwarder);
  if (recalled.contains("error") || forwarded["op"] != "recall" || forwarded["params"]["limit"] != 3) {
    return 1;
  }

  const auto unknown = cgraph::handle_mcp_request(
      nlohmann::json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", {{"name", "missing"}}}},
      forwarder);
  if (!unknown.contains("error") || unknown["error"]["code"] != -32602) {
    return 1;
  }

  // 4.1: graph_update schema describes content-verified synchronization and advertises content_root.
  bool update_doc_verified = false;
  bool update_doc_content_root = false;
  bool update_doc_work_counts = false;
  for (const auto& tool : listed["result"]["tools"]) {
    if (tool.value("name", std::string{}) == "graph_update") {
      const auto desc = tool.value("description", std::string{});
      update_doc_verified = desc.find("verified") != std::string::npos;
      update_doc_content_root = desc.find("content_root") != std::string::npos;
      update_doc_work_counts = desc.find("files_hashed") != std::string::npos &&
                               desc.find("bytes_hashed") != std::string::npos;
    }
  }
  if (!update_doc_verified || !update_doc_content_root || !update_doc_work_counts) {
    return 1;
  }

  // 4.1: All five graph read tools advertise expected_content_root in their input schema.
  const std::vector<std::string> read_tools = {
      "graph_query", "graph_path", "graph_explain", "graph_impact", "graph_context"};
  std::size_t read_tools_with_root = 0;
  for (const auto& tool : listed["result"]["tools"]) {
    const auto tname = tool.value("name", std::string{});
    for (const auto& rt : read_tools) {
      if (tname == rt) {
        if (!tool["inputSchema"]["properties"].contains("expected_content_root")) {
          return 1;
        }
        ++read_tools_with_root;
      }
    }
  }
  if (read_tools_with_root != read_tools.size()) {
    return 1;
  }

  // 4.1: expected_content_root forwards verbatim to each daemon read op.
  const auto q_with_root = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 10},
          {"method", "tools/call"},
          {"params",
           {{"name", "graph_query"},
            {"arguments", {{"query", "Foo"}, {"expected_content_root", "abc123"}}}}}},
      forwarder);
  if (q_with_root.contains("error") || forwarded["op"] != "query" ||
      forwarded["params"]["expected_content_root"] != "abc123") {
    return 1;
  }

  const auto path_with_root = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 11},
          {"method", "tools/call"},
          {"params",
           {{"name", "graph_path"},
            {"arguments", {{"source", "a"}, {"target", "b"}, {"expected_content_root", "abc123"}}}}}},
      forwarder);
  if (path_with_root.contains("error") || forwarded["op"] != "path" ||
      forwarded["params"]["expected_content_root"] != "abc123") {
    return 1;
  }

  const auto explain_with_root = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 12},
          {"method", "tools/call"},
          {"params",
           {{"name", "graph_explain"},
            {"arguments", {{"id", "a"}, {"expected_content_root", "abc123"}}}}}},
      forwarder);
  if (explain_with_root.contains("error") || forwarded["op"] != "explain" ||
      forwarded["params"]["expected_content_root"] != "abc123") {
    return 1;
  }

  const auto impact_with_root = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 13},
          {"method", "tools/call"},
          {"params",
           {{"name", "graph_impact"},
            {"arguments", {{"id", "a"}, {"expected_content_root", "abc123"}}}}}},
      forwarder);
  if (impact_with_root.contains("error") || forwarded["op"] != "impact" ||
      forwarded["params"]["expected_content_root"] != "abc123") {
    return 1;
  }

  const auto ctx_with_root = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 14},
          {"method", "tools/call"},
          {"params",
           {{"name", "graph_context"},
            {"arguments", {{"id", "a"}, {"expected_content_root", "abc123"}}}}}},
      forwarder);
  if (ctx_with_root.contains("error") || forwarded["op"] != "context" ||
      forwarded["params"]["expected_content_root"] != "abc123") {
    return 1;
  }

  // A forwarder that throws (a filesystem error reaching the daemon) fails that
  // one request with a JSON-RPC error naming the path; it must not escape and
  // abort the stdio loop that serves every later request.
  const cgraph::McpForwarder throwing_forwarder = [](const nlohmann::json&) -> nlohmann::json {
    throw std::filesystem::filesystem_error("cannot create directories", std::filesystem::path("/ro/daemon-dir"),
                                            std::make_error_code(std::errc::permission_denied));
  };
  const auto thrown = cgraph::handle_mcp_request(
      nlohmann::json{{"jsonrpc", "2.0"},
                     {"id", 15},
                     {"method", "tools/call"},
                     {"params", {{"name", "graph_query"}, {"arguments", {{"q", "alpha"}}}}}},
      throwing_forwarder);
  if (thrown["id"] != 15 || thrown["error"]["code"] != -32603 ||
      thrown["error"]["message"].get<std::string>().find("/ro/daemon-dir") == std::string::npos) {
    return 1;
  }

  return 0;
}
