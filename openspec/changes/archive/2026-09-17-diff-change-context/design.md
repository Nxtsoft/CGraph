## Context

Source IDs embed checkout paths and graph exports omit the runtime source-hash ledger. An old exported graph pointed at modified target files cannot supply verified deleted source.

## Goals / Non-Goals

Provide source evidence for a supplied diff, directional dependents, explicit uncertainty and one globally bounded response. Do not prove a merge safe, infer arbitrary symbol renames, perform Git checkout operations, or mutate either supplied tree.

## Decisions

Build the base and target in memory with the canonical one-shot pipeline. Validate exact unified diff replay and new positions, reject path escapes, and reverify both source inventories with fresh source readers before returning. Keep snapshot-local IDs; prefix IDs only inside the temporary union used by the canonical context packer, then restore original IDs with explicit side labels in the response.

Use canonical multi-source impact traversal with predecessor edges and originating changed IDs. Pack context once over the union, then bound the complete serialized JSON envelope. The budget unit is ceil(UTF-8 bytes / 4), an explicit approximation rather than a model tokenizer. Retain mandatory source/diff metadata or return a clear insufficient-budget error; report omitted impact/context entries.

Map edited ranges on each side, including insertion/deletion boundaries. File nodes cover changed lines outside extracted symbols. Pair symbols only when the qualified owner/name/kind is unique in its file; report remaining added/deleted-or-renamed ambiguity explicitly. File rename is based on diff paths and checked root existence.

A supplied diff may deliberately cover a subset of root changes. Report concrete unassessed detected-source paths and counts; never claim whole-root coverage from a partial patch.

MCP executes this explicit-root operation directly in its server process; it does not start or mutate a resident daemon. Existing daemon navigation keeps its current behavior.

## Risks / Trade-offs

Two cold builds cost more than a warm navigation call. Unresolved calls are aggregate counts, not per-change diagnostics. Grammar coverage and dynamic dispatch remain bounded by the engine. Binary patches, mode-only sections and quoted Git paths are rejected explicitly. Final verification detects edits observed before return but does not lock external files against future changes.
