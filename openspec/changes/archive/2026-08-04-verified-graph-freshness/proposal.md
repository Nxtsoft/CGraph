## Why

CGraph can currently reuse a cached file hash when size and modification time are unchanged, so an equal-length rewrite with a restored timestamp can leave the resident graph and persisted fast-load cache stale. Agents need an explicit, content-verified synchronization contract and a stable source identity they can pin before trusting graph results.

## What Changes

- Compute a deterministic, domain-separated SHA-256 Merkle root over normalized project-relative code paths and the exact per-file content hashes used by the graph build.
- Persist that content root with both the graph snapshot and index manifest, and reject fast-load unless the persisted roots match a newly content-verified source tree.
- Make the explicit graph update path a full-content verification barrier that never accepts a metadata-only cache hit, while still reusing extraction results whose content hash is unchanged.
- Return freshness metadata from synchronization, status, and graph read operations, and allow reads to require the content root returned by synchronization so a changed snapshot fails closed.
- Expose the verified synchronization contract through the MCP integration and teach the CGraph skill to synchronize before freshness-sensitive reads and after source edits.
- Add regression coverage for equal-length, preserved-mtime edits, deterministic root construction, persistence mismatch, pinned reads, and a real daemon sync/query/restart flow.
- Benchmark verified synchronization separately from ordinary low-latency reads.
- Non-goals: providing an atomic filesystem snapshot while unrelated writers continue mutating the tree, replacing the existing watcher with native OS events, or hashing the entire repository implicitly on every ordinary query.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `deterministic-graph-pipeline`: Define the canonical content-root calculation and require content-verified rescans to bind cache reuse to file hashes rather than metadata alone.
- `graph-daemon-client`: Add persisted source identity, a content-verified synchronization barrier, freshness metadata, and content-root-pinned reads.
- `host-integration-mcp`: Expose and document verified synchronization and root pinning for agent hosts.

## Impact

- Engine: file hashing/cache classification, incremental/full rescan, graph snapshot metadata, graph JSON persistence, index manifests, daemon request dispatch, and operation stats.
- Client/MCP: update/sync responses, graph read parameters and schemas, status output, and the CGraph host skill contract.
- On-disk cache: the index logic version changes; older graph/manifest pairs are rebuilt rather than fast-loaded.
- Performance: daemon startup and explicit synchronization read and hash all detected code inputs; ordinary query latency and watcher cadence remain unchanged.
