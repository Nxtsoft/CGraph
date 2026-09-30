#include "cgraph/langgraph_config.hpp"

#include "cgraph/normalize.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace cgraph {
namespace {

// The Agent Server's route table as `@langchain/langgraph-api` 1.5.1 registers it
// (dist/api/{meta,assistants,runs,threads,store,protocol}.mjs, mounted in
// dist/server.mjs behind the `http.disable_<group>` switches; the protocol routes
// ride on `disable_runs`). `subgraphs/:namespace?` is Hono's optional segment,
// so it serves both spellings. The internal `/internal/truncate` test hook is not
// part of the public surface. Reference:
// https://docs.langchain.com/langsmith/agent-server (Agent Server API, 2026).
constexpr std::array<LangGraphRoute, 53> kRoutes = {{
    {"get", "/info", "meta"},
    {"get", "/ok", "meta"},
    {"post", "/assistants", "assistants"},
    {"post", "/assistants/search", "assistants"},
    {"post", "/assistants/count", "assistants"},
    {"get", "/assistants/:assistant_id", "assistants"},
    {"delete", "/assistants/:assistant_id", "assistants"},
    {"patch", "/assistants/:assistant_id", "assistants"},
    {"get", "/assistants/:assistant_id/graph", "assistants"},
    {"get", "/assistants/:assistant_id/schemas", "assistants"},
    {"get", "/assistants/:assistant_id/subgraphs", "assistants"},
    {"get", "/assistants/:assistant_id/subgraphs/:namespace", "assistants"},
    {"post", "/assistants/:assistant_id/latest", "assistants"},
    {"post", "/assistants/:assistant_id/versions", "assistants"},
    {"post", "/runs/crons", "runs"},
    {"post", "/runs/crons/search", "runs"},
    {"delete", "/runs/crons/:cron_id", "runs"},
    {"post", "/threads/:thread_id/runs/crons", "runs"},
    {"post", "/runs/stream", "runs"},
    {"get", "/runs/:run_id/stream", "runs"},
    {"post", "/runs/wait", "runs"},
    {"post", "/runs", "runs"},
    {"post", "/runs/batch", "runs"},
    {"get", "/threads/:thread_id/runs", "runs"},
    {"post", "/threads/:thread_id/runs", "runs"},
    {"post", "/threads/:thread_id/runs/stream", "runs"},
    {"post", "/threads/:thread_id/runs/wait", "runs"},
    {"get", "/threads/:thread_id/runs/:run_id", "runs"},
    {"delete", "/threads/:thread_id/runs/:run_id", "runs"},
    {"get", "/threads/:thread_id/runs/:run_id/join", "runs"},
    {"get", "/threads/:thread_id/runs/:run_id/stream", "runs"},
    {"post", "/threads/:thread_id/runs/:run_id/cancel", "runs"},
    {"get", "/threads/:thread_id/stream/events", "runs"},
    {"post", "/threads/:thread_id/commands", "runs"},
    {"post", "/threads/:thread_id/stream/events", "runs"},
    {"post", "/threads", "threads"},
    {"post", "/threads/search", "threads"},
    {"post", "/threads/count", "threads"},
    {"get", "/threads/:thread_id/state", "threads"},
    {"post", "/threads/:thread_id/state", "threads"},
    {"get", "/threads/:thread_id/state/:checkpoint_id", "threads"},
    {"post", "/threads/:thread_id/state/checkpoint", "threads"},
    {"get", "/threads/:thread_id/history", "threads"},
    {"post", "/threads/:thread_id/history", "threads"},
    {"get", "/threads/:thread_id", "threads"},
    {"delete", "/threads/:thread_id", "threads"},
    {"patch", "/threads/:thread_id", "threads"},
    {"post", "/threads/:thread_id/copy", "threads"},
    {"post", "/store/namespaces", "store"},
    {"post", "/store/items/search", "store"},
    {"put", "/store/items", "store"},
    {"delete", "/store/items", "store"},
    {"get", "/store/items", "store"},
}};

// 1-based line of a byte offset.
[[nodiscard]] std::uint32_t line_at(std::string_view source, std::size_t offset) {
  const auto end = source.begin() + static_cast<std::ptrdiff_t>(std::min(offset, source.size()));
  return static_cast<std::uint32_t>(std::count(source.begin(), end, '\n') + 1);
}

// Offset just past the JSON string starting at `open` (a `"`), or npos.
[[nodiscard]] std::size_t skip_string(std::string_view source, std::size_t open) {
  for (std::size_t index = open + 1; index < source.size(); ++index) {
    if (source[index] == '\\') {
      ++index;
    } else if (source[index] == '"') {
      return index + 1;
    }
  }
  return std::string_view::npos;
}

// Offset of the `"key"` token that is a member of the object opening at
// `object_open` (depth one inside it), or npos. JSON already parsed, so the
// text is well formed.
[[nodiscard]] std::size_t member_key(std::string_view source, std::size_t object_open, std::string_view key) {
  int depth = 0;
  for (std::size_t index = object_open; index < source.size();) {
    const char ch = source[index];
    if (ch == '"') {
      const auto end = skip_string(source, index);
      if (end == std::string_view::npos) {
        return end;
      }
      if (depth == 1 && source.substr(index + 1, end - index - 2) == key) {
        // A key is followed by `:`; a value string is not.
        auto next = end;
        while (next < source.size() && (source[next] == ' ' || source[next] == '\t' || source[next] == '\r' || source[next] == '\n')) {
          ++next;
        }
        if (next < source.size() && source[next] == ':') {
          return index;
        }
      }
      index = end;
      continue;
    }
    if (ch == '{' || ch == '[') {
      ++depth;
    } else if (ch == '}' || ch == ']') {
      if (--depth == 0) {
        return std::string_view::npos;
      }
    }
    ++index;
  }
  return std::string_view::npos;
}

// Offset of the bracket closing the one at `open`, or npos.
[[nodiscard]] std::size_t matching_close(std::string_view source, std::size_t open) {
  int depth = 0;
  for (std::size_t index = open; index < source.size();) {
    const char ch = source[index];
    if (ch == '"') {
      index = skip_string(source, index);
      if (index == std::string_view::npos) {
        return index;
      }
      continue;
    }
    if (ch == '{' || ch == '[') {
      ++depth;
    } else if ((ch == '}' || ch == ']') && --depth == 0) {
      return index;
    }
    ++index;
  }
  return std::string_view::npos;
}

}  // namespace

