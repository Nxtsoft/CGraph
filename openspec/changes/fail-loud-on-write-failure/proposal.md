# Fail loud when an output file cannot be written

## Why

When `/tmp` filled to 100%, `cgraph seam fuse` (bin-v0.8.0) exited 0 after writing a `graph.json` cut off at the point the disk ran out. The CLI and engine wrote every output through a bare `std::ofstream` and never looked at the stream state, so a short write was silent: `seam fuse`, `seam discover`, `seam gen`, the one-shot build (`write_exports`, `stats.json`), `workspace init` and the enrichment plan/ingest all exited 0 over truncated files. The daemon's `persist_graph_snapshot` and `write_index_manifest` did check the rename but not the write, so a truncated temp file was renamed over the last-known-good `graph.json`.

## What Changes

- One helper, `write_file_atomically` / `try_write_file_atomically` (`src/engine/atomic_write.cpp`), replaces the three private temp-and-rename copies (`daemon_lifecycle.cpp` twice, `index_persistence.cpp`) and every bare `std::ofstream` output write. It writes a sibling `<name>.tmp`, checks the stream after write, flush and close, checks the temp file's size equals the bytes meant, and only then renames it over the destination. On any failure the temp file is removed, a prior file at the destination is left untouched, and the error names the destination and the OS reason.
- The `cgraph` CLI exits 1 with `cgraph: failed to write <path>: <reason>` when any output file cannot be written whole.
- The daemon logs the reason and keeps serving; an unwritten semantic cache or stat index is logged rather than fatal, as an unwritten manifest already was. A `remember` checkpoint that cannot be written is an error response naming the file.

## Non-goals

- The append-only operation-stats ledger (`append_op_stats_ledger`) is unchanged: it already checks the stream and is best-effort by design.
- No output content changes, so the index version key is not bumped.

## Impact

- `src/engine/atomic_write.cpp` (new), `src/cli/main.cpp`, `pipeline.cpp`, `daemon_lifecycle.cpp`, `index_persistence.cpp`, `semantic_cache.cpp`, `semantic_chunk_plan.cpp`, `semantic_orchestration.cpp`, `daemon_server.cpp`, `daemon_ops.cpp`, `launch_agent.cpp`.
- Tests: `atomic_write_test.cpp` (new), `cli_main_test.cpp` (new, drives the built `cgraph`), `daemon_lifecycle_test.cpp`. A failing write is simulated with `RLIMIT_FSIZE` in a child process (SIGXFSZ ignored, so `write()` fails with EFBIG part-way), which truncates a file mid-write as a full disk does and works unprivileged on Linux and macOS.
