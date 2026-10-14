# Billed-cost A/B: Claude Code with and without the CGraph plugin (CGR-29)

**Verdict: the gate fails.** On 32 SWE-bench Verified tasks (3 runs per task per arm), installing the
CGraph plugin made Claude Code **7.1% more expensive** (95% CI 4.8% to 9.4%). The solve rate was
the same within noise: 84/96 runs with the plugin, 82/96 without. The agent **never called a CGraph
tool** in any of the 96 plugin runs. All of the extra cost comes from the plugin's always-loaded
prompt text, which is written to the prompt cache and then re-read on every request.

Gate (CGR-29): billed cost no higher than baseline, at an equal or better solve rate.

| | Baseline | CGraph plugin | Paired difference (CGraph minus baseline), 95% CI |
|---|--:|--:|---|
| Billed cost per run (mean) | $0.0529 | $0.0567 | **+$0.0038** [+$0.0025, +$0.0050]; median +$0.0031 [+$0.0020, +$0.0048] |
| Billed cost, ratio of totals | 1.000 | **1.071** | [1.048, 1.094] |
| Tasks where CGraph cost less | | | 4 of 32 (28 cost more) |
| Turns per run (mean) | 4.35 | 4.46 | +0.10 [-0.09, +0.32]; median 0.00 [-0.33, +0.33] |
| Wall time per run (mean) | 14.2 s | 14.3 s | +0.1 s [-0.5, +0.7]; median -0.15 s [-1.05, +0.83] |
| Solve rate | 85.4% (82/96) | 87.5% (84/96) | +2.1 points [0.0, +5.2]; median 0 [0, 0] |
| Runs that called any CGraph tool | n/a | **0 of 96** | |

The supplementary hard set (13 tasks rated "1-4 hours" in SWE-bench Verified) gives the same
result. CGraph cost 6.6% more (95% CI 0.7% to 12.9%), solved 13/39 runs against the baseline's
14/39, and made 0 CGraph calls in 39 runs.

## Setup

- **Tasks.** The main set is 32 SWE-bench Verified instances that are also in ContextBench Lite
  (`selected_500_instances.csv`) and rated "15 min - 1 hour". It covers 5 repos: django 10,
  sympy 8, scikit-learn 6, sphinx 6, xarray 2. The instances were sampled with seed 29 by
  `select_tasks.py main` and are listed in `tasks.json`. The supplementary set (`tasks-hard.json`,
  from `select_tasks.py hard`) is every ContextBench Lite ∩ Verified instance rated "1-4 hours":
  13 tasks across astropy, django, pylint, pytest, sphinx and sympy.
- **Arms.**
  - Baseline: Claude Code with no plugins.
  - CGraph: the same install plus `claude plugin marketplace add Nxtsoft/CGraph` and
    `claude plugin install cgraph@cgraph`, taken from GitHub main at 7ae98e3. That is plugin 0.9.1,
    whose launcher is pinned to `bin-v0.9.1`. The binary was downloaded into the template once, so
    the download does not count against any run.
  - Every run reported `mcp_servers: plugin:cgraph:cgraph connected` (96/96 main, 39/39 hard).
- **Held equal across arms.**
  - Binary: `/home/taylor/.agents/.history/versions/claude/2.1.260/node_modules/.bin/claude`. The
    `init` event reports `claude_code_version` 2.1.296 in all 270 gated runs.
  - Model: `--model claude-sonnet-5-5`, confirmed by `init.model` in every run.
  - Prompt: the same `PROMPT` in `run_ab.py`, which is the issue text plus "edit non-test source,
    do not install or run tests".
  - Flags: `--dangerously-skip-permissions --max-budget-usd 5`, headless
    `-p --output-format stream-json --verbose`.
- **Isolation.** Each run gets its own HOME and CLAUDE_CONFIG_DIR, copied from a per-arm template,
  and a clean environment with only PATH, LANG, the OAuth token and `DISABLE_AUTOUPDATER`.
  - The checkout is a fresh one-commit git repo at the instance's base commit, so the fix cannot
    be read out of git history.
  - Checkouts sit under `/var/tmp/cgr29/w/<hash>/`. No ancestor directory has a CLAUDE.md, and no
    path the agent sees names its arm. A first attempt put the arm name in the path; those 7 runs
    were thrown away (see Spend).
- **Seeds.** Claude Code has no sampling seed, so a "seed" here is an independent repeat. Each
  task ran 3 times per arm. Jobs interleave the arms for each task and seed and run 6 at a time,
  so both arms see the same load.
