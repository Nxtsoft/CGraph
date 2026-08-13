add_library(cgraph_engine STATIC
  analysis.cpp
  daemon_endpoint.cpp
  daemon_server.cpp
  configured_extractors.cpp
  daemon_lifecycle.cpp
  daemon_ops.cpp
  daemon_security.cpp
  daemon_supervisor.cpp
  daemon_identity.cpp
  dedup.cpp
  cpp_extractor.cpp
  detect.cpp
  engine.cpp
  enrich_drainer.cpp
  extractor.cpp
  export_json.cpp
  file_cache.cpp
  file_extraction.cpp
  file_watcher.cpp
  fragment_json.cpp
  graph_builder.cpp
  incremental_update.cpp
  index_persistence.cpp
  javascript_extractor.cpp
  language_config.cpp
  launch_agent.cpp
  non_grammar_extractors.cpp
  normalize.cpp
  operation_stats.cpp
  parser_pool.cpp
  path_ignore.cpp
  pipeline.cpp
  protocol.cpp
  python_extractor.cpp
  seam.cpp
  semantic_chunk_plan.cpp
  semantic_cache.cpp
  semantic_code_links.cpp
  semantic_connectivity.cpp
  semantic_drop.cpp
  semantic_fragment_validation.cpp
  semantic_ingest.cpp
  semantic_orchestration.cpp
  skills_install.cpp
  snapshot_source_reader.cpp
  tsconfig_aliases.cpp)

add_library(cgraph::engine ALIAS cgraph_engine)

# Stamp the engine with the source revision at CONFIGURE time, so a stale
# installed binary is identifiable from `cgraph --version`, `graph_status`,
# and the MCP serverInfo. It refreshes on reconfigure, not per commit -- CI
# and release builds always reconfigure. This is observability only: the
# fast-load index key deliberately stays a hand-bumped logic version (see the
# regression guard in tests/smoke/index_persistence_test.cpp).
execute_process(
  COMMAND git describe --always --dirty --tags
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE CGRAPH_GIT_DESCRIBE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT CGRAPH_GIT_DESCRIBE)
  set(CGRAPH_GIT_DESCRIBE "unknown")
endif()
target_compile_definitions(cgraph_engine
  PRIVATE
    CGRAPH_GIT_DESCRIBE="${CGRAPH_GIT_DESCRIBE}")

target_include_directories(cgraph_engine
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(cgraph_engine
  PUBLIC
    nlohmann_json::nlohmann_json
    cgraph_tree_sitter
  PRIVATE
    CURL::libcurl
    igraph::igraph
    utf8proc::utf8proc)

cgraph_set_warnings(cgraph_engine)
cgraph_enable_sanitizers(cgraph_engine)
