add_executable(cgraph_smoke_test
  smoke_test.cpp)

target_link_libraries(cgraph_smoke_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_smoke_test)
cgraph_enable_sanitizers(cgraph_smoke_test)

add_test(NAME cgraph_smoke_test COMMAND cgraph_smoke_test)

add_executable(cgraph_id_normalization_test
  id_normalization_test.cpp)

target_link_libraries(cgraph_id_normalization_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_id_normalization_test)
cgraph_enable_sanitizers(cgraph_id_normalization_test)

add_test(NAME cgraph_id_normalization_test COMMAND cgraph_id_normalization_test)

add_executable(cgraph_fragment_json_test
  fragment_json_test.cpp)

target_link_libraries(cgraph_fragment_json_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_fragment_json_test)
cgraph_enable_sanitizers(cgraph_fragment_json_test)

add_test(NAME cgraph_fragment_json_test COMMAND cgraph_fragment_json_test)

add_executable(cgraph_seam_test
  seam_test.cpp)

target_link_libraries(cgraph_seam_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_seam_test)
cgraph_enable_sanitizers(cgraph_seam_test)

add_test(NAME cgraph_seam_test COMMAND cgraph_seam_test)

add_executable(cgraph_detect_test
  detect_test.cpp)

target_link_libraries(cgraph_detect_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_detect_test)
cgraph_enable_sanitizers(cgraph_detect_test)

add_test(NAME cgraph_detect_test COMMAND cgraph_detect_test)

add_executable(cgraph_path_ignore_test
  path_ignore_test.cpp)

target_link_libraries(cgraph_path_ignore_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_path_ignore_test)
cgraph_enable_sanitizers(cgraph_path_ignore_test)

add_test(NAME cgraph_path_ignore_test COMMAND cgraph_path_ignore_test)

add_executable(cgraph_language_config_test
  language_config_test.cpp)

target_link_libraries(cgraph_language_config_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_language_config_test)
cgraph_enable_sanitizers(cgraph_language_config_test)

add_test(NAME cgraph_language_config_test COMMAND cgraph_language_config_test)

add_executable(cgraph_parser_pool_test
  parser_pool_test.cpp)

target_link_libraries(cgraph_parser_pool_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_parser_pool_test)
cgraph_enable_sanitizers(cgraph_parser_pool_test)

add_test(NAME cgraph_parser_pool_test COMMAND cgraph_parser_pool_test)

add_executable(cgraph_extractor_test
  extractor_test.cpp)

target_link_libraries(cgraph_extractor_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_extractor_test)
cgraph_enable_sanitizers(cgraph_extractor_test)

add_test(NAME cgraph_extractor_test COMMAND cgraph_extractor_test)

add_executable(cgraph_configured_extractors_test
  configured_extractors_test.cpp)

target_link_libraries(cgraph_configured_extractors_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_configured_extractors_test)
cgraph_enable_sanitizers(cgraph_configured_extractors_test)

add_test(NAME cgraph_configured_extractors_test COMMAND cgraph_configured_extractors_test)

add_executable(cgraph_python_extractor_test
  python_extractor_test.cpp)

target_link_libraries(cgraph_python_extractor_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_python_extractor_test)
cgraph_enable_sanitizers(cgraph_python_extractor_test)

add_test(NAME cgraph_python_extractor_test COMMAND cgraph_python_extractor_test)

add_executable(cgraph_javascript_extractor_test
  javascript_extractor_test.cpp)

target_link_libraries(cgraph_javascript_extractor_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_javascript_extractor_test)
cgraph_enable_sanitizers(cgraph_javascript_extractor_test)

add_test(NAME cgraph_javascript_extractor_test COMMAND cgraph_javascript_extractor_test)

add_executable(cgraph_cpp_extractor_test
  cpp_extractor_test.cpp)

target_link_libraries(cgraph_cpp_extractor_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_cpp_extractor_test)
cgraph_enable_sanitizers(cgraph_cpp_extractor_test)