- **Scoring.** The official SWE-bench harness (`swebench` 5.0.2, Docker images
  `swebench/sweb.eval.x86_64.*`) applies the agent's patch, applies the test patch and runs the
  FAIL_TO_PASS and PASS_TO_PASS tests.
  - The harness marked 2 hard-set instances "ambiguous" in some runs (pylint-8898
    `missing_module`, pytest-10356 `no_tests_collected`). The gold patch resolves both in the same
    images, so the graded per-instance `report.json` result is used for those runs.
- **Cost.** `total_cost_usd` from the final `result` event. Auth is a Claude subscription OAuth
  token, so this is Claude Code's list-price bill for the usage (`costBasis: list`), not an
  invoice line.
  - A least-squares fit of cost against the four token classes over the 192 main runs reproduces
    every run to within $3e-16, at $2 per million input tokens, $10 output, $0.10 cache read and
    $4 cache write (1-hour TTL). Those are the prices used in the breakdown below.
- **Statistics.** Each task's value for an arm is the mean of its 3 runs. Paired differences are
  per task (n = 32). CIs are 95% percentile bootstraps over tasks (10,000 resamples, RNG seed 29).

## Why CGraph loses

**1. The agent does not use CGraph, so it cannot win anything back.** There were 0 graph-tool
calls, 0 `Skill` calls and 0 `ToolSearch` calls in all 135 plugin runs (main plus hard). Claude Code
2.1.296 lists MCP tools as deferred. The plugin's 12 tool schemas measure 16,576 characters
(about 4.1k tokens) from `cgraph-mcp` `tools/list`, but the first request grows by only 751 tokens.
So the agent sees only the tool names and the skill descriptions. To call a graph tool it must
first decide to `ToolSearch` for it, and on these tasks it never did. Sonnet 5.5 finds the code
with 1 to 3 `grep` calls (3.4 tool calls and 4.4 turns per run on average), which leaves little
navigation for a graph to replace.

**2. The plugin adds a fixed 751-token prefix to every request, and under prompt caching that prefix
is paid as a cache write and then as a cache read on every turn.**

| Main set, all 96 runs per arm | Baseline | CGraph | Δ | Δ in $ (fitted prices) |
|---|--:|--:|--:|--:|
| First-request prefix tokens (mean) | 17,573 | 18,323 | **+751** [749, 752] on 32/32 tasks | |
| Cache-write tokens per run (mean) | 8,918 | 9,743 | +825 [693, 957] | +$0.0033 |
| Cache-read tokens per run (mean) | 70,665 | 75,638 | +4,974 [981, 9,312] | +$0.0005 |
| Output tokens per run (mean) | 1,015 | 1,011 | -4 [-51, +41] | -$0.00004 |
| Billed $ by class, cache write (total) | $3.42 | $3.74 | +$0.32 | 88% of the gap |
| Billed $ by class, cache read (total) | $0.68 | $0.73 | +$0.05 | 13% of the gap |
| Billed $ by class, output (total) | $0.97 | $0.97 | -$0.004 | |

The prefix grows by about 751 tokens in every run. That is the 1,326 characters of the `cgraph` and
`cgraph-enrich` skill descriptions plus the 12 deferred tool names (490 characters); this split is
estimated from the plugin files. Cache writes make up 67% of the baseline bill, so a prefix that
gets written to the cache is the most expensive kind of token here. The cost difference comes
almost entirely from cache tokens: 88% from cache writes, 13% from cache reads, and output tokens
are unchanged. This is the "fewer tokens is not a lower bill" effect from the research, seen from
the other side: a few hundred always-loaded tokens are a measurable share of a $0.05 run.

**3. Telling the agent to use CGraph does not fix it (diagnostic arm).** The `cgraph_directed` arm
used the same plugin install and added one sentence to the prompt: "Use the cgraph code-graph
tools … to locate and understand the relevant code before reading or editing files." This arm is a
diagnostic only. Its prompt differs from the baseline, so it does not count toward the gate.

| | Main (32 tasks × 3) | Hard (13 tasks × 3) |
|---|--:|--:|
| Runs with any CGraph call | 1 / 96 | 5 / 39 |
| CGraph calls (all `graph_query`) | 1 | 5 |
| Cost ratio vs baseline | 1.069 [1.032, 1.104] | 1.043 [0.981, 1.107] |
| Solved | 84/96 (baseline 82/96) | 13/39 (baseline 14/39) |
| Paired runs that called CGraph: cost vs same task and seed baseline | +$0.0265 (n=1) | +$0.0128 (n=5), +2.4 turns |

Even when told to, the agent made at most 1 graph query per run, then went back to `grep` and
`Read`. The first query on a cold checkout took 9 to 10 seconds of wall time on django, 8 seconds on
sympy and 3 seconds on sphinx while the daemon built the index. For example, in django-16263 seed 1, `graph_query
{"query":"get_count"}` was sent at 17:10:15.281 and answered at 17:10:24.451. The answer was a
symbol list that the agent then re-read with `Read`, so in the runs that called CGraph it added
turns instead of replacing them.

