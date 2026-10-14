# CGraph on Agent Retrieval Bench

This is the first CGraph retrieval score on a public benchmark that others can rerun.
The benchmark is [Agent Retrieval Bench](https://arxiv.org/abs/2607.24882) (ARB, MIT):
427 samples across 25 repositories, scored with ARB's own metric code. Every sample's
repository is checked out at its base commit, because ARB's released corpus is chunked
and truncated and CGraph has to index the real tree.

## Gate (CGR-24)

All 345 positive samples:

| Gate | Published RepoMap | RepoMap rerun on this pipeline | CGraph | Result |
|---|---:|---:|---:|---|
| BCY@8k >= 0.3788 | 0.3788 | 0.3846 | 0.3790 | Meets the published threshold by 0.0002, which is inside run-to-run noise (see below). Below the RepoMap rerun by 0.0056. Not a meaningful pass. |
| Recall@20 >= 0.6333 | 0.6333 | 0.6311 | 0.5726 | Fails, by 0.0607 against the published threshold. |

On BCY@8k, CGraph and RepoMap are level. On Recall@20, CGraph is clearly behind.
CGraph's MRR (0.2356) is above RepoMap's (0.2158 published, 0.2156 rerun) and below
Qwen3-Embedding-4B's (0.2379). The recall gap comes from ranking breadth, not graph
coverage (see [Where recall is lost](#where-recall-is-lost)). It is the input to the
BM25 ticket (CGR-25).

**Noise floor.** Here is how much the same method moves between runs of this pipeline:

- Checking CGraph's snapshots out at a different path changed R@20 by 0.0037, MRR by
  0.0010 and BCY@8k by less than 0.0001 (see [Reproduce](#reproduce)).
- Three RepoMap reruns with different Python hash seeds gave R@20 between 0.6256 and
  0.6311, MRR between 0.2106 and 0.2156, and BCY@8k between 0.3784 and 0.3846.

Differences smaller than about 0.006 on these metrics should not be read as a win or a
loss.

## Pins

| What | Value |
|---|---|
| CGraph engine | Release bin-v0.8.3. `cgraph --version` reports `cgraph-native 0.1.0 (269048a)`, and all 427 rows record `engine_revision` 269048a. |
| Harness | `bench/arb` at commit `c1520dc` (branch `cgr-24-arb-adapter`, based on main `42d8501`); the scored run used exactly this code. |
| ARB code | github.com/eyuansu62/agent-retrieval-bench @ `07014c986f3deadb1548c62b32c0ffbe6a81465d` |
| ARB data | huggingface.co/datasets/eyuansu71/agent_retrieval_bench @ `5901e1ee3aff048290db72edf9c63bc498b79ea3`. Releases: v2_code2test, v2_comment2context, v2_trace2code, v2_edit2ripple, v2_abstention. ARB's `download-benchmark` verifies their sha256. |
| Machine | Linux x86-64, CPU only, Python 3.13.5, `run.sh --jobs 4` |

## Reproduce

```sh
bench/arb/run.sh                       # installed cgraph release; work dir ~/.cache/cgraph-arb
bench/arb/run.sh --cgraph-bin-dir DIR  # a dev build: DIR holds cgraph, cgraph-client, graphd
```

The script:

1. clones ARB at the pinned commit;
2. downloads the five data releases and verifies them;
3. fetches the 271 repository snapshots;
4. reruns the Lexical, BM25 and RepoMap baselines through ARB's own CLI;
5. runs CGraph on all 427 samples;
6. writes `runs/cgraph-<engine revision>/results.{json,md}`.

It needs git, curl, zstd, tar and Python >= 3.10. It needs no GPU, no Docker and no
model.

I reran 30 samples, covering all five tasks. They matched the full run's top files,
metrics and per-op lists on 30 of 30 samples. Two things had to be pinned to get that:

- **Snapshot path.** `context` counts each entry's serialized JSON against its token
  budget, and that JSON includes the absolute `source_file` path. A longer checkout
  path therefore packs fewer entries. With the same query and the same snapshot, an
  89-character root packed 40 entries and a 147-character root packed 38. `run.sh`
  checks snapshots out under the fixed path `/tmp/cgraph-arb-snapshots`. This is engine
  behaviour; it is reported here and not changed.
- **Python hash seed for RepoMap.** ARB's RepoMap baseline iterates Python sets, so its
  ranking moves with `PYTHONHASHSEED`. On edit2ripple, R@20 was 0.5948 at seed 0 and
  0.5934 at seed 1. `run.sh` sets `PYTHONHASHSEED=0`.

## Pipeline check: reproducing the published baselines

I applied the same `score.py` path (ARB `sample_metrics` plus ARB
`bcy_curve.evaluate_run`) to baselines rerun through ARB's own CLI:

| Method | Published R@20 / MRR / BCY@8k | Rerun here |
|---|---|---|
| Lexical | 0.4940 / 0.1574 / 0.2650 | 0.4940 / 0.1574 / 0.2650 |
| BM25 | 0.4452 / 0.1520 / 0.2051 | 0.4452 / 0.1520 / 0.2051 |
| RepoMap | 0.6333 / 0.2158 / 0.3788 | 0.6311 / 0.2156 / 0.3846 |

Lexical and BM25 match to four decimals. That includes the paper's per-budget
edit2ripple BCY for Lexical: 0.3218, 0.4468, 0.5187 and 0.5876 at 4k, 8k, 16k and 32k.
RepoMap does not match exactly, for two reasons. Its ranking moves with the hash seed
(see above). Also, ARB's published core rows come from its v1.x evaluation artifacts:
`bcy_curve.py` `DEFAULT_RUNS` reads
`data/eval/v1_3_reviewed/repomap_all_files_details.jsonl`.

## Overall (345 positive samples)

| Method | Source | R@5 | R@10 | R@20 | MRR | BCY@4k | BCY@8k | BCY@16k | BCY@32k |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CGraph | this run | 0.3399 | 0.4435 | 0.5726 | 0.2356 | 0.2292 | 0.3790 | 0.4681 | 0.5594 |
| RepoMap | rerun here | 0.3192 | 0.4769 | 0.6311 | 0.2156 | 0.2034 | 0.3846 | 0.5279 | 0.6210 |
| Lexical | rerun here | 0.2292 | 0.3548 | 0.4940 | 0.1574 | 0.1488 | 0.2650 | 0.3886 | 0.4882 |
| BM25 | rerun here | 0.1899 | 0.3034 | 0.4452 | 0.1520 | 0.1220 | 0.2051 | 0.3184 | 0.4365 |
| Qwen3-Embedding-8B | ARB README | | | 0.7029 | 0.2336 | | 0.3732 | | |
| Qwen3-Embedding-4B | ARB README | | | 0.6306 | 0.2379 | | 0.3409 | | |
| pplx-embed-v1-4b | ARB README | | | 0.6072 | 0.2267 | | 0.3549 | | |
| RepoMap | ARB README | | | 0.6333 | 0.2158 | | 0.3788 | | |
| nomic-embed-code | ARB README | | | 0.5244 | 0.1986 | | 0.2781 | | |
| jina-code-embeddings-0.5b | ARB README | | | 0.4823 | 0.1914 | | 0.2783 | | |

The paper also reports a post-hoc fusion, RRF of Qwen3-Embedding-8B and RepoMap
(paper tables "Post-hoc RRF fusion" and "Positive-core context-budget"). On the
287-sample positive core, which excludes edit2ripple, the fusion scores R@20 0.7331,
MRR 0.2713 and BCY@8k 0.4884. It is ahead of CGraph on all three metrics. The sample
sets differ (287 vs 345), so these numbers are a reference rather than a like-for-like
row.

Compared with RepoMap rerun on the same pipeline, CGraph is higher on R@5 (+0.021),
MRR (+0.020) and BCY@4k (+0.026). It is level on BCY@8k (-0.006, the size of the noise
floor). It is lower from rank 10 on: R@10 -0.033, R@20 -0.059, BCY@16k -0.060.

## Per task

| Task (n) | Method | R@5 | R@10 | R@20 | MRR | BCY@8k |
|---|---|---:|---:|---:|---:|---:|
| code2test (106) | CGraph | 0.1965 | 0.3255 | 0.4421 | 0.1488 | 0.2500 |
| | RepoMap | 0.2739 | 0.4013 | 0.5761 | 0.2013 | 0.3635 |
| | BM25 | 0.1305 | 0.2186 | 0.3066 | 0.1057 | 0.1541 |
| | Lexical | 0.0676 | 0.1399 | 0.2469 | 0.0663 | 0.0928 |
| comment2context (80) | CGraph | 0.1458 | 0.2562 | 0.4333 | 0.1557 | 0.2437 |
| | RepoMap | 0.1708 | 0.2562 | 0.4708 | 0.1527 | 0.2896 |
| | BM25 | 0.2146 | 0.4042 | 0.5292 | 0.1974 | 0.2604 |
| | Lexical | 0.1667 | 0.3479 | 0.4979 | 0.1530 | 0.2167 |
| trace2code (101) | CGraph | 0.6502 | 0.7376 | 0.8564 | 0.3856 | 0.6502 |
| | RepoMap | 0.4488 | 0.7046 | 0.8366 | 0.2706 | 0.5000 |
| | Lexical | 0.3432 | 0.4818 | 0.6964 | 0.2075 | 0.3795 |
| | BM25 | 0.2228 | 0.3218 | 0.4934 | 0.1638 | 0.1931 |
| edit2ripple (58) | CGraph | 0.3290 | 0.4052 | 0.5086 | 0.2434 | 0.3290 |
| | RepoMap | 0.3807 | 0.5230 | 0.5948 | 0.2330 | 0.3534 |
| | Lexical | 0.4124 | 0.5359 | 0.5876 | 0.2428 | 0.4468 |
| | BM25 | 0.2069 | 0.2874 | 0.4986 | 0.1536 | 0.2428 |

How each task compares with the paper's published rows:

- **trace2code.** CGraph's MRR is 0.3856. The published rows are RepoMap 0.2742 (the
  best single method), the RRF Qwen3-8B + RepoMap fusion 0.2672, and Qwen3-Embedding-8B
  0.1654. On R@20, CGraph's 0.8564 is above RepoMap's 0.8366 and below the fusion's
  0.8795.
- **edit2ripple.** CGraph does not lead on any metric. Its MRR of 0.2434 is level with
  Lexical (0.2428) and below pplx-embed-v1-4b (0.2877). Its R@20 of 0.5086 is below the
  rerun RepoMap and Lexical rows above and below Qwen3-Embedding-4B (0.7112). Its
  BCY@8k of 0.3290 is below Lexical (0.4468) and Qwen3-Embedding-8B (0.5129).
- **code2test.** CGraph is behind RepoMap on every metric. The paper's best MRR here is
  Qwen3-Embedding-4B at 0.3225.
- **comment2context.** CGraph is below BM25 and Lexical on R@20 and below BM25 on MRR.

## Given and anchor files

In comment2context, the query names a `given_file`; in edit2ripple, it names an
`anchor_file`. These files are never gold. ARB's evaluators keep them in the ranking:
RepoMap only down-weights the given file, and Lexical and BM25 do nothing special.
CGraph's results keep them too, so the scores above leave them in place.

The `score.py` output also includes a diagnostic table that removes the given or anchor
file from every method's stored top 20. This is not the ARB protocol. The same rule
applies to every row. Recall@20 is a lower bound and MRR only sees ranks 1-20.

| Method | overall R@20 / MRR@20 / BCY@8k | comment2context | edit2ripple |
|---|---|---|---|
| CGraph | 0.5726 / 0.2498 / 0.3862 | 0.4333 / 0.1849 / 0.2625 | 0.5086 / 0.3077 / 0.3463 |
| RepoMap | 0.6311 / 0.2350 / 0.4006 | 0.4708 / 0.1739 / 0.3229 | 0.5948 / 0.3380 / 0.4023 |
| Lexical | 0.4940 / 0.1987 / 0.2775 | 0.4979 / 0.2273 / 0.2542 | 0.5876 / 0.4253 / 0.4698 |
| BM25 | 0.4452 / 0.1761 / 0.2133 | 0.5292 / 0.2790 / 0.2833 | 0.4986 / 0.2231 / 0.2601 |

With the same exclusion applied to every method, RepoMap's BCY@8k (0.4006) is ahead of
CGraph's (0.3862).

## Selective retrieval (82 no-gold samples)

CGraph has no abstention signal. Every op returned files for every one of the 427
samples; the shortest fused ranking held 14 files. So CGraph abstained on 0 of 50
natural and 0 of 32 counterfactual no-gold samples.

Under ARB's selective-success@20 definition, this is the "never abstain" operating
point. All no-gold samples count as failures, and 229 of 345 positives count as
successes (a gold file in the top 20). ARB reports the same negative result for score
thresholding with Lexical, BM25 and Jina.

## How the adapter queries CGraph

`cgraph_eval.py` applies one fixed recipe to every sample. It reads only the query,
never the gold labels:

1. `cgraph --root <snapshot>` builds the graph. `cgraph-client` then spawns `graphd`
   and waits for `build_state: ready`.
2. The query text is every string field of the sample's `query`, joined.
   `context {q, budget: 8000}` and `query {q, limit: 100}` each give a ranked node list.
   All 427 `query` calls took the `search` route; none hit a single-symbol entity match.
3. Some files are named by the query itself: `given_file` or `path` with `line`,
   `anchor_file`, `changed_file`, `implementation_files`, and repository paths quoted
   in the text (such as a trace frame `x_test.go:195`). Each one resolves to the
   innermost symbol on that line, or to the file node. Each anchor gets
   `context {id, budget: 8000}`.
4. For edit2ripple, the symbols overlapping the anchor diff's `@@ -a,b` base ranges get
   `impact {direction: dependents}`. `cgraph change-context` was not usable here,
   because it needs a diff that applies and ARB's anchor diffs are excerpts. 30 of 58
   end in `...[truncated]`, and `git apply --check` rejects the first sample's diff as
   a corrupt patch. One of the 58 samples resolved no changed symbol.
5. Each op's nodes collapse to files in first-hit order. The lists then merge by
   reciprocal rank fusion (k = 60). The ranking is restricted to ARB's official
   `all_files` candidate set, which is the released corpus snapshot that every
   leaderboard method ranks.

BCY packs the stored top 20 files with ARB's canonical tokenizer against ARB's corpus
file texts, exactly as for the published rows.

Cost per snapshot:

| Measure | Median | Worst |
|---|---:|---:|
| Graph build | 1.68 s | 101.3 s (max) |
| Retrieval per sample | 0.151 s | 2.69 s (p95) |

## Where recall is lost

Of the 514 gold files in the 345 positive samples:

| Gold file | Count |
|---|---:|
| in CGraph's top 20 | 264 |
| ranked, but below 20 | 125 |
| a graph node, but no op reached it | 120 |
| not in the graph at all (`.json`, `.toml`, `.yml`, `.md`) | 5 |

Coverage is not the problem: 509 of 514 gold files are graph nodes. The losses come
from breadth (120 files never surfaced by any op) and from ordering (125 surfaced too
low).

The op ablation below comes from `ablation.py`, which re-fuses the per-op lists stored
with each sample. The stored lists are cut at 50 files, so the "all ops" row only
approximates the scored 0.5726 / 0.2356.

| Ops fused | overall R@20 / MRR | code2test | comment2context | trace2code | edit2ripple |
|---|---:|---:|---:|---:|---:|
| context(q) only | 0.3548 / 0.1817 | 0.2406 / 0.1696 | 0.1833 / 0.0982 | 0.6403 / 0.2388 | 0.3032 / 0.2194 |
| query(q) only | 0.5842 / 0.2083 | 0.5726 / 0.2373 | 0.4062 / 0.1406 | 0.8086 / 0.2019 | 0.4598 / 0.2595 |
| context(q) + query(q) | 0.5293 / 0.2136 | 0.4343 / 0.2098 | 0.3521 / 0.1335 | 0.8234 / 0.2668 | 0.4353 / 0.2384 |
| anchor ops only | 0.4010 / 0.1301 | 0.1981 / 0.0390 | 0.3063 / 0.0756 | 0.6799 / 0.2492 | 0.4167 / 0.1643 |
| all ops (scored recipe) | 0.5759 / 0.2361 | 0.4531 / 0.1492 | 0.4333 / 0.1561 | 0.8564 / 0.3856 | 0.5086 / 0.2448 |

These numbers were computed post hoc against the test labels, so read them with care:

- **`query` alone.** It has the best Recall@20 of any op subset (0.5842), and on
  code2test (0.5726) it is level with RepoMap. Fusing `context(q)` into it lowers
  recall there. `context` seeds from the top 3 lexical matches (`kFocalSeedCount = 3`
  in `daemon_ops.cpp`) and packs their neighbourhood. That plausibly misses tests for
  a PR description that names many symbols.
- **Anchor ops.** Most of the trace2code result comes from these. They raise
  trace2code MRR from 0.2668 (text ops only) to 0.3856.
- **The RepoMap gap.** No op subset reaches RepoMap's 0.6311 Recall@20. CGraph's lexical
  `search` scores terms against symbol labels only: `lexical_matches` in
  `daemon_ops.cpp` tokenizes `node.label`. RepoMap and BM25 score file contents. A
  first stage that scores file contents is what the BM25 ticket is for.
