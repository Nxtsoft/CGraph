# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`cgraph` is a native C++20/C graph-analysis engine that ports Graphify's deterministic
code-graph pipeline. It produces a Graphify-compatible `graph.json` (node-link format) plus
HTML/SVG/Obsidian/Cypher exports, and serves low-latency graph queries from a resident
per-project daemon. The design split is deliberate: **the native binary owns deterministic
graph work, chunk planning, fragment validation, and local graph mutation. Hosts own all
model/LLM logic** — there are no API keys, providers, or LLM clients in the binary. Semantic
enrichment enters only as host-written, validated fragment files. See
`docs/host-skill-contract.md`.

## Build, test, run

The project uses CMake presets + vcpkg. **vcpkg is vendored at `./.vcpkg`** and the presets
read `$env{VCPKG_ROOT}`, which is **not** set in the environment — you must export it:

```bash
export VCPKG_ROOT="$PWD/.vcpkg"          # required; presets fail without it
cmake --preset default                    # configure -> build/default (Ninja, Debug)
cmake --build build/default               # build all targets
ctest --preset default                    # run the smoke tests (output-on-failure)
ctest --preset default -R cgraph_detect_test   # run a single test by name (regex)
```

Other presets: `sanitizers` (ASan/UBSan, `build/sanitizers`) and `fuzzers` (libFuzzer +
ASan/UBSan, `build/fuzzers`, requires `CGRAPH_BUILD_FUZZERS=ON`). System deps via vcpkg:
`igraph`, `curl`, `nlohmann-json`, `utf8proc`; tree-sitter is vendored under `vendor/tree-sitter`.

Built binaries land in `build/default/src/<target>/`:

- `cgraph` (cli) — one-shot: `cgraph --root PATH --out cgraph-out` builds the graph and writes all exports.
- `graphd` (daemon) — resident per-project server; also `--benchmark-query --graph PATH --query TEXT`.
- `cgraph-client` (client) — thin client: `cgraph-client --root PATH <query|path|explain|update|status|shutdown> [JSON params]`.
- `cgraph-mcp` (mcp) — MCP server (stdio JSON-RPC) fronting the daemon ops.

Benchmarks (compare native vs Python Graphify): `scripts/benchmark_one_shot.py`, `scripts/benchmark_daemon_query.py`.

## Architecture

Everything links against one static library, **`cgraph::engine`** (`src/engine/`), which holds
the entire pipeline. The four executables (`cli`, `daemon`, `client`, `mcp`) are thin `main.cpp`
shells over it. Core data types (`Node`, `Edge`, `Hyperedge`, `Fragment`, `GraphSnapshot`,
`Confidence`, `BuildState`) live in `src/engine/include/cgraph/types.hpp` — read this first.

**Deterministic pipeline** (`pipeline.cpp::run_one_shot`, the canonical flow):
`detect_project_files` → per-file `extract_detected_file` (tree-sitter via `parser_pool`,
language-specific `python_extractor`/`javascript_extractor`/`non_grammar_extractors`, the
latter including `contract_schemas` for OpenAPI JSON, `.proto` and `.graphql` documents,
configured by `language_config`/`configured_extractors`; every function body also gets a
rename-insensitive `fingerprint` for `report clones`, runtime-only, never exported) → `merge_fragments`
(`graph_builder`) → `resolve_raw_calls` → `resolve_raw_relations` → `resolve_contracts`
(`contracts`: route registrations and router mounts become `endpoint` nodes with full paths
composed across files; `fetch`/client/wrapper calls become `CONSUMES` edges to them) →
`semantic_dedup` (`dedup`) → `detect_communities`
(`analysis`/igraph clustering) → `analyze_graph` → `write_exports` (`export_json`). ID
normalization (`normalize.cpp`) preserves Graphify's ID contract — this is a parity surface,
treat it as load-bearing.

**Daemon / client / IPC** (`graph-daemon-client` capability):
- `daemon_identity.cpp` derives a per-project-root hash + endpoint name (one daemon per canonical project root).
- `daemon_lifecycle.cpp` / `daemon_endpoint.cpp` handle spawn, listen, and connection.
- `daemon_ops.cpp::handle_daemon_request` dispatches the eleven ops (`query`/`path`/`explain`/`impact`/`context`/`update`/`status`/`shutdown`/`remember`/`recall`/`report`). Graph state is a `shared_ptr<const GraphSnapshot>` read under `snapshot_mutex`; mutations go through a **single-writer path** (`writer_mutex`, `publish_graph_snapshot`/`mutate_graph_snapshot`).
- `protocol.cpp` — length-prefixed JSON frames, `kProtocolVersion = 1`; version-checked on every message.
- `client_runtime.cpp` — thin client with connect/spawn/backoff hooks (`ClientRuntimeHooks`); auto-spawns the daemon if absent. A root holding `cgraph.workspace.json` federates instead (`workspace.cpp`): each member repo is asked exactly as a lone project is, and `impact`/`path` cross between them at the shared `endpoint:` contract nodes. This is the only entry point, so the CLI, thin client and MCP server all federate.
- `incremental_update.cpp` + `file_watcher.cpp` + `file_cache.cpp` — `update .` triggers a full stat-index rescan; the serve loop polls the (gitignore-aware) watcher on `code_poll_interval` and applies incremental updates, with a hydrating full rescan on the first edit after a fast-load restart and a full-dedup reconcile every 5th update. Incremental state re-persists via `persist_if_due` and on exit.
- `daemon_security.cpp` / `daemon_hardening` tests — endpoint hardening surface.