std::span<const LangGraphRoute> langgraph_server_routes() { return kRoutes; }

ExtractionResult extract_langgraph_config(const ExtractionContext& context) {
  ExtractionResult result;
  auto& fragment = result.fragment;
  const std::string_view source = context.source;
  const auto file_id = make_id(context.relative_path);
  const std::filesystem::path source_path(context.relative_path);
  std::string file_label = source_path.filename().string();
  if (source_path.has_parent_path() && source_path.parent_path().has_filename()) {
    file_label = source_path.parent_path().filename().string() + "/" + file_label;
  }
  fragment.nodes.push_back(Node{
      .id = file_id,
      .label = file_label.empty() ? context.relative_path : std::move(file_label),
      .source_file = context.source_file,
      .source_location = SourceLocation{.start_line = 1, .end_line = 1},
      .kind = "file",
      .confidence = Confidence::Extracted,
  });

  nlohmann::json config;
  try {
    config = nlohmann::json::parse(source);
  } catch (const nlohmann::json::exception& error) {
    fragment.warnings.push_back(std::string{"failed to parse langgraph.json: "} + error.what());
    return result;
  }
  const auto graphs = config.is_object() ? config.find("graphs") : config.end();
  if (!config.is_object() || graphs == config.end() || !graphs->is_object() || graphs->empty()) {
    return result;  // no graphs: nothing is served
  }

  const auto root_open = source.find('{');
  const auto graphs_key = member_key(source, root_open, "graphs");
  const auto graphs_open = graphs_key == std::string_view::npos ? graphs_key : source.find('{', graphs_key);
  const auto graphs_close = graphs_open == std::string_view::npos ? graphs_open : matching_close(source, graphs_open);
  const auto graphs_line = graphs_key == std::string_view::npos ? 1U : line_at(source, graphs_key);
  const auto graphs_end_line = graphs_close == std::string_view::npos ? graphs_line : line_at(source, graphs_close);

  const auto server_id = make_id(context.relative_path + ":langgraph_server");
  fragment.nodes.push_back(Node{
      .id = server_id,
      .label = "LangGraph server",
      .source_file = context.source_file,
      .source_location = SourceLocation{.start_line = graphs_line, .end_line = graphs_end_line},
      .kind = "langgraph_server",
      .confidence = Confidence::Extracted,
  });
  fragment.edges.push_back(Edge{.source = file_id, .target = server_id, .relation = "contains", .confidence = Confidence::Extracted});

  for (const auto& [name, value] : graphs->items()) {
    std::string entrypoint;
    if (value.is_string()) {
      entrypoint = value.get<std::string>();
    } else if (value.is_object()) {
      entrypoint = value.value("path", std::string{});
    }
    const auto key = graphs_open == std::string_view::npos ? graphs_open : member_key(source, graphs_open, name);
    const auto line = key == std::string_view::npos ? graphs_line : line_at(source, key);
    const auto graph_id = make_id(context.relative_path + ":langgraph_graph:" + name);
    Node graph{
        .id = graph_id,
        .label = name,
        .source_file = context.source_file,
        .source_location = SourceLocation{.start_line = line, .end_line = line},
        .kind = "langgraph_graph",
        .confidence = Confidence::Extracted,
    };
    if (!entrypoint.empty()) {
      graph.properties.emplace("entrypoint", entrypoint);
    }
    fragment.nodes.push_back(std::move(graph));
    fragment.edges.push_back(Edge{.source = file_id, .target = graph_id, .relation = "contains", .confidence = Confidence::Extracted});
    // The server dispatches a run to this graph by assistant_id.
    fragment.edges.push_back(Edge{.source = server_id, .target = graph_id, .relation = "defines", .confidence = Confidence::Extracted});
  }

  const auto http = config.find("http");
  const auto disabled = [&](std::string_view group) {
    if (http == config.end() || !http->is_object()) {
      return false;
    }
    const auto flag = http->find("disable_" + std::string(group));
    return flag != http->end() && flag->is_boolean() && flag->get<bool>();
  };
  for (const auto& route : kRoutes) {
    if (disabled(route.group)) {
      continue;
    }
    result.raw_relations.push_back(RawRelation{
        .source_id = server_id,
        .target_label = {},
        .relation = "file_route",  // the path is absolute: the server mounts every group at `/`
        .context = std::string(route.method) + " " + std::string(route.path),
        .source_file = context.source_file,
    });
  }
  return result;
}

}  // namespace cgraph