add_test(NAME cgraph_cpp_extractor_test COMMAND cgraph_cpp_extractor_test)

add_executable(cgraph_non_grammar_extractors_test
  non_grammar_extractors_test.cpp)

target_link_libraries(cgraph_non_grammar_extractors_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_non_grammar_extractors_test)
cgraph_enable_sanitizers(cgraph_non_grammar_extractors_test)

add_test(NAME cgraph_non_grammar_extractors_test COMMAND cgraph_non_grammar_extractors_test)

add_executable(cgraph_file_extraction_test
  file_extraction_test.cpp)

target_link_libraries(cgraph_file_extraction_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_file_extraction_test)
cgraph_enable_sanitizers(cgraph_file_extraction_test)

add_test(NAME cgraph_file_extraction_test COMMAND cgraph_file_extraction_test)

add_executable(cgraph_snapshot_source_reader_test
  snapshot_source_reader_test.cpp)

target_link_libraries(cgraph_snapshot_source_reader_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_snapshot_source_reader_test)
cgraph_enable_sanitizers(cgraph_snapshot_source_reader_test)

add_test(NAME cgraph_snapshot_source_reader_test COMMAND cgraph_snapshot_source_reader_test)

add_executable(cgraph_extractor_hardening_test
  extractor_hardening_test.cpp)

target_link_libraries(cgraph_extractor_hardening_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_extractor_hardening_test)
cgraph_enable_sanitizers(cgraph_extractor_hardening_test)

add_test(NAME cgraph_extractor_hardening_test COMMAND cgraph_extractor_hardening_test)

add_executable(cgraph_file_watcher_test
  file_watcher_test.cpp)

target_link_libraries(cgraph_file_watcher_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_file_watcher_test)
cgraph_enable_sanitizers(cgraph_file_watcher_test)

add_test(NAME cgraph_file_watcher_test COMMAND cgraph_file_watcher_test)

add_executable(cgraph_file_cache_test
  file_cache_test.cpp)

target_link_libraries(cgraph_file_cache_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_file_cache_test)
cgraph_enable_sanitizers(cgraph_file_cache_test)

add_test(NAME cgraph_file_cache_test COMMAND cgraph_file_cache_test)

add_executable(cgraph_index_persistence_test
  index_persistence_test.cpp)

target_link_libraries(cgraph_index_persistence_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_index_persistence_test)
cgraph_enable_sanitizers(cgraph_index_persistence_test)

add_test(NAME cgraph_index_persistence_test COMMAND cgraph_index_persistence_test)

add_executable(cgraph_semantic_cache_test
  semantic_cache_test.cpp)

target_link_libraries(cgraph_semantic_cache_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_cache_test)
cgraph_enable_sanitizers(cgraph_semantic_cache_test)

add_test(NAME cgraph_semantic_cache_test COMMAND cgraph_semantic_cache_test)

add_executable(cgraph_semantic_chunk_plan_test
  semantic_chunk_plan_test.cpp)

target_link_libraries(cgraph_semantic_chunk_plan_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_chunk_plan_test)
cgraph_enable_sanitizers(cgraph_semantic_chunk_plan_test)

add_test(NAME cgraph_semantic_chunk_plan_test COMMAND cgraph_semantic_chunk_plan_test)

add_executable(cgraph_semantic_code_links_test
  semantic_code_links_test.cpp)

target_link_libraries(cgraph_semantic_code_links_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_code_links_test)
cgraph_enable_sanitizers(cgraph_semantic_code_links_test)

add_test(NAME cgraph_semantic_code_links_test COMMAND cgraph_semantic_code_links_test)

add_executable(cgraph_semantic_connectivity_test
  semantic_connectivity_test.cpp)

target_link_libraries(cgraph_semantic_connectivity_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_connectivity_test)
cgraph_enable_sanitizers(cgraph_semantic_connectivity_test)

