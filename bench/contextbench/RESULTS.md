# CGraph on ContextBench Lite: file-level precision and recall

This measures item 2 of the CGraph build plan, **"ContextBench file precision >= 0.5"**.
[ContextBench](https://arxiv.org/abs/2602.05892) (v3, Apache-2.0) gives each issue a
human-annotated gold context. This bench scores the files CGraph retrieves for the issue
against that gold, using ContextBench's own metric code. Every instance's repository is
checked out at its base commit, and the real `cgraph`, `graphd` and `cgraph-client`
binaries index and serve it.

## Verdict

**Fail.** CGraph's default context has macro file precision **0.100** (95% CI 0.089-0.112)
on 499 of the 500 Lite instances. The gate is 0.5. No cutoff gets close: the best point on
the full top-K curve is K=2, with 0.217 (95% CI 0.191-0.243). The bash-only agent
(mini-SWE-agent with GPT-5) reaches 0.709 (paper Table 2). Item 2's ContextBench half
fails, and it fails by a wide margin. It is not a noise call.

## Pins

| What | Pin |
|---|---|
| Paper | arXiv 2602.05892v3 (11 Feb 2026); PDF sha256 `517140a2504959bfb94c7c875f6e80a031e899bc362dcc73c446536b45e18a46` |
| Metric code | [EuniAI/ContextBench](https://github.com/EuniAI/ContextBench) @ `1436c28a8eb95496da4ea69ad458b9f8a8eb7d61` |
| Data | HF `Contextbench/ContextBench` @ `c2855792b006af41c67202d33883fb9d46362853`, `data/contextbench_verified.parquet`, sha256 `e9dcfd504cbfb849ac815a79040c793d0d92f94eecc9b5a4ee3e1445a2f8a791` |
| Subset | ContextBench Lite: all 500 instances (Verified 174, Multi 156, Poly 116, Pro 54; 8 languages; 59 repositories) |
| Engine | `cgraph --version`: `cgraph-native 0.1.0 (bin-v0.9.4-1-g43453f3)`, built from main at 43453f3 |

The HF config is named `contextbench_verified`, but it is the Lite set. Its 500
`instance_id`s are exactly the 500 rows of `data/selected_500_instances.csv` in the pinned
repo, which the billed-cost bench used. Every one of its rows is byte-identical to the same
instance in `data/full.parquet` at the same revision.

## Metric

The metric is ContextBench's `compute_granularity_metrics` (`contextbench/metrics/compute.py`).
At the file level it is:

- recall (ContextBench calls it coverage) `= |pred ∩ gold| / |gold|`
- precision `= |pred ∩ gold| / |pred|`

`gold` is `Gold.files()`: the files of every gold-context span, with paths normalized by
ContextBench (`contextbench/parsers/gold.py`). Appendix H of the paper says "all reported
metrics are averaged over evaluated tasks". The headline is therefore the macro mean over instances.
The micro aggregate that ContextBench's CLI prints (`aggregate_results` in
`contextbench/evaluate.py`) is reported next to it. An empty prediction is an error in
ContextBench's evaluator (`no_context_extracted`) and leaves the average. This run had
no empty predictions. One instance has no CGraph output at all (see Findings). The 95% CIs are percentile bootstraps over instances (10,000
resamples, seed 2602).

## What CGraph is asked, and the cutoff

The input is the instance's `problem_statement` only. `instances.py` exports the repo,
commit and issue text, and leaves `gold_context`, `patch`, `test_patch` and the test lists
behind. Retrieval reuses the ARB adapter (`bench/arb/cgraph_eval.py`) unchanged:

- **default** (the gate row). This is one `context` call with `q` set to the issue text and
  every other parameter at the engine default: budget 6000 tokens
  (`kDefaultContextBudget`, `src/engine/daemon_ops.cpp`). The prediction is that bundle's
  distinct files. It is what CGraph hands an agent that asks about the issue. The cutoff is
  CGraph's own default, fixed before any scoring.
- **top-K**. This is the ARB recipe's fused ranking: `context` at budget 8000, `query` with
  limit 100, and `context` around each file the issue text names, merged by reciprocal-rank
  fusion. It is restricted to files in the checked-out tree. Precision and recall are
  reported for every K from 1 to 20, so no K is chosen from the labels.

The agents' numbers are not measured at a cutoff. They score the context each agent
declares at the end of its run (paper §2.5), so ContextBench has no rank cutoff to copy.

## Results (500 instances)

Engine `bin-v0.9.4-1-g43453f3`. n is the number of Lite instances; "valid" is the number
with a non-empty prediction. The one missing instance is
`SWE-Bench-Pro__javascript__maintenance__bugfix__5c001746`: its graph build did not finish,
see Findings. The last two columns score it as 0, and they move no overall number by more
than 0.001.

| Prediction | Group | n (valid) | Mean files | Precision [95% CI] | Recall [95% CI] | F1 | Micro P | Micro R | P, errors as 0 | R, errors as 0 |
|---|---|---:|---:|---|---|---:|---:|---:|---:|---:|
| **default** | overall | 500 (499) | 12.54 | **0.100** [0.089, 0.112] | 0.371 [0.337, 0.407] | 0.134 | 0.072 | 0.260 | 0.100 | 0.371 |
| default | Verified | 174 (174) | 12.38 | 0.098 [0.082, 0.114] | 0.492 [0.428, 0.556] | 0.148 | 0.073 | 0.449 | 0.098 | 0.492 |
| default | Pro | 54 (53) | 10.42 | 0.168 [0.120, 0.219] | 0.246 [0.173, 0.326] | 0.171 | 0.136 | 0.178 | 0.165 | 0.241 |
| default | Poly | 116 (116) | 12.05 | 0.096 [0.073, 0.121] | 0.306 [0.243, 0.371] | 0.123 | 0.069 | 0.284 | 0.096 | 0.306 |
| default | Multi | 156 (156) | 13.81 | 0.083 [0.066, 0.102] | 0.329 [0.269, 0.391] | 0.113 | 0.056 | 0.195 | 0.083 | 0.329 |
| top-1 | overall | 500 (499) | 1.00 | 0.214 [0.178, 0.251] | 0.106 [0.083, 0.129] | 0.126 | 0.214 | 0.062 | 0.214 | 0.105 |
| top-2 | overall | 500 (499) | 2.00 | 0.217 [0.191, 0.243] | 0.200 [0.173, 0.229] | 0.183 | 0.217 | 0.125 | 0.217 | 0.200 |
| top-3 | overall | 500 (499) | 3.00 | 0.192 [0.172, 0.213] | 0.262 [0.231, 0.295] | 0.194 | 0.192 | 0.166 | 0.192 | 0.261 |
| top-5 | overall | 500 (499) | 5.00 | 0.162 [0.146, 0.178] | 0.347 [0.313, 0.382] | 0.194 | 0.162 | 0.233 | 0.162 | 0.346 |
| top-10 | overall | 500 (499) | 10.00 | 0.110 [0.100, 0.120] | 0.441 [0.406, 0.477] | 0.158 | 0.110 | 0.316 | 0.110 | 0.440 |
| top-20 | overall | 500 (499) | 19.97 | 0.070 [0.065, 0.076] | 0.555 [0.520, 0.591] | 0.116 | 0.070 | 0.404 | 0.070 | 0.554 |

Per-source rows for every K are in `runs/<rev>/results.json`. The highest precision on any
row is Pro at top-1: 0.377 (CI 0.245-0.509, n=53).

Full curve (overall, macro):

| K | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 12 | 15 | 20 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Precision | 0.214 | 0.217 | 0.192 | 0.176 | 0.162 | 0.147 | 0.135 | 0.125 | 0.116 | 0.110 | 0.099 | 0.086 | 0.070 |
| Recall | 0.106 | 0.200 | 0.262 | 0.307 | 0.347 | 0.367 | 0.395 | 0.411 | 0.423 | 0.441 | 0.477 | 0.513 | 0.555 |

## Published baselines (file level, ContextBench Lite)

These come from the paper, arXiv 2602.05892v3. The paper gives point estimates only. The
pinned repo ships no per-instance results, so no CI can be computed for them.

| System | Recall | Precision | F1 | Source |
|---|---:|---:|---:|---|
| mini-SWE-agent + GPT-5 (the bash-only agent) | 0.682 | **0.709** | 0.634 | Table 2 |
| Agentless + GPT-5 | 0.609 | 0.352 | 0.390 | Table 2 |
| SWE-agent + GPT-5 | 0.726 | 0.537 | 0.544 | Table 2 |
| OpenHands + GPT-5 | 0.733 | 0.400 | 0.463 | Table 2 |
| Prometheus + GPT-5 (graph-based retrieval) | 0.717 | 0.336 | 0.403 | Table 2 |
| mini-SWE-agent + Claude Sonnet 4.5 | 0.720 | 0.665 | 0.624 | Table 3 |
| mini-SWE-agent + Gemini 2.5 Pro | 0.587 | 0.752 | 0.600 | Table 3 |
| mini-SWE-agent + Devstral 2 | 0.660 | 0.693 | 0.615 | Table 3 |

The build plan's line "graph retrieval found the most gold files, but with half the
precision of a plain bash agent" matches Table 2. Prometheus had recall 0.717 and precision
0.336. mini-SWE-agent had recall 0.682 and precision 0.709.

**Comparability.** The agents are not the same kind of system as CGraph. Each agent runs a
full GPT-5 session, reads code, writes a patch, and then declares the files it used. CGraph
answers the issue text once, with no model and no iteration. The agent numbers show where
the 0.5 gate sits. They are not a like-for-like baseline.

## Findings

1. **The default bundle is too wide for this gate.** A default `context` call returns 12.54
   distinct files on average (median 10, range 1 to 64). The gold context has a mean of 3.47
   files and a median of 2. Suppose every gold file were inside the bundle. Macro precision
   at the bundle sizes CGraph actually returned would still be at most 0.332, so the default
   cannot reach 0.5 however good its ranking is. That ceiling is computed after scoring from
   gold sizes. It is a diagnostic only and was not used to pick anything.
2. **Ranking is the larger gap.** Even a 2-file answer is right only about one time in
   five (top-2 precision 0.217). The default bundle contains at least one gold file on 291
   of the 499 instances.
3. **Recall is also below the agents.** Default recall is 0.371 and top-20 recall is 0.555.
   The agents' file recall is 0.587 to 0.733. The plan's "most gold files" line describes
   Prometheus, a different graph-based agent. It does not describe CGraph's single-shot
   retrieval.
4. **Engine bug: one build never finished.** The build of `element-hq/element-web@c9d9c421`
   ran 2 h 9 min and was killed. That covered 2 h 1 min of CPU on one thread (`ps`:
   `ELAPSED 02:09:22, TIME 02:01:25`). Building each of the snapshot's 2,100 JS/TS files on
   its own with a 10 s timeout isolated one file: `docs/lib/mermaid.min.js`, a 2,935,907-byte
   minified bundle. The thread's stack was:
   `ts_node_parent <- branch_path <- exclusive_branches <- local_values <- collect_url_template
   <- analyze_client_call <- http_call_handler <- js_extra_walk <- walk_node`. This is the
   HTTP-client contract extractor, and it was super-linear on minified code. #204 (a41425e)
   fixed it on main after this run: the same file now builds in 17-28 s with an identical
   graph. The numbers here stay pinned to 43453f3; a 500/500 rerun on a build with #204
   has not been done.
5. **ContextBench path quirk, unchanged.** `_normalize_rel_path` in
   `contextbench/parsers/gold.py` ends with `p.lstrip("./")`. That call strips the leading dot
   of `.github/...`, so gold `github/workflows/*.yml` matches no repository file. In total, 18
   of the 1,746 gold files on Lite are not in their base-commit tree. The others include
   agent-written reproduction scripts such as `repro.js`. This affects every system scored
   with ContextBench's code equally. It is reported, not patched: that would be an eval-data
   change.

## Cost and time

- **Model and API spend: $0.** CGraph has no model in the loop.
- **Wall time.** The full run took 2 h 28 min (`time`: `16132.80s user 755.04s system 190%
  cpu 2:28:03.77 total`), pinned to 8 cores (`taskset -c 0-7`, `--jobs 8`). The other 499
  instances finished within the first 51 minutes. The rest of the time was the single
  stuck build in Finding 4.
- **CPU.** 4.7 CPU-hours in total, of which 2.0 were the stuck build. Over the 499 completed
  instances, the graph builds took 3.08 h (median 5.5 s; maximum 664 s, on mui/material-ui)
  and retrieval took 0.13 h (median 0.45 s per instance).
- **Data.** The snapshot cache is 1.4 GB of shallow bare repositories (59 repositories, 500
  commits). Ten Python repositories were seeded from local mirrors. Their commits are
  identical by SHA, so the result does not depend on where they came from.

## Reproduce

```sh
PYTHON=<python with pyarrow> bench/contextbench/run.sh --cgraph-bin-dir DIR --jobs 8
bench/contextbench/run.sh --cgraph-bin-dir DIR --limit 30   # smoke run, not a score
```

`DIR` holds `cgraph`, `cgraph-client` and `graphd`. Without `--cgraph-bin-dir`, the script
uses the release that `cgraph` on PATH resolves to. The work directory defaults to
`~/.cache/cgraph-contextbench` (`--work` overrides it). The script:

1. clones ContextBench at the pinned commit;
2. downloads the Lite parquet and checks its sha256;
3. exports the label-free inputs;
4. fetches the 500 snapshots with `bench/arb/snapshots.py`;
5. runs CGraph;
6. writes `runs/cgraph-<engine revision>/results.{json,md}`.

It needs git, curl, and Python >= 3.10 with pyarrow. It needs no GPU, no Docker and no
model. ContextBench's tree-sitter pin is needed only for its symbol and span levels, which
this bench does not use.

**Determinism.** `--limit 30` was rerun separately from the full run. For all 30 of 30
instances it produced the same default files, the same fused ranking and the same per-op
lists.

## Files

- `run.sh`: the pinned one-command pipeline.
- `instances.py`: exports the label-free task inputs.
- `cgraph_eval.py`: the adapter. It imports `served_snapshot`, `retrieve`, `SnapshotIndex`
  and the context helpers from `bench/arb/cgraph_eval.py`, and the snapshot cache from
  `bench/arb/snapshots.py`.
- `score.py`: scores the predictions with ContextBench's `GoldLoader`,
  `compute_granularity_metrics` and `aggregate_results`.

`bench/arb/cgraph_eval.py` gained `served_snapshot`. It is the checkout, build, serve and
teardown lifecycle that `evaluate_snapshot` had inline, and both benches now share it. A
one-sample run per task, made with ARB's `trace2code` and `edit2ripple` before and after
this change, gave identical details: metrics, ranked files, per-op lists and graph size.

Eval data changed: no.
