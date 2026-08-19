# Add: content root in one-shot builds and graph.json exports

## Why
The daemon publishes `freshness.content_root` (sha256-merkle-v1) and reads can pin it, but
one-shot builds computed no root and `graph.json` never carried one — so downstream consumers
of the export (CI test selection, provenance records) had only mtime heuristics. blastline's
freshness pinning needs the persisted artifact to name the exact tree it was built from.

## What Changes
1. `run_one_shot` computes the content root from the per-file hashes extraction already
   produced (root canonicalized — on macOS /var vs /private/var made every leaf look
   out-of-project and threw).
2. `to_node_link_json` embeds `graph.content_root {algorithm, sha256, leaf_count}` when
   valid (never fabricated); `parse_node_link_graph` round-trips it. Daemon-persisted
   exports gain it for free from the snapshot.
3. New smoke test `content_root_export_test`: valid root on one-shot, determinism (same
   tree → same root), round-trip identity, and sensitivity (edit → different root).
4. README: repository moved to Nxtsoft/CGraph — self-referencing links updated.

## Impact
- Verified end-to-end with blastline: `--daemon-verify` matches a live daemon's root to the
  embedded root (subset with provenance), and an edited tree fails open with both roots
  stated. 70/70 smoke tests.
- **Touches:** `pipeline.cpp`, `export_json.cpp`, `tests/smoke/{content_root_export_test.cpp,CMakeLists.txt}`, `README.md`.

## Capabilities
### Modified Capabilities
- `deterministic-graph-pipeline` — content-root provenance in one-shot exports.