add_test(NAME cgraph_semantic_connectivity_test COMMAND cgraph_semantic_connectivity_test)

add_executable(cgraph_semantic_drop_test
  semantic_drop_test.cpp)

target_link_libraries(cgraph_semantic_drop_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_drop_test)
cgraph_enable_sanitizers(cgraph_semantic_drop_test)

add_test(NAME cgraph_semantic_drop_test COMMAND cgraph_semantic_drop_test)

add_executable(cgraph_semantic_fragment_validation_test
  semantic_fragment_validation_test.cpp)

target_link_libraries(cgraph_semantic_fragment_validation_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_fragment_validation_test)
cgraph_enable_sanitizers(cgraph_semantic_fragment_validation_test)

add_test(NAME cgraph_semantic_fragment_validation_test COMMAND cgraph_semantic_fragment_validation_test)

add_executable(cgraph_semantic_ingest_test
  semantic_ingest_test.cpp)

target_link_libraries(cgraph_semantic_ingest_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_ingest_test)
cgraph_enable_sanitizers(cgraph_semantic_ingest_test)

add_test(NAME cgraph_semantic_ingest_test COMMAND cgraph_semantic_ingest_test)

add_executable(cgraph_semantic_status_test
  semantic_status_test.cpp)

target_link_libraries(cgraph_semantic_status_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_status_test)
cgraph_enable_sanitizers(cgraph_semantic_status_test)

add_test(NAME cgraph_semantic_status_test COMMAND cgraph_semantic_status_test)

add_executable(cgraph_host_contract_test
  host_contract_test.cpp)

cgraph_set_warnings(cgraph_host_contract_test)
cgraph_enable_sanitizers(cgraph_host_contract_test)

add_test(NAME cgraph_host_contract_test
  COMMAND cgraph_host_contract_test "${CMAKE_SOURCE_DIR}/docs/host-skill-contract.md")

add_executable(cgraph_hook_integration_test
  hook_integration_test.cpp)

cgraph_set_warnings(cgraph_hook_integration_test)
cgraph_enable_sanitizers(cgraph_hook_integration_test)

add_test(NAME cgraph_hook_integration_test
  COMMAND cgraph_hook_integration_test "${CMAKE_SOURCE_DIR}/integrations/hooks/cgraph-hook.sh")

add_executable(cgraph_always_on_integration_test
  always_on_integration_test.cpp)

cgraph_set_warnings(cgraph_always_on_integration_test)
cgraph_enable_sanitizers(cgraph_always_on_integration_test)

add_test(NAME cgraph_always_on_integration_test
  COMMAND cgraph_always_on_integration_test
    "${CMAKE_SOURCE_DIR}/integrations/always-on/SKILL.md"
    "${CMAKE_SOURCE_DIR}/integrations/always-on/cgraph-always-on.sh")

add_executable(cgraph_mcp_server_test
  mcp_server_test.cpp)

target_link_libraries(cgraph_mcp_server_test
  PRIVATE
    cgraph::mcp
    cgraph::engine)

cgraph_set_warnings(cgraph_mcp_server_test)
cgraph_enable_sanitizers(cgraph_mcp_server_test)

add_test(NAME cgraph_mcp_server_test COMMAND cgraph_mcp_server_test)

add_executable(cgraph_host_surface_integration_test
  host_surface_integration_test.cpp)

target_link_libraries(cgraph_host_surface_integration_test
  PRIVATE
    cgraph::engine
    cgraph::mcp)

cgraph_set_warnings(cgraph_host_surface_integration_test)
cgraph_enable_sanitizers(cgraph_host_surface_integration_test)

add_test(NAME cgraph_host_surface_integration_test
  COMMAND cgraph_host_surface_integration_test
    "${CMAKE_SOURCE_DIR}/integrations/hooks/cgraph-hook.sh"
    "${CMAKE_SOURCE_DIR}/integrations/always-on/cgraph-always-on.sh")

