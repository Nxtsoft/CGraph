## Why

Free-text focal resolution is the largest measured recall lever left in the query path. On the
committed fixture, the end-to-end q-only gate measures 0.280 grade-2 recall at the default
budget while the focal-injected ceiling is 0.530 — resolution forfeits ~47% of what the packer
can deliver. The prior experiment (research/focal-resolution) shipped the lexical fallback and
multi-seed gather and parked the remainder: "closing it needs better focal ranking."

The successor experiment (research/focal-ranking) found the cause and the fix: the lexical
fallback scores every query term equally, so ubiquitous words dominate the seed set and rare
identifiers — the terms that actually name the code the query is about — get outranked.
Weighting terms by inverse document frequency over node-label subtokens, measured through the
real daemon path via the committed e2e gate, lifts recall at every budget; and with good
ranking, a tighter 3-seed gather beats the 5-seed one (k=3 under the OLD ranking is a wash —
pool quality, not pool size, is the lever, mirroring the packer-regression lesson).

The observable contract tests verify: a rare query term outranks a ubiquitous one in seed
selection, and the end-to-end gate meets the new, higher pinned baselines.

## What Changes

- `lexical_matches` ranks by **idf-weighted overlap**: each query term is worth
  `log(1 + N/(1+df))` with df = document frequency over node-label subtokens; node term sets
  are computed once per node in the same pass that builds df (one O(nodes) scan, the same
  shape as `index_nodes`/`matching_nodes` on this read path). Determinism unchanged (ties by
  centrality then label); off-topic queries still yield no match (a term absent from every
  label matches no node and only deflates all scores equally).
- `kFocalSeedCount` 5 → 3, valid only together with the idf ranking (measured attribution:
  k=3 alone is a wash; k=10 loses outright).
- The multi-seed union test asserts the idf ordering (the rare-term match becomes the focus)
  and checks the union through focus-or-included.
- The e2e gate re-pins to the measured post-change values in the same change.
- Non-goals: non-lexical (semantic/embedding) ranking — the measured lexical hit ceiling is
  0.733 of rows and the binary has a no-model constraint; changing `matching_nodes`,
  `query_term_overlap` (gather gate / knapsack value), or the query op's routing grammar.

## Measured effect (committed fixture, 1580 nodes / 3178 links, N=75, real e2e gate)

| budget | before | after | delta |
| --- | --- | --- | --- |
| 2000 | 0.1804 | 0.2176 | +0.037 |
| 4000 | 0.2330 | 0.3404 | +0.107 |
| 6000 (default) | 0.2795 | 0.3865 | +0.107 |
| 8000 | 0.2936 | 0.4245 | +0.131 |

73% of the focal-injected ceiling at 6000, up from 53%.

## Impact

- `src/engine/daemon_ops.cpp` — `lexical_matches` scoring, `kFocalSeedCount`.
- `tests/smoke/daemon_ops_test.cpp` — multi-seed/idf-ordering assertions.
- `tests/smoke/retrieval_quality_test.cpp` — re-pinned baselines.
- Capability: `graph-daemon-client` (lexical focal-resolution requirement). Graph exports,
  parity surfaces, and the parity gate's focal-injected path are untouched.