## Proposed follow-up (the gap becomes the next ticket)

1. **Shrink the always-loaded footprint.** This is the only lever that changes cost when the tools
   go unused, which was 100% of runs here.
   - Cut the two skill descriptions from 1,326 characters to one short line each.
   - Keep `cgraph-enrich` out of coding sessions, for example as a separate plugin or with
     `disable-model-invocation`.
   - Expose fewer MCP tools by default (a core of query, context and impact rather than 12).
   - Target: a first-request prefix delta of no more than 150 tokens, which would put the
     plugin's overhead inside the noise on these tasks.
2. **Make one call worth more than the greps it replaces.** Return the source span the agent needs
   inside `graph_query` (or route to `graph_context` first) so the follow-up `Read` is not needed.
   Pre-build the index when the session starts (the plugin's `SessionStart` hook) so the first call
   is not 8 to 10 seconds.
3. **Re-run this harness unchanged on the new plugin build:** `run_ab.py` and `evaluate.py` with
   the same `tasks.json`, plus a larger-repo or multi-file task set where `grep` needs many rounds.
   On SWE-bench Verified tasks of this size, Sonnet 5.5 needs about 3 tool calls, so the most a
   navigation tool can save is small.

## Per-task results (main set, mean of 3 runs per arm)

| Task | Base $ | CGraph $ | Δ $ | Base turns | CGraph turns | Base solved | CGraph solved |
|---|--:|--:|--:|--:|--:|--:|--:|
| django__django-11749 | 0.0481 | 0.0489 | +0.0007 | 4.0 | 3.7 | 3/3 | 3/3 |
| django__django-12155 | 0.0394 | 0.0399 | +0.0006 | 3.7 | 3.3 | 3/3 | 3/3 |
| django__django-12262 | 0.0408 | 0.0467 | +0.0059 | 3.0 | 3.3 | 3/3 | 3/3 |
| django__django-13343 | 0.0445 | 0.0466 | +0.0021 | 4.3 | 4.0 | 3/3 | 3/3 |
| django__django-14140 | 0.0393 | 0.0423 | +0.0030 | 3.0 | 3.0 | 2/3 | 3/3 |
| django__django-14434 | 0.0566 | 0.0594 | +0.0028 | 6.3 | 6.0 | 3/3 | 3/3 |
| django__django-15103 | 0.0461 | 0.0505 | +0.0044 | 5.0 | 4.7 | 3/3 | 3/3 |
| django__django-16032 | 0.0778 | 0.0843 | +0.0065 | 5.3 | 6.7 | 3/3 | 3/3 |
| django__django-16527 | 0.0363 | 0.0395 | +0.0032 | 4.0 | 3.3 | 3/3 | 3/3 |
| django__django-16819 | 0.0517 | 0.0536 | +0.0019 | 4.0 | 4.0 | 3/3 | 3/3 |
| sympy__sympy-13551 | 0.0644 | 0.0617 | -0.0027 | 4.3 | 3.7 | 2/3 | 3/3 |
| sympy__sympy-13647 | 0.0374 | 0.0413 | +0.0039 | 3.0 | 3.0 | 3/3 | 3/3 |
| sympy__sympy-14976 | 0.0453 | 0.0429 | -0.0024 | 4.0 | 3.0 | 3/3 | 3/3 |
| sympy__sympy-17318 | 0.0616 | 0.0732 | +0.0116 | 4.7 | 6.3 | 3/3 | 3/3 |
| sympy__sympy-21930 | 0.0417 | 0.0473 | +0.0057 | 3.7 | 4.3 | 0/3 | 0/3 |
| sympy__sympy-22456 | 0.0524 | 0.0607 | +0.0082 | 4.3 | 5.0 | 3/3 | 3/3 |
| sympy__sympy-22914 | 0.0440 | 0.0492 | +0.0052 | 4.0 | 4.3 | 3/3 | 3/3 |
| sympy__sympy-23824 | 0.0410 | 0.0439 | +0.0030 | 3.0 | 3.0 | 3/3 | 3/3 |
| scikit-learn__scikit-learn-10844 | 0.0396 | 0.0441 | +0.0045 | 3.0 | 3.3 | 3/3 | 3/3 |
| scikit-learn__scikit-learn-15100 | 0.0404 | 0.0426 | +0.0022 | 3.3 | 3.0 | 3/3 | 3/3 |
| scikit-learn__scikit-learn-25747 | 0.1024 | 0.1113 | +0.0089 | 7.7 | 8.0 | 0/3 | 0/3 |
| scikit-learn__scikit-learn-25931 | 0.0531 | 0.0601 | +0.0070 | 4.7 | 5.0 | 3/3 | 3/3 |
| scikit-learn__scikit-learn-26323 | 0.0471 | 0.0490 | +0.0019 | 4.0 | 3.7 | 3/3 | 3/3 |
| scikit-learn__scikit-learn-9288 | 0.0645 | 0.0724 | +0.0079 | 5.7 | 6.3 | 3/3 | 3/3 |
| sphinx-doc__sphinx-10614 | 0.0701 | 0.0689 | -0.0012 | 6.3 | 6.0 | 0/3 | 0/3 |
| sphinx-doc__sphinx-11445 | 0.0418 | 0.0450 | +0.0032 | 3.3 | 3.3 | 3/3 | 3/3 |
| sphinx-doc__sphinx-8120 | 0.0488 | 0.0502 | +0.0013 | 3.0 | 3.0 | 3/3 | 3/3 |
| sphinx-doc__sphinx-8265 | 0.0680 | 0.0689 | +0.0008 | 6.7 | 6.3 | 3/3 | 3/3 |
| sphinx-doc__sphinx-8638 | 0.0635 | 0.0737 | +0.0102 | 5.3 | 6.3 | 3/3 | 3/3 |
| sphinx-doc__sphinx-9602 | 0.0631 | 0.0630 | -0.0001 | 4.7 | 4.3 | 0/3 | 0/3 |
| pydata__xarray-6599 | 0.0684 | 0.0688 | +0.0004 | 5.0 | 5.0 | 3/3 | 3/3 |
| pydata__xarray-6721 | 0.0538 | 0.0632 | +0.0094 | 3.0 | 4.3 | 3/3 | 3/3 |

The hard-set per-task table and every number above are in `results/*.json`. The `per_task`,
`metrics`, `cgraph_usage` and `cost_breakdown_usd` keys come from `analyze.py`.

## Spend

The total billed was **$25.25**, against the approved ceiling of $400. The smoke run (2 tasks × 2
arms) cost $0.055 per run on average, which projected about $10 for the main set.

| Batch | Runs | Billed |
|---|--:|--:|
| Smoke (2 tasks × 2 arms, kept out of the results) | 4 | $0.22 |
| First attempt, aborted because the arm name was in the checkout path (discarded) | 7 finished | $0.31 |
| Main set, baseline + CGraph, 3 seeds | 192 | $10.52 |
| Hard set, baseline + CGraph, 3 seeds | 78 | $5.83 |
| Directed diagnostic arm, main + hard, 3 seeds | 135 | $8.37 |

Five runs that were still in flight when the first attempt was stopped were killed before they
reported a cost. They are not included in the total.

## Reproduce

These steps were run on Linux x86-64 with Docker. The Python dependencies are `swebench==5.0.2`,
`datasets`, `pandas`, `pyarrow` and `numpy`.

```bash
W=/var/tmp/cgr29                                    # work dir; no CLAUDE.md in any ancestor
for r in django/django sympy/sympy scikit-learn/scikit-learn sphinx-doc/sphinx pydata/xarray \
         astropy/astropy pylint-dev/pylint pytest-dev/pytest; do
  git clone --bare https://github.com/$r.git $W/mirrors/${r//\//__}.git
done
python select_tasks.py main  <ContextBench selected_500_instances.csv> tasks.json
python select_tasks.py hard  <ContextBench selected_500_instances.csv> tasks-hard.json
secrets exec anthropic -- python run_ab.py templates --work $W
# copy the downloaded launcher cache into the template so no run pays for the download:
#   $W/templates/cgraph/cfg/plugins/data/cgraph-cgraph/bin-v0.9.1/
secrets exec anthropic -- python run_ab.py run --work $W --tasks tasks.json --seeds 1 2 3 --jobs 6
secrets exec anthropic -- python run_ab.py run --work $W --tasks tasks-hard.json --seeds 1 2 3 --jobs 6
secrets exec anthropic -- python run_ab.py run --work $W --tasks tasks.json --arms cgraph_directed --seeds 1 2 3 --jobs 6
secrets exec anthropic -- python run_ab.py run --work $W --tasks tasks-hard.json --arms cgraph_directed --seeds 1 2 3 --jobs 6
python evaluate.py --work $W --tasks tasks.json --jobs 8
python evaluate.py --work $W --tasks tasks-hard.json --jobs 8
for t in main hard; do f=tasks.json; [ $t = hard ] && f=tasks-hard.json
  python analyze.py --work $W --tasks $f --out results/$t-cgraph.json
  python analyze.py --work $W --tasks $f --arm-b cgraph_directed --out results/$t-cgraph_directed.json
done
```

`runs/<instance>/<arm>-s<seed>/` holds each run's `result.json` (cost, usage, turns, tool calls,
SWE-bench verdict), the agent's `patch.diff`, and the full stream-json transcript
(`stream.jsonl.gz`). There are 405 runs in all.