add_executable(cgraph_incremental_update_test
  incremental_update_test.cpp)

target_link_libraries(cgraph_incremental_update_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_incremental_update_test)
cgraph_enable_sanitizers(cgraph_incremental_update_test)

add_test(NAME cgraph_incremental_update_test COMMAND cgraph_incremental_update_test)

add_executable(cgraph_incremental_dedup_test
  incremental_dedup_test.cpp)

target_link_libraries(cgraph_incremental_dedup_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_incremental_dedup_test)
cgraph_enable_sanitizers(cgraph_incremental_dedup_test)

add_test(NAME cgraph_incremental_dedup_test COMMAND cgraph_incremental_dedup_test)

add_executable(cgraph_incremental_rescan_test
  incremental_rescan_test.cpp)

target_link_libraries(cgraph_incremental_rescan_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_incremental_rescan_test)
cgraph_enable_sanitizers(cgraph_incremental_rescan_test)

add_test(NAME cgraph_incremental_rescan_test COMMAND cgraph_incremental_rescan_test)

add_executable(cgraph_extractor_goldens_test
  extractor_goldens_test.cpp)

target_link_libraries(cgraph_extractor_goldens_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_extractor_goldens_test)
cgraph_enable_sanitizers(cgraph_extractor_goldens_test)

add_test(NAME cgraph_extractor_goldens_test COMMAND cgraph_extractor_goldens_test)

add_executable(cgraph_graph_builder_test
  graph_builder_test.cpp)

target_link_libraries(cgraph_graph_builder_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_graph_builder_test)
cgraph_enable_sanitizers(cgraph_graph_builder_test)

add_test(NAME cgraph_graph_builder_test COMMAND cgraph_graph_builder_test)

add_executable(cgraph_tsconfig_aliases_test
  tsconfig_aliases_test.cpp)

target_link_libraries(cgraph_tsconfig_aliases_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_tsconfig_aliases_test)
cgraph_enable_sanitizers(cgraph_tsconfig_aliases_test)

add_test(NAME cgraph_tsconfig_aliases_test COMMAND cgraph_tsconfig_aliases_test)

add_executable(cgraph_semantic_orchestration_test
  semantic_orchestration_test.cpp)

target_link_libraries(cgraph_semantic_orchestration_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_semantic_orchestration_test)
cgraph_enable_sanitizers(cgraph_semantic_orchestration_test)

add_test(NAME cgraph_semantic_orchestration_test COMMAND cgraph_semantic_orchestration_test)

add_executable(cgraph_daemon_server_test
  daemon_server_test.cpp)

target_link_libraries(cgraph_daemon_server_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_server_test)
cgraph_enable_sanitizers(cgraph_daemon_server_test)

add_test(NAME cgraph_daemon_server_test COMMAND cgraph_daemon_server_test)

add_executable(cgraph_daemon_persistence_test
  daemon_persistence_test.cpp)

target_link_libraries(cgraph_daemon_persistence_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_persistence_test)
cgraph_enable_sanitizers(cgraph_daemon_persistence_test)

add_test(NAME cgraph_daemon_persistence_test COMMAND cgraph_daemon_persistence_test)

add_executable(cgraph_engine_test
  engine_test.cpp)

target_link_libraries(cgraph_engine_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_engine_test)
cgraph_enable_sanitizers(cgraph_engine_test)

add_test(NAME cgraph_engine_test COMMAND cgraph_engine_test)

add_executable(cgraph_dedup_test
  dedup_test.cpp)

target_link_libraries(cgraph_dedup_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_dedup_test)
cgraph_enable_sanitizers(cgraph_dedup_test)

add_test(NAME cgraph_dedup_test COMMAND cgraph_dedup_test)

add_executable(cgraph_analysis_test
  analysis_test.cpp)

