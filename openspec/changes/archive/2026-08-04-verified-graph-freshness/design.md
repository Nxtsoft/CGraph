## Context

The resident daemon currently uses the same size-plus-mtime shortcut in three correctness-sensitive paths: watcher candidate detection, explicit/full rescans, and persisted-cache fast-load validation. If content changes without either metadata value changing, no hash is recomputed and the old graph may be served indefinitely. The manifest already stores per-file SHA-256 values, but there is no deterministic aggregate identity and graph reads cannot be pinned to the source snapshot an agent synchronized.

The daemon must retain low-latency ordinary reads. A Merkle root is only a summary of verified leaves; it cannot discover an un-hashed edit. Strong freshness therefore requires an explicit operation that reads and hashes every detected code input. Automatic watching remains an eventual convenience path and gains a stronger change hint, but it does not replace the verification barrier.

## Goals / Non-Goals

**Goals:**

- Detect equal-length content changes whose mtime was restored during explicit update, daemon startup, and normal local filesystem writes observed through a changed file identity/change-time token.
- Give every graph snapshot a deterministic identity derived from normalized project-relative paths and SHA-256 file-content hashes.
- Make `graph_update` a blocking, content-verified synchronization barrier and return its content root.
- Let graph reads require the root returned by synchronization and fail closed if the daemon has published a different snapshot.
- Preserve extraction reuse: full verification hashes every code file but re-parses only files whose content hash changed.
- Preserve ordinary query latency and Graphify node-link compatibility.

**Non-Goals:**

- Claiming a globally atomic repository view while unrelated processes keep writing during verification.
- Full-hashing the project implicitly before every ordinary query.
- Replacing the polling watcher with FSEvents, inotify, or another native event subsystem.
- Extending the source root to host-generated semantic fragments, media, or session-memory sidecars in this change.
- Changing node, edge, ID-normalization, or graph-analysis output.

## Decisions

### 1. Canonical source identity is a domain-separated SHA-256 Merkle root

Each detected code file contributes one leaf bound to its normalized project-relative path:

```text
leaf = SHA256(0x00 || length(path) || path || file_sha256_bytes)
node = SHA256(0x01 || left_digest || right_digest)
empty = SHA256(0x02)
```

Leaves are sorted lexicographically by normalized relative path. An odd node is promoted unchanged to the next level. The persisted identity includes algorithm (`sha256-merkle-v1`), root hex, and leaf count.

Path binding makes rename/add/delete operations change the root even when bytes are identical. Domain separation and length-prefixing avoid ambiguous concatenations. A flat hash of concatenated manifest JSON was rejected because JSON formatting would become part of the identity and it would not define reusable leaf/node semantics.

### 2. Content verification is an explicit cache-classification mode

`classify_cached_file` gains an explicit metadata or content validation mode. Metadata mode retains the existing stat optimization for non-authoritative advisory uses. Content mode always reads SHA-256 and may return a hash hit, stale file, new file, or deletion; it never returns a stat hit.

Full code rescans, explicit update, watcher-delivered code events, and persisted fast-load validation use content mode. This standardizes correctness at the cache source rather than adding special-case rehashes in consumers. Unchanged files remain extraction cache hits because their freshly computed hash matches the prior hash.

### 3. The existing update operation becomes the synchronization barrier

The wire protocol keeps `update`; `graph_update` is documented as content-verified synchronization rather than adding a second operation with duplicate ownership. Its response includes the root, leaf count, files hashed, files re-extracted, and graph size.

Ordinary watcher-driven reads stay fast and eventual. Trusted agent flow is:

```text
graph_update -> receive content_root -> graph read(expected_content_root)
```

Every query/path/explain/impact/context result also self-describes the snapshot root. If `expected_content_root` is supplied and differs from the immutable snapshot selected for the request, the daemon returns an error without serving graph data. Root pinning was chosen over a process-local integer revision because it survives restart and directly identifies source content.

Automatic synchronization before every MCP read was rejected because verification is O(total indexed source bytes) and the current SHA implementation reads file content. Hiding that unbounded cost behind nominally low-latency tools would break their performance contract.

### 4. Snapshot and manifest carry the root without changing graph node-link output

`GraphSnapshot` carries the root in memory. `IndexManifest` persists it and bumps the index logic version. Startup hashes the complete detected code tree and accepts a fast-load only when file set, individual hashes, and recomputed root equal the manifest.

The root is not added to exported node-link JSON in this change, preserving byte-level Graphify parity. On fast-load, the daemon assigns the freshly verified manifest root to the loaded immutable snapshot before publication. The existing write order (atomic graph write, then atomic manifest write) means a crash between files leaves an old manifest that fails source verification and forces rebuild.

### 5. Watcher metadata gains a POSIX file-change token, but remains advisory

The polling watcher records device, inode, and nanosecond change time in addition to size and mtime on macOS and Linux. A normal in-place rewrite with restored mtime changes ctime; an atomic replacement also changes inode. A watcher event then forces content validation before extraction.

This closes the reproduced local edit path without hashing the entire tree every two seconds. It is still described as a change hint, not cryptographic proof; explicit update is the only operation that claims a full content verification.

### 6. Verification is test-first and measured separately

Regression tests first reproduce equal-length/restored-mtime reuse, then assert:

- content mode changes the leaf/root and re-extracts the file;
- the watcher observes the normal POSIX rewrite through its change token;
- persisted fast-load rejects the stale root;
- Merkle roots are deterministic across input ordering and change on path/content/file-set changes;
- a real daemon update returns a new root, a pinned query returns the new symbol, and an old-root query fails;
- MCP schemas forward `expected_content_root` and describe `graph_update` as verified synchronization.

The full default and sanitizer suites remain required. Verification performance is measured as files, bytes, and wall time on the repository fixture; ordinary warmed query latency is checked separately to prove it did not inherit the full-hash cost.

## Risks / Trade-offs

- **Risk: startup and explicit update read every detected code byte** → Keep extraction reuse hash-based, report verification work, and benchmark verification separately from reads.
- **Risk: files mutate during a verification pass** → The contract is explicitly “verified against bytes read by the barrier.” Reused extractions are matched to freshly verified hashes, while changed/new extractions return the SHA-256 of the exact in-memory source buffer parsed and replace the earlier classification hash before root publication. A future filesystem-snapshot integration can strengthen cross-file atomicity without changing root format.
- **Risk: POSIX change-time fields differ by platform** → Encapsulate the platform spelling in one file-cache primitive and run the existing Ubuntu/macOS CI matrix; watcher correctness never upgrades the explicit verification contract.
- **Risk: old manifests lack a root** → Bump the index logic version and rebuild; do not infer or synthesize a root for old cache files.
- **Risk: adding freshness fields changes response fixtures** → Add fields only to daemon result objects and MCP schemas, leaving node-link exports untouched; update exact response tests deliberately.
- **Risk: agents forget to synchronize** → Update the bundled CGraph skill so freshness-sensitive navigation starts with `graph_update` and pins subsequent reads to its root. Unpinned responses remain self-describing rather than falsely claiming current-worktree proof.

## Migration Plan

1. Ship the new binary and MCP schema with the bumped index logic version.
2. The first daemon start content-verifies source files; old manifests miss the new version/root and rebuild once.
3. Existing `graph_update` callers continue working and receive additional freshness fields.
4. Updated host skills use the returned root for pinned reads. Older unpinned clients remain valid but receive self-describing roots.
5. Rollback to the prior binary treats the newer manifest version as incompatible and rebuilds using its existing rules.

## Open Questions

- None for implementation. Atomic multi-file filesystem snapshots and semantic-input roots are separate future capabilities.
