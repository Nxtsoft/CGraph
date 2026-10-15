#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace cgraph {

using McpForwarder = std::function<nlohmann::json(const nlohmann::json& daemon_request)>;

// Runs graph_change_context. Unset, the engine's change_context runs alone; the
// cgraph-mcp binary passes one that adds other services' consumers and providers.
using McpChangeContext = std::function<nlohmann::json(const nlohmann::json& arguments)>;

// Names of every tool the server implements, in tools/list order.
[[nodiscard]] std::vector<std::string> mcp_tool_names();

// Parses a --tools value: comma-separated tool names, whitespace around each
// ignored. Empty, or any entry "all", means every tool (an empty set). Throws
// std::invalid_argument naming the first entry the server does not implement.
[[nodiscard]] std::set<std::string> parse_mcp_tool_list(const std::string& value);

// listed_tools limits what tools/list advertises (empty: every tool). Hosts that
// list each tool in the agent's prompt pay for every name on every request, so a
// host can expose a small core and leave the rest unadvertised.
[[nodiscard]] nlohmann::json handle_mcp_request(const nlohmann::json& request, const McpForwarder& forwarder,
                                                const McpChangeContext& change_context_runner = {},
                                                const std::set<std::string>& listed_tools = {});

}  // namespace cgraph