target_link_libraries(cgraph_analysis_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_analysis_test)
cgraph_enable_sanitizers(cgraph_analysis_test)

add_test(NAME cgraph_analysis_test COMMAND cgraph_analysis_test)

add_executable(cgraph_graph_parity_test
  graph_parity_test.cpp)

target_link_libraries(cgraph_graph_parity_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_graph_parity_test)
cgraph_enable_sanitizers(cgraph_graph_parity_test)

add_test(NAME cgraph_graph_parity_test COMMAND cgraph_graph_parity_test)

add_executable(cgraph_export_json_test
  export_json_test.cpp)

target_link_libraries(cgraph_export_json_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_export_json_test)
cgraph_enable_sanitizers(cgraph_export_json_test)

add_test(NAME cgraph_export_json_test COMMAND cgraph_export_json_test)

add_executable(cgraph_pipeline_test
  pipeline_test.cpp)

target_link_libraries(cgraph_pipeline_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_pipeline_test)
cgraph_enable_sanitizers(cgraph_pipeline_test)

add_test(NAME cgraph_pipeline_test COMMAND cgraph_pipeline_test)

add_executable(cgraph_operation_stats_test
  operation_stats_test.cpp)

target_link_libraries(cgraph_operation_stats_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_operation_stats_test)
cgraph_enable_sanitizers(cgraph_operation_stats_test)

add_test(NAME cgraph_operation_stats_test COMMAND cgraph_operation_stats_test)

add_test(
  NAME cgraph_cli_one_shot_test
  COMMAND cgraph
    --root "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/one_shot"
    --out "${CMAKE_CURRENT_BINARY_DIR}/cli-one-shot-out")

add_executable(cgraph_node_link_loader_test
  node_link_loader_test.cpp)

target_link_libraries(cgraph_node_link_loader_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_node_link_loader_test)
cgraph_enable_sanitizers(cgraph_node_link_loader_test)

add_test(NAME cgraph_node_link_loader_test COMMAND cgraph_node_link_loader_test)

add_executable(cgraph_daemon_identity_test
  daemon_identity_test.cpp)

target_link_libraries(cgraph_daemon_identity_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_identity_test)
cgraph_enable_sanitizers(cgraph_daemon_identity_test)

add_test(NAME cgraph_daemon_identity_test COMMAND cgraph_daemon_identity_test)

add_test(
  NAME cgraph_daemon_benchmark_query_cli_test
  COMMAND graphd
    --benchmark-query
    --graph "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/daemon_query/graph.json"
    --query Alpha)

set_tests_properties(cgraph_daemon_benchmark_query_cli_test
  PROPERTIES
    PASS_REGULAR_EXPRESSION "Alpha")

add_executable(cgraph_daemon_endpoint_test
  daemon_endpoint_test.cpp)

target_link_libraries(cgraph_daemon_endpoint_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_endpoint_test)
cgraph_enable_sanitizers(cgraph_daemon_endpoint_test)

add_test(NAME cgraph_daemon_endpoint_test COMMAND cgraph_daemon_endpoint_test)

add_executable(cgraph_daemon_security_test
  daemon_security_test.cpp)

target_link_libraries(cgraph_daemon_security_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_security_test)
cgraph_enable_sanitizers(cgraph_daemon_security_test)

add_test(NAME cgraph_daemon_security_test COMMAND cgraph_daemon_security_test)

add_executable(cgraph_daemon_supervisor_test
  daemon_supervisor_test.cpp)

target_link_libraries(cgraph_daemon_supervisor_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_supervisor_test)
cgraph_enable_sanitizers(cgraph_daemon_supervisor_test)

add_test(NAME cgraph_daemon_supervisor_test COMMAND cgraph_daemon_supervisor_test)

add_executable(cgraph_enrich_drainer_test
  enrich_drainer_test.cpp)

target_link_libraries(cgraph_enrich_drainer_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_enrich_drainer_test)
cgraph_enable_sanitizers(cgraph_enrich_drainer_test)

