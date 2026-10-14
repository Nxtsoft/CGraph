# CGraph on Agent Retrieval Bench

First public, independently scorable retrieval number for CGraph. The benchmark is
[Agent Retrieval Bench](https://arxiv.org/abs/2607.24882) (ARB, MIT): 427 samples
across 25 repositories, scored with ARB's own metric code. Every sample's repository
is checked out at its base commit (ARB's released corpus is chunked and truncated, so
CGraph indexes the real tree).

## Gate (CGR-24)

Measured on all 345 positive samples, against ARB's published RepoMap row:

| Gate | Published RepoMap | CGraph | Result |
|---|---:|---:|---|
| BCY@8k >= 0.3788 | 0.3788 | **0.3862** | **pass** (+0.0074) |
| Recall@20 >= 0.6333 | 0.6333 | **0.5726** | **fail** (-0.0607) |

CGraph has the highest MRR of any row on the board (0.2553, against 0.2379 for
Qwen3-Embedding-4B) and leads `trace2code` by a wide margin, but it does not reach
RepoMap's Recall@20. The gap is a ranking-breadth problem, not a coverage one (see
[Where recall is lost](#where-recall-is-lost)). This gap is the input to the BM25
ticket.

## Pins

| What | Value |
|---|---|
| CGraph engine | release bin-v0.8.3, `cgraph --version` = `cgraph-native 0.1.0 (269048a)` (all 427 rows record `engine_revision` 269048a) |
| Harness | this directory at commit `ca00698` (branch `cgr-24-arb-adapter`, based on main `42d8501`) |
| ARB code | github.com/eyuansu62/agent-retrieval-bench @ `07014c986f3deadb1548c62b32c0ffbe6a81465d` |
| ARB data | huggingface.co/datasets/eyuansu71/agent_retrieval_bench @ `5901e1ee3aff048290db72edf9c63bc498b79ea3` (releases v2_code2test, v2_comment2context, v2_trace2code, v2_edit2ripple, v2_abstention; sha256 verified by ARB's `download-benchmark`) |
| Machine | Linux x86-64, CPU only, Python 3.13.5, `run.sh --jobs 4` |

## Reproduce

```sh
bench/arb/run.sh                       # installed cgraph release; work dir ~/.cache/cgraph-arb
bench/arb/run.sh --cgraph-bin-dir DIR  # a dev build: DIR holds cgraph, cgraph-client, graphd
```

The script clones ARB at the pinned commit, downloads and verifies the five data
releases, fetches the 271 repository snapshots, reruns the Lexical/BM25/RepoMap
baselines through ARB's own CLI, runs CGraph on all 427 samples, and writes
`runs/cgraph-<engine revision>/results.{json,md}`. Needs git, curl, zstd, tar and
Python >= 3.10; no GPU, no Docker, no model.

Two runs of the CGraph stage give identical per-sample rankings: a 30-sample rerun
(`--limit 6` over all five tasks) matched the full run's top files, metrics and per-op
lists on 30 of 30 samples. Two things had to be pinned for that:

- **Snapshot path.** `context` counts each entry's serialized JSON, including the
  absolute `source_file`, against its token budget, so a longer checkout path packs
  fewer entries (same query, same snapshot: 40 entries at an 89-character root, 38
  at a 147-character root). `run.sh` checks snapshots out under the fixed path
  `/tmp/cgraph-arb-snapshots`. An earlier run from a longer scratch path differed on
  181 of 427 top-20 lists and scored R@20 0.5763 / MRR 0.2563 / BCY@8k 0.3862. This
  is engine behaviour and is reported here, not changed.
- **Python hash seed for RepoMap.** ARB's RepoMap baseline iterates Python sets, so its
  ranking moves with `PYTHONHASHSEED` (edit2ripple R@20 0.5948 at seed 0, 0.5934 at
  seed 1). `run.sh` sets `PYTHONHASHSEED=0`.

## Pipeline check: reproducing the published baselines

The same `score.py` path (ARB `sample_metrics` + ARB `bcy_curve.evaluate_run`) applied
to baselines rerun through ARB's own CLI:

| Method | Published R@20 / MRR / BCY@8k | Rerun here |
|---|---|---|
| Lexical | 0.4940 / 0.1574 / 0.2650 | 0.4940 / 0.1574 / 0.2650 (exact) |
| BM25 | 0.4452 / 0.1520 / 0.2051 | 0.4452 / 0.1520 / 0.2051 (exact) |
| RepoMap | 0.6333 / 0.2158 / 0.3788 | 0.6311 / 0.2156 / 0.3846 |

Lexical and BM25 match to four decimals, including the per-budget `edit2ripple` BCY
values in the paper (Lexical 0.3218 / 0.4468 / 0.5187 / 0.5876 at 4k/8k/16k/32k).
RepoMap moves with the hash seed (above), and ARB's published core rows come from its
v1.x evaluation artifacts (`bcy_curve.py` `DEFAULT_RUNS` reads
`data/eval/v1_3_reviewed/repomap_all_files_details.jsonl`), so it lands within 0.006. The gate uses the published thresholds.

## Overall (345 positive samples)

| Method | Source | R@5 | R@10 | R@20 | MRR | BCY@4k | BCY@8k | BCY@16k | BCY@32k |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **CGraph** | this run | **0.3471** | 0.4546 | 0.5726 | **0.2553** | **0.2413** | **0.3862** | 0.4715 | 0.5594 |
| RepoMap | rerun here | 0.3192 | 0.4769 | 0.6311 | 0.2156 | 0.2034 | 0.3846 | 0.5279 | 0.6210 |
| Lexical | rerun here | 0.2292 | 0.3548 | 0.4940 | 0.1574 | 0.1488 | 0.2650 | 0.3886 | 0.4882 |
| BM25 | rerun here | 0.1899 | 0.3034 | 0.4452 | 0.1520 | 0.1220 | 0.2051 | 0.3184 | 0.4365 |
| Qwen3-Embedding-8B | ARB README | | | 0.7029 | 0.2336 | | 0.3732 | | |
| Qwen3-Embedding-4B | ARB README | | | 0.6306 | 0.2379 | | 0.3409 | | |
| pplx-embed-v1-4b | ARB README | | | 0.6072 | 0.2267 | | 0.3549 | | |
| RepoMap | ARB README | | | 0.6333 | 0.2158 | | 0.3788 | | |
| nomic-embed-code | ARB README | | | 0.5244 | 0.1986 | | 0.2781 | | |
| jina-code-embeddings-0.5b | ARB README | | | 0.4823 | 0.1914 | | 0.2783 | | |

Against the baselines rerun here, CGraph is ahead at the top of the ranking (R@5, MRR,
BCY at 4k and 8k) and behind from rank 10 on. RepoMap (0.6311) and both Qwen models
(0.7029, 0.6306) reach a higher Recall@20.

## Per task

| Task (n) | Method | R@5 | R@10 | R@20 | MRR | BCY@8k |
|---|---|---:|---:|---:|---:|---:|
| code2test (106) | **CGraph** | 0.1965 | 0.3255 | 0.4421 | 0.1488 | 0.2500 |
| | RepoMap | 0.2739 | 0.4013 | 0.5761 | 0.2013 | 0.3635 |
| | BM25 | 0.1305 | 0.2186 | 0.3066 | 0.1057 | 0.1541 |
| | Lexical | 0.0676 | 0.1399 | 0.2469 | 0.0663 | 0.0928 |
| comment2context (80) | **CGraph** | 0.1646 | 0.2979 | 0.4333 | 0.1901 | 0.2625 |
| | RepoMap | 0.1708 | 0.2562 | 0.4708 | 0.1527 | 0.2896 |
| | BM25 | 0.2146 | 0.4042 | 0.5292 | 0.1974 | 0.2604 |
| | Lexical | 0.1667 | 0.3479 | 0.4979 | 0.1530 | 0.2167 |
| trace2code (101) | **CGraph** | **0.6502** | **0.7376** | **0.8564** | **0.3856** | **0.6502** |
| | RepoMap | 0.4488 | 0.7046 | 0.8366 | 0.2706 | 0.5000 |
| | Lexical | 0.3432 | 0.4818 | 0.6964 | 0.2075 | 0.3795 |
| | BM25 | 0.2228 | 0.3218 | 0.4934 | 0.1638 | 0.1931 |
| edit2ripple (58) | **CGraph** | 0.3463 | 0.4138 | 0.5086 | **0.3130** | 0.3463 |
| | RepoMap | 0.3807 | 0.5230 | 0.5948 | 0.2330 | 0.3534 |
| | Lexical | 0.4124 | 0.5359 | 0.5876 | 0.2428 | 0.4468 |
| | BM25 | 0.2069 | 0.2874 | 0.4986 | 0.1536 | 0.2428 |

Against the paper's per-task tables:

- `trace2code` (paper table "trace2code task-level results"): best published is
  aider-style RepoMap at R@20 0.8366 / MRR 0.2742, then Qwen3-Embedding-8B at 0.7970 /
  0.1654. CGraph: 0.8564 / 0.3856.
- `edit2ripple` (paper table "edit2ripple retrieval results on 58 audited samples"):
  best published MRR is pplx-embed-v1-4b at 0.2877, best R@20 Qwen3-Embedding-4B at
  0.7112, best BCY@8k Qwen3-Embedding-8B at 0.5129. CGraph: MRR 0.3130, R@20 0.5086,
  BCY@8k 0.3463.
- `code2test`: CGraph is clearly behind RepoMap; the paper's winner here is
  Qwen3-Embedding-4B (MRR 0.3225).

## Selective retrieval (82 no-gold samples)

CGraph has no abstention signal. Every op returned files for every one of the 427
samples (fewest files in a fused ranking: 14), so it abstained on 0 of 50 natural and
0 of 32 counterfactual no-gold samples. Under ARB's selective-success@20 definition that
is the "never abstain" operating point: no-gold samples all count as failures and 229
of 345 positives count as successes (a gold file in the top 20). ARB reports the same
negative result for score thresholding with Lexical, BM25 and Jina.

## How the adapter queries CGraph

`cgraph_eval.py`, one fixed recipe for every sample, chosen before any score existed
and never read against the gold labels:

1. `cgraph --root <snapshot>` builds the graph; `cgraph-client` spawns `graphd` and
   waits for `build_state: ready`.
2. Query text = every string field of the sample's `query`, joined.
   `context {q, budget: 8000}` and `query {q, limit: 100}` each give a ranked node list.
   All 427 `query` calls took the `search` route (no single-symbol entity match).
3. Anchors the query itself names (`given_file`/`path` with `line`, `anchor_file`,
   `changed_file`, `implementation_files`, and repository paths quoted in the text,
   such as a trace frame `x_test.go:195`) resolve to the innermost symbol on that line,
   or to the file node. Each anchor gets `context {id, budget: 8000}`.
4. `edit2ripple`: the symbols overlapping the anchor diff's `@@ -a,b` base ranges get
   `impact {direction: dependents}`. `cgraph change-context` was not usable because it
   needs an applicable diff, and ARB's anchor diffs are excerpts: 30 of 58 end in
   `...[truncated]` and `git apply --check` rejects the first sample's as a corrupt
   patch. One of 58 samples resolved no changed symbol.
5. Each op's nodes collapse to files in first-hit order; the lists merge by reciprocal
   rank fusion (k = 60). Files the task already gives the agent (`comment2context`
   given file, `edit2ripple` anchor file) are removed. The ranking is restricted to
   ARB's official `all_files` candidate set, the released corpus snapshot every
   leaderboard method ranks.

BCY packs the stored top 20 files with ARB's canonical tokenizer against ARB's corpus
file texts, exactly as for the published rows. Median cost per snapshot: build 1.77 s
(max 106.9 s); retrieval per sample 0.144 s median, 2.88 s p95; graph size median
7,230 nodes (max 108,602).

## Where recall is lost

Of the 514 gold files in the 345 positive samples:

| Gold file | Count |
|---|---:|
| in CGraph's top 20 | 264 |
| ranked, but below 20 | 125 |
| a graph node, but no op reached it | 120 |
| not in the graph at all (`.json`, `.toml`, `.yml`, `.md`) | 5 |

Coverage is not the problem: 509 of 514 gold files are graph nodes. The losses are
breadth (120 never surfaced by any op) and ordering (125 surfaced too low).

Op ablation (`ablation.py`, re-fusing the per-op lists stored with each sample; lists
are cut at 50 files, so the "all ops" row approximates the scored 0.5726 / 0.2553):

| Ops fused | overall R@20 / MRR | code2test | comment2context | trace2code | edit2ripple |
|---|---:|---:|---:|---:|---:|
| context(q) only | 0.3548 / 0.1867 | 0.2406 / 0.1696 | 0.1833 / 0.1089 | 0.6403 / 0.2388 | 0.3032 / 0.2346 |
| query(q) only | 0.5842 / 0.2145 | 0.5726 / 0.2373 | 0.4062 / 0.1564 | 0.8086 / 0.2019 | 0.4598 / 0.2749 |
| context(q) + query(q) | 0.5308 / 0.2204 | 0.4343 / 0.2098 | 0.3521 / 0.1522 | 0.8234 / 0.2668 | 0.4440 / 0.2531 |
| anchor ops only | 0.4010 / 0.1524 | 0.1981 / 0.0390 | 0.3063 / 0.1016 | 0.6799 / 0.2492 | 0.4167 / 0.2610 |
| all ops (scored recipe) | 0.5769 / 0.2568 | 0.4531 / 0.1492 | 0.4333 / 0.1907 | 0.8564 / 0.3856 | 0.5144 / 0.3204 |

Read with care, since these are post-hoc on the test labels:

- `query` alone has the best Recall@20 (0.5842) and is the strongest single op on
  `code2test` (0.5726, level with RepoMap). Fusing `context(q)` into it costs recall
  there. `context` seeds from the top 3 lexical matches (`kFocalSeedCount = 3` in
  `daemon_ops.cpp`) and packs their neighbourhood, which plausibly misses tests for a
  PR description that names many symbols.
- The anchor ops carry the precision: they lift MRR from 0.2145 to 0.2568 and drive
  the `trace2code` lead.
- No op subset reaches RepoMap's 0.6311 Recall@20. CGraph's lexical `search` scores
  terms against symbol labels only (`lexical_matches` in `daemon_ops.cpp` tokenizes
  `node.label`); RepoMap and BM25 score file contents. A file-content first stage is
  what the BM25 ticket is for.
