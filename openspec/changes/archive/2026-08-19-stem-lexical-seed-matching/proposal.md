## Why

The lexical fallback matches query subtokens against label subtokens exactly, so a query
inflection never reaches the identifier it names: "packing" cannot match `pack_context`,
"resolves" cannot match `resolve_imports`. Measured on the committed fixture, this caps the
share of rows whose grade-2 anchor is lexically reachable at all at 0.733; light stemming
raises that bound to 0.747 and, through the real e2e gate, lifts recall at every budget
(research/beyond-lexical). The sibling candidate from the same experiment — a file-level
seed — was rejected by the gate (pool dilution, below baseline at 6000/8000) and is parked,
not shipped.

The observable contract tests verify: an inflected query term resolves the identifier it
names; an off-topic query still resolves nothing; the e2e gate meets the new, higher pins.

## What Changes

- `lexical_matches` stems both query terms and label terms with a one-shot suffix strip
  (ing/tion/sion/ers/ies/ed/es/al/er/s; the stem must keep ≥ 4 chars so short roots stay
  exact), and computes document frequencies over stems. Deliberately light: single strip,
  fixed list; doubled-consonant gerunds ("running" → "runn") are accepted misses rather than
  reasons for a heavier stemmer.
- Scope: seed ranking only. `lexical_terms` itself, the substring pre-pass, the adaptive
  gather gate, and the knapsack value keep exact-match semantics.
- e2e gate re-pinned to the measured post-change values in the same change.
- Non-goals: file-level seeds (measured and rejected — recorded in
  research/beyond-lexical/results.md so the mechanism is not re-tried without a
  pool-cost-aware design); language-aware or dictionary stemming; changing the off-topic
  zero-hit contract.

## Measured effect (committed fixture, 1580 nodes / 3178 links, N=75, real e2e gate)

| budget | before | after | delta |
| --- | --- | --- | --- |
| 2000 | 0.2195 | 0.2487 | +0.029 |
| 4000 | 0.3404 | 0.3629 | +0.023 |
| 6000 (default) | 0.3865 | 0.3927 | +0.006 |
| 8000 | 0.4226 | 0.4425 | +0.020 |

Both engines measured against the identical final tree (the gate slices live worktree bytes
for snippets, so recall is tree-state-sensitive; the old engine was compiled, then the file
restored, before running). The 6000 delta sits within the gate's 0.03 tolerance — the
enforcing red/green for this change is the daemon_ops stemmed-resolution test, which fails
hard against the pre-change engine.

## Impact

- `src/engine/daemon_ops.cpp` — `stem_term`/`stem_terms` + stemmed scoring in
  `lexical_matches`.
- `tests/smoke/daemon_ops_test.cpp` — stemmed-resolution and off-topic regression block.
- `tests/smoke/retrieval_quality_test.cpp` — re-pinned baselines.
- Capability: `graph-daemon-client`. Exports, parity surfaces, and the focal-injected parity
  gate are untouched.