add_test(NAME cgraph_enrich_drainer_test COMMAND cgraph_enrich_drainer_test)

add_executable(cgraph_skills_install_test
  skills_install_test.cpp)

target_link_libraries(cgraph_skills_install_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_skills_install_test)
cgraph_enable_sanitizers(cgraph_skills_install_test)

add_test(NAME cgraph_skills_install_test COMMAND cgraph_skills_install_test)

add_executable(cgraph_protocol_test
  protocol_test.cpp)

target_link_libraries(cgraph_protocol_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_protocol_test)
cgraph_enable_sanitizers(cgraph_protocol_test)

add_test(NAME cgraph_protocol_test COMMAND cgraph_protocol_test)

add_executable(cgraph_daemon_ops_test
  daemon_ops_test.cpp)

target_link_libraries(cgraph_daemon_ops_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_ops_test)
cgraph_enable_sanitizers(cgraph_daemon_ops_test)

add_test(NAME cgraph_daemon_ops_test COMMAND cgraph_daemon_ops_test)

add_executable(cgraph_daemon_snapshot_test
  daemon_snapshot_test.cpp)

target_link_libraries(cgraph_daemon_snapshot_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_snapshot_test)
cgraph_enable_sanitizers(cgraph_daemon_snapshot_test)

add_test(NAME cgraph_daemon_snapshot_test COMMAND cgraph_daemon_snapshot_test)

add_executable(cgraph_daemon_lifecycle_test
  daemon_lifecycle_test.cpp)

target_link_libraries(cgraph_daemon_lifecycle_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_lifecycle_test)
cgraph_enable_sanitizers(cgraph_daemon_lifecycle_test)

add_test(NAME cgraph_daemon_lifecycle_test COMMAND cgraph_daemon_lifecycle_test)

add_executable(cgraph_daemon_hardening_test
  daemon_hardening_test.cpp)

target_link_libraries(cgraph_daemon_hardening_test
  PRIVATE
    cgraph::client_runtime
    cgraph::engine)

cgraph_set_warnings(cgraph_daemon_hardening_test)
cgraph_enable_sanitizers(cgraph_daemon_hardening_test)

add_test(NAME cgraph_daemon_hardening_test COMMAND cgraph_daemon_hardening_test)

add_executable(cgraph_client_runtime_test
  client_runtime_test.cpp)

target_link_libraries(cgraph_client_runtime_test
  PRIVATE
    cgraph::client_runtime
    cgraph::engine)

cgraph_set_warnings(cgraph_client_runtime_test)
cgraph_enable_sanitizers(cgraph_client_runtime_test)

add_test(NAME cgraph_client_runtime_test COMMAND cgraph_client_runtime_test)

add_executable(cgraph_pack_context_parity_test
  pack_context_parity_test.cpp)

target_compile_definitions(cgraph_pack_context_parity_test
  PRIVATE
    CGRAPH_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    CGRAPH_PARITY_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/pack_context_parity")

target_link_libraries(cgraph_pack_context_parity_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_pack_context_parity_test)
cgraph_enable_sanitizers(cgraph_pack_context_parity_test)

add_test(NAME cgraph_pack_context_parity_test COMMAND cgraph_pack_context_parity_test)

add_executable(cgraph_retrieval_quality_test
  retrieval_quality_test.cpp)

target_compile_definitions(cgraph_retrieval_quality_test
  PRIVATE
    CGRAPH_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    CGRAPH_PARITY_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/pack_context_parity")

target_link_libraries(cgraph_retrieval_quality_test
  PRIVATE
    cgraph::engine)

cgraph_set_warnings(cgraph_retrieval_quality_test)
cgraph_enable_sanitizers(cgraph_retrieval_quality_test)

add_test(NAME cgraph_retrieval_quality_test COMMAND cgraph_retrieval_quality_test)
