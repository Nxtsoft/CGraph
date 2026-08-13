## Context

`graph_update` now publishes a deterministic code content root and graph reads can pin that root, but snippets are still reopened from the working tree after the immutable graph snapshot was selected. The exact bytes used by extraction are already hashed and persisted in the deterministic file manifest, yet the live `GraphSnapshot` does not retain the per-file hash ledger required to verify a later snippet read.

Semantic overlays have a separate trust gap. Cache records are keyed only by semantic-source content hash and fragment existence. After a deterministic rebuild removes or changes a referenced code node, replay rejects the old fragment for referential-integrity failure but leaves its cache record valid, increments a lifetime counter, and allows planning to skip the source indefinitely.

The implementation spans deterministic extraction/persistence, snippet-producing graph reads, semantic cache persistence, fragment replay, planning, and daemon status. Tests therefore define both boundaries before implementation and include a real daemon update/restart flow.

## Goals / Non-Goals

**Goals:**

- Bind every snippet returned by a content-root-pinned graph read to bytes recorded in the selected immutable snapshot.
- Fail a pinned read atomically when a required source file is missing, changed, or lacks snapshot evidence.
- Bind semantic cache validity to the fragment bytes and every external deterministic code node the fragment references.
- Invalidate and requeue only records whose semantic source, fragment, or code dependency changed.
- Make enrichment status describe current records and current pending work, and clear failures after successful replacement.
- Reproduce the same state after restart and preserve deterministic graph/export parity.

**Non-Goals:**

- Persisting historical source-file bodies or serving an older retained blob.
- Folding semantic documents, media, or memory into the deterministic code content root.
- Full-project hashing before every ordinary graph read.
- Invalidating all semantic fragments after every code-root change.
- Adding model/provider execution, a vector index, or a new retrieval surface.

## Decisions

### Store a snapshot-local source hash ledger

`GraphSnapshot` will carry an internal normalized-path-to-SHA-256 ledger. Deterministic rebuilds populate it from `IncrementalGraphIndex::cache`, whose hashes are corrected to the exact buffers parsed by extraction. Persisted fast-load reconstructs it from the content-verified index manifest. Semantic and memory overlay ingestion add hashes for their own source sidecars without changing the deterministic content root.

The ledger is runtime evidence, not graph topology. It will not enter graph JSON, graph IDs, exports, or Merkle computation. This preserves parity while making the selected immutable snapshot sufficient to verify query-visible source.

Alternative considered: place a source hash in every node property. Rejected because it duplicates file-level data across nodes and changes exported graph parity.

### Read, hash, and slice one exact buffer per file per request

Snippet-producing operations will use a request-local source reader. On first access to a path it reads the complete bounded source file once, hashes that exact buffer, compares it with the selected snapshot ledger, and caches the verified buffer for all snippets in the response. Slices are taken from the same bytes that were hashed, eliminating hash-then-reopen races and repeated hashing for dense same-file context.

When `expected_content_root` is present, a missing ledger entry, missing file, or hash mismatch aborts the operation with a synchronization error and no partial graph result. Unpinned reads retain the existing explicitly-eventual compatibility contract.

Alternative considered: retain historical source blobs. Rejected for this change because it adds storage lifecycle and garbage-collection policy; fail-closed synchronization already satisfies the trust contract.

### Persist semantic fragment and code-dependency fingerprints

Semantic cache schema v2 will identify a record by normalized source path plus source content hash and persist:

- fragment path and fragment SHA-256;
- state and last rejection reason;
- each external deterministic endpoint referenced by the fragment: node ID, normalized source path, and exact snapshot source hash.

Endpoints defined inside the same semantic fragment are not external dependencies. Unsupported or malformed cache schema is treated as no reusable cache and rebuilt from validated drops; there is no dual-schema compatibility path.

Alternative considered: depend on the whole deterministic content root. Rejected because any unrelated code edit would invalidate every semantic fragment.

### Reconcile before semantic replay

After a deterministic rebuild and before overlays are re-applied, reconciliation indexes the new graph by node ID and compares each current semantic record's fragment hash and dependency fingerprints. A missing node, changed source path/hash, changed fragment, or malformed dependency marks the affected record stale with an explicit reason. A drop is replayed only when all current source records mapped to it are dependency-valid; a new drop with no cache record is validated and ingested normally.

This implementation may scan the bounded semantic cache on a rebuild, but it changes state and requeues work only for affected records. A reverse dependency index is deferred until measured scale requires it.

### Derive health from current records

Planning will report current pending, stale, and failed inputs. The refresh worker replaces all three daemon counters from the plan/cache snapshot instead of incrementing a failure event forever. Failed ingestion writes failed current records for every manifest source. Successful ingestion atomically writes valid records with fresh fingerprints and clears the current failure for those sources.

Status precedence remains `running`, then `failed`, then `pending`/`stale`, then `idle`; counts remain independently visible.

### Test strategy

- Paired source tests cover cache v2 round-trip, duplicate-content source paths, fragment-hash changes, dependency reconciliation, current failure clearing, and request-local source verification.
- Existing graph parity/golden tests prove the runtime ledger does not alter exports.
- A real daemon test synchronizes, edits source without publishing a new graph, performs a pinned snippet read, and requires fail-closed behavior.
- A real semantic lifecycle test ingests a document-to-code edge, removes the code target, updates, observes precise stale/failed requeue state, ingests a replacement, restarts, and observes cleared stable state.

## Risks / Trade-offs

- [A context response can touch many large files] → read each file at most once per request, retain the existing extraction-size bound, and measure warmed context latency.
- [A code edit can invalidate a multi-source fragment] → treat the dropped fragment as the atomic overlay unit and requeue its mapped sources; never partially merge it.
- [Old cache data lacks dependency evidence] → reject schema v1 reuse and reconstruct v2 records from existing validated drops or new enrichment output.
- [Semantic or memory sidecars can change between validation and hashing] → hash the exact bytes used to validate/merge and publish their ledger entries in the same snapshot mutation.
- [Status can race planning and ingest] → continue using the dedicated enrichment mutex and replace counters from one cache/plan snapshot.

## Migration Plan

1. Bump semantic cache schema to v2 and deterministic index logic version if persisted snapshot shape requires it.
2. On first start, ignore v1 semantic cache reuse, validate present drop files against the verified deterministic graph, and write v2 records.
3. Existing deterministic manifests remain usable only if they contain complete per-file hashes; fast-load reconstructs the runtime ledger from them.
4. Rollback uses the previous binary, which ignores the v2 semantic cache as non-reusable and rebuilds safely; graph JSON remains unchanged.

## Open Questions

None. Historical source retention and reverse dependency indexing remain explicitly deferred until measured demand.
