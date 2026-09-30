#pragma once

#include "cgraph/extractor.hpp"
#include "cgraph/language_config.hpp"

#include <span>
#include <string_view>

// LangGraph deployment config (`langgraph.json`): routes a framework serves,
// declared by config rather than by code.
//
// A repository holding `langgraph.json` is a LangGraph Agent Server. The server
// registers a fixed REST surface (`POST /runs/wait`, `POST /threads/{thread_id}/runs/stream`,
// `GET /assistants/{assistant_id}`, ...) and dispatches each call to one of the
// graphs the config's `graphs` object names, chosen by the `assistant_id` in the
// request body. No handler for those routes exists in the repo's own source, so
// a client calling `${AGENTS_API_URL}/runs/wait` would otherwise meet no provider.
//
// The extractor emits the config's `file` node, one `langgraph_server` node
// spanning the `graphs` object, and one `langgraph_graph` node per graph entry
// (property `entrypoint`, the `./path:export` it names), each `contains`-ed by the
// file. Every route of the server surface becomes a `file_route` fact whose
// handler is the server node, so resolve_contracts mints the canonical
// `endpoint:<METHOD> <path>` node `handled_by` it, exactly as for a Next.js route
// file. A route group the config switches off (`http.disable_runs`, ...) is not
// emitted. A `langgraph.json` without a non-empty `graphs` object yields its file
// node only: it declares no server.
namespace cgraph {

struct LangGraphRoute {
  std::string_view method;  // lowercase verb, as a `file_route` context spells it
  std::string_view path;    // provider spelling, `:param` segments
  std::string_view group;   // the `http.disable_<group>` flag that switches it off
};

// The Agent Server route surface, in registration order.
[[nodiscard]] std::span<const LangGraphRoute> langgraph_server_routes();

[[nodiscard]] ExtractionResult extract_langgraph_config(const ExtractionContext& context);

}  // namespace cgraph