**Semantic enrichment ingest** (`semantic-fragment-ingest`, host-orchestrated, no LLM in binary):
`semantic_chunk_plan.cpp` emits bounded chunk plans for uncached/stale docs/media. Hosts compute
each chunk and atomically write exactly one `chunk_NN.json` fragment into the semantic drop dir.
`semantic_drop.cpp` watches for these; `semantic_fragment_validation.cpp` + `fragment_json.cpp`
validate against the Graphify fragment schema (malformed → rejected, graph unchanged);
`semantic_ingest.cpp` merges valid fragments through the daemon single-writer path;
`semantic_cache.cpp` keys cache records by content hash. Enrichment status is surfaced through
`status` (`enrichment_state`/`pending`/`running`/`stale`/`failed`).

**MCP** (`src/mcp/mcp_server.cpp`): exposes `graph_query`/`graph_path`/`graph_explain`/
`graph_impact`/`graph_context`/`graph_update`/`graph_status`/`graph_shutdown`/`graph_remember`/
`graph_recall`/`graph_report` tools that translate directly to daemon ops. Adds no model logic.

## Conventions

- **Tests are 1:1 with engine source** in `tests/smoke/` (e.g. `dedup.cpp` → `dedup_test.cpp`), each registered as its own CTest executable in `tests/smoke/CMakeLists.txt`. Fixtures live in `tests/fixtures/`. Goldens for parity in `extractor_goldens_test.cpp`. Fuzzers in `tests/fuzz/`.
- **Adding an engine source file** means editing `src/engine/CMakeLists.txt` (explicit source list, not globbed) and adding the matching `_test.cpp` + `add_test` block.
- Warnings/sanitizers are applied per-target via `cmake/CgraphWarnings.cmake` and `cmake/CgraphSanitizers.cmake` (`cgraph_set_warnings` / `cgraph_enable_sanitizers`) — call both on any new target.
- **Graphify parity is a hard contract**: extraction fragment shape, ID normalization, and `graph.json` node-link output must stay compatible. Changes here need golden/parity test coverage.

## Spec workflow

This repo uses **OpenSpec** (`openspec/`, config in `openspec/config.yaml`). The active change is
`openspec/changes/build-native-graphify-variant/` (proposal, design, tasks, and per-capability
specs under `specs/`). The four capability names above map to the spec directories there.
`/opsx:*` slash commands and the `openspec-*` skills drive the propose → apply → verify → archive
loop.

## Testing

Always run the full test suite after multi-file changes and before opening a PR; report pass/fail counts (e.g., '35 passing tests').

## Performance / Benchmarks

When optimizing or refactoring, capture before/after benchmarks (e.g., update time 50.7s -> ~15s) and include them in the PR description.

## UI / Frontend

For graph/UI rendering changes, verify visually via browser screenshots and watch for regressions (e.g., nodes clamping to canvas walls) before shipping.

## Debugging Workflow

Use OpenSpec change proposals for non-trivial fixes and trace root causes via LangSmith/Datadog before implementing.

## Research / Eval Discipline

Python research harnesses (e.g. `research/`) are allowed and encouraged for fast retrieval
experiments. They are NOT product code:

- Python research code is **disposable** unless explicitly promoted to the C++ engine.
- A **metric win in Python is not automatically a product win** — it must transfer through the
  engine's real accounting (tokenizer, snippet caps, JSON-entry cost) and real query path.
- **Never modify eval data or ground-truth labels to improve a metric.** Seeds/candidates must
  come from the query, never from the labels (that leaks the answer).
- **Do not commit or push** research artifacts or experiment output unless explicitly asked.

Any successful retrieval experiment MUST document, in `research/<id>/results.md`:

- exact command run (reproducible)
- baseline metric, candidate metric, and the delta
- files changed
- whether eval data changed (expected: no)
- **which layer the idea belongs in** — graph construction, `graphd` (daemon), retrieval
  (candidate gathering), or context selection (packing)
- **proposed C++ integration point** (file/function), so the Python result has a concrete path
  to production or is explicitly parked

Also record the graph the numbers were measured on (e.g. node/edge count) — absolute metrics are
only comparable across runs on the same graph; rely on within-run deltas.
