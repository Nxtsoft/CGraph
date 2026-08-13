#include "cgraph/mcp_server.hpp"

#include "cgraph/engine.hpp"
#include "cgraph/protocol.hpp"

#include <string>
#include <utility>

namespace cgraph {
namespace {

[[nodiscard]] nlohmann::json response(const nlohmann::json& id, nlohmann::json result) {
  return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

[[nodiscard]] nlohmann::json error_response(const nlohmann::json& id, int code, std::string message) {
  return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", std::move(message)}}}};
}

[[nodiscard]] nlohmann::json text_content(const nlohmann::json& value) {
  return nlohmann::json{{"content", {{{"type", "text"}, {"text", value.dump()}}}}};
}

[[nodiscard]] nlohmann::json tool_schema(std::string name, std::string description, nlohmann::json properties) {
  return nlohmann::json{
      {"name", std::move(name)},
      {"description", std::move(description)},
      {"inputSchema", {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", true}}},
  };
}

[[nodiscard]] nlohmann::json string_param(std::string description) {
  return nlohmann::json{{"type", "string"}, {"description", std::move(description)}};
}

[[nodiscard]] nlohmann::json integer_param(std::string description) {
  return nlohmann::json{{"type", "integer"}, {"description", std::move(description)}};
}

[[nodiscard]] nlohmann::json tools() {
  return nlohmann::json::array({
      tool_schema(
          "graph_query",
          "Find symbols by name AND answer structural questions in one call -- it routes by intent "
          "and reports which route ran in a `route` field. Ask a structural question directly in "
          "plain words: \"who calls X\" / \"callers of X\", \"what does X call\" / \"callees of X\", "
          "\"references to X\" / \"uses of X\", \"implementations of X\" / \"subclasses of X\", "
          "\"who imports X\" -- and it returns the typed neighbors (no need to graph_query then "
          "graph_explain). A unique exact symbol name returns that node plus a callers/callees/"
          "references summary (route \"entity\"). Anything else is a case-insensitive name search "
          "ranked by importance (route \"search\"), with did-you-mean suggestions when nothing "
          "matches. Prefer this over grep to locate a definition; feed a returned id into "
          "graph_explain / graph_impact / graph_context for more depth.",
          {{"query", string_param("substring of the symbol name or id to find")},
           {"kind", string_param("only nodes of this kind, e.g. function, class, file")},
           {"file", string_param("only nodes whose source file path contains this substring")},
           {"limit", integer_param("max results (default 50)")},
           {"expected_content_root",
            string_param("content root returned by graph_update; if supplied, the response comes "
                         "only from the matching snapshot and errors if the daemon has published "
                         "a different root")}}),
      tool_schema(
          "graph_path",
          "Shortest connection between two symbols (how A relates to B through calls, imports, "
          "containment). Accepts node ids or exact symbol names.",
          {{"source", string_param("node id or exact symbol name to start from")},
           {"target", string_param("node id or exact symbol name to reach")},
           {"expected_content_root",
            string_param("content root returned by graph_update; if supplied, the response comes "
                         "only from the matching snapshot and errors if the daemon has published "
                         "a different root")}}),
      tool_schema(
          "graph_explain",
          "One symbol in depth: its source snippet plus its neighbor edges (most important first). "
          "Accepts a node id or exact symbol name. Use after graph_query to understand how a symbol "
          "is used. Pass relation to ask one typed structural question instead of post-filtering a "
          "mixed neighbor dump: find callers (direction:in, relation:CALLS), find callees "
          "(direction:out, relation:CALLS), find references (relation:references), trace imports "
          "(relation:imports), inspect inheritance/implementation (relation:inherits).",
          {{"id", string_param("node id or exact symbol name")},
           {"direction", {{"type", "string"}, {"enum", {"in", "out", "both"}},
                          {"description", "in = callers/importers only, out = callees/imports only (default both)"}}},
           {"relation", string_param("only edges of this relation (exact, case-sensitive), e.g. CALLS, "
                                     "references, imports, inherits, contains")},
           {"limit", integer_param("max neighbor edges returned (default 100)")},
           {"expected_content_root",
            string_param("content root returned by graph_update; if supplied, the response comes "
                         "only from the matching snapshot and errors if the daemon has published "
                         "a different root")}}),
      tool_schema(
          "graph_impact",
          "Transitive blast radius of a symbol: dependents (callers/importers that break if it "
          "changes), dependencies (what it relies on), or both, bounded by max_depth. Use before "
          "modifying or deleting a symbol to see what is affected.",
          {{"id", string_param("node id or exact symbol name")},
           {"direction", {{"type", "string"}, {"enum", {"dependents", "dependencies", "both"}},
                          {"description", "default dependents"}}},
           {"relation", string_param("only follow edges of this relation, e.g. CALLS, IMPORTS")},
           {"max_depth", integer_param("hops to traverse (default 3)")},
           {"limit", integer_param("max nodes returned (default 200)")},
           {"expected_content_root",
            string_param("content root returned by graph_update; if supplied, the response comes "
                         "only from the matching snapshot and errors if the daemon has published "
                         "a different root")}}),
      tool_schema(
          "graph_context",
          "Token-budgeted context bundle for a symbol: the focal node plus its most relevant "
          "neighbors with source snippets, packed to fit a token budget. The fastest way to load "
          "just enough code to edit or review a symbol. Two gather modes: adaptive (the default) "
          "keeps the 2-hop core but expands the third hop only along query-relevant nodes, "
          "reaching more of what you asked about at a fraction of the full 3-hop token cost -- "
          "pass a query/q describing the task for the relevance gate to work with. fixed pulls "
          "the plain k-hop neighborhood instead. The response reports which gather/packing mode "
          "ran, and for adaptive a reach summary (candidates, expanded_past_core, gated_at_core).",
          {{"id", string_param("node id or exact symbol name to focus on")},
           {"query", string_param("free-text fallback when the id is unknown; the best match becomes the focus")},
           {"budget", integer_param("token budget for the bundle (default 6000)")},
           {"max_depth", integer_param("neighborhood radius in hops (default 3 adaptive/knapsack, 2 fixed+greedy)")},
           {"gather", {{"type", "string"}, {"enum", {"fixed", "adaptive"}},
                       {"description", "adaptive = keep the 2-hop core but expand the 3rd hop only along "
                                       "query-relevant nodes (default; give it a query/q); fixed = plain "
                                       "k-hop neighborhood"}}},
           {"gather_theta", {{"type", "number"},
                             {"description", "adaptive gate: min query-term overlap to expand past 2 hops (default 0.05)"}}},
           {"expected_content_root",
            string_param("content root returned by graph_update; if supplied, the response comes "
                         "only from the matching snapshot and errors if the daemon has published "
                         "a different root")}}),
      tool_schema(
          "graph_update",
          "Blocking content-verified synchronization barrier: reads and hashes every detected "
          "code file, re-extracts any file whose content changed, and publishes a new snapshot. "
          "Returns freshness.content_root plus files_hashed and bytes_hashed verification counts. "
          "The root uniquely identifies the verified source snapshot. Pass it as "
          "expected_content_root to graph_query / graph_path / "
          "graph_explain / graph_impact / graph_context to pin those reads to the same verified "
          "snapshot; a mismatch fails closed without returning graph data. Use before any "
          "freshness-sensitive navigation or after editing source files. The daemon also watches "
          "the tree and folds edits in automatically within seconds, but only graph_update "
          "provides a cryptographically verified content_root.",
          {{"path", string_param("project-relative path hint; \".\" rescans the whole project")}}),
      tool_schema(
          "graph_status",
          "Daemon health: node/edge counts, build_state (\"building\" means the initial graph build "
          "is still running and query results may be empty), whether live file watching is active, "
          "and semantic enrichment progress.",
          nlohmann::json::object()),
      tool_schema(
          "graph_remember",
          "Checkpoint task state so you can /compact or /clear a long session without losing the "
          "thread. cgraph is external to your context, so a checkpoint survives /clear. Write a "
          "distilled summary (what you did, what's next) -- never raw tool output, browser DOMs, or "
          "chain-of-thought. List the code symbols it touches so recall can link back to them. "
          "Call this before /compact or /clear and after a long exploration.",
          {{"title", string_param("short title for the checkpoint")},
           {"body", string_param("distilled markdown summary: what was done and what is next")},
           {"touches", {{"type", "array"}, {"items", {{"type", "string"}}},
                        {"description", "node ids or exact symbol names the checkpoint concerns"}}},
           {"tags", {{"type", "array"}, {"items", {{"type", "string"}}},
                     {"description", "optional labels for filtering on recall"}}}}),
      tool_schema(
          "graph_recall",
          "Restore task state after /clear: returns recent checkpoints newest-first, each with its "
          "summary and briefs of the code it touched. Then use graph_context on a linked node to "
          "reload bounded code context. The fastest way to resume a long session.",
          {{"query", string_param("optional filter over checkpoint titles/tags")},
           {"limit", integer_param("max checkpoints returned (default 10)")}}),
      tool_schema("graph_shutdown", "Ask the per-project graph daemon to shut down", nlohmann::json::object()),
  });
}

[[nodiscard]] nlohmann::json daemon_request_for_tool(std::string_view name, const nlohmann::json& arguments) {
  // query/path/explain forward arguments verbatim so optional params (limit,
  // kind, file, direction) reach the daemon; each op applies its own defaults.
  if (name == "graph_query") {
    return make_request("query", arguments);
  }
  if (name == "graph_path") {
    return make_request("path", arguments);
  }
  if (name == "graph_explain") {
    return make_request("explain", arguments);
  }
  if (name == "graph_impact") {
    // Forward arguments verbatim; the daemon op applies direction/depth/limit
    // defaults. id is required and carried through.
    return make_request("impact", arguments);
  }
  if (name == "graph_context") {
    // Forward arguments verbatim; the daemon op applies budget/depth defaults and
    // resolves the focal node from id, label, or query.
    return make_request("context", arguments);
  }
  if (name == "graph_remember") {
    return make_request("remember", arguments);
  }
  if (name == "graph_recall") {
    return make_request("recall", arguments.empty() ? nlohmann::json::object() : arguments);
  }
  if (name == "graph_update") {
    return make_request("update", arguments.empty() ? nlohmann::json::object() : arguments);
  }
  if (name == "graph_status") {
    return make_request("status");
  }
  if (name == "graph_shutdown") {
    return make_request("shutdown");
  }
  return {};
}

}  // namespace

nlohmann::json handle_mcp_request(const nlohmann::json& request, const McpForwarder& forwarder) {
  const auto id = request.value("id", nlohmann::json(nullptr));
  const auto method = request.value("method", std::string{});
  if (request.value("jsonrpc", std::string{}) != "2.0" || method.empty()) {
    return error_response(id, -32600, "invalid JSON-RPC request");
  }

  if (method == "initialize") {
    return response(id, {{"protocolVersion", "2024-11-05"}, {"capabilities", {{"tools", nlohmann::json::object()}}}, {"serverInfo",
                          {{"name", "cgraph-mcp"},
                           {"version",
                            std::string{cgraph::build_info().version} + "+" +
                                std::string{cgraph::build_info().revision}}}}});
  }
  if (method == "tools/list") {
    return response(id, {{"tools", tools()}});
  }
  if (method == "notifications/initialized") {
    return nlohmann::json{};
  }
  if (method != "tools/call") {
    return error_response(id, -32601, "method not found");
  }

  const auto params = request.value("params", nlohmann::json::object());
  const auto name = params.value("name", std::string{});
  const auto arguments = params.value("arguments", nlohmann::json::object());
  const auto daemon_request = daemon_request_for_tool(name, arguments);
  if (daemon_request.empty()) {
    return error_response(id, -32602, "unknown tool: " + name);
  }
  if (!forwarder) {
    return error_response(id, -32603, "missing daemon forwarder");
  }

  const auto daemon_response = forwarder(daemon_request);
  if (!daemon_response.value("ok", false)) {
    return error_response(id, -32603, daemon_response.value("error", std::string{"daemon request failed"}));
  }
  return response(id, text_content(daemon_response.value("result", nlohmann::json::object())));
}

}  // namespace cgraph
