#pragma once

#include <nlohmann/json.hpp>

#include <functional>

namespace cgraph {

using McpForwarder = std::function<nlohmann::json(const nlohmann::json& daemon_request)>;

// Runs graph_change_context. Unset, the engine's change_context runs alone; the
// cgraph-mcp binary passes one that adds other services' consumers and providers.
using McpChangeContext = std::function<nlohmann::json(const nlohmann::json& arguments)>;

[[nodiscard]] nlohmann::json handle_mcp_request(const nlohmann::json& request, const McpForwarder& forwarder,
                                                const McpChangeContext& change_context_runner = {});

}  // namespace cgraph
