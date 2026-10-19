# ARB external dev set (extdev), CGR-38

A dev set built the way Agent Retrieval Bench (ARB) builds its data, from 10 repositories ARB never used. It exists so CGraph's retrieval can be tuned without touching any ARB sample, which keeps the "no tuning on ARB" claim checkable. It has two tasks: `v2_code2test` (229 samples) and `v2_edit2ripple` (187 samples). The rows were mined by ARB's own deterministic miners at the commit pinned in `bench/arb/run.sh` (`ARB_COMMIT`). Built 2026-10-17. No LLM calls were made at any step.

`$ARB_WORK` below is `run.sh`'s work dir (default `~/.cache/cgraph-arb`). ARB is checked out at `$ARB_WORK/arb`.

## Layout

Committed here:

```
bench/arb/extdev/
  data/benchmark/v2_code2test/{samples.jsonl,manifest.json}
  data/benchmark/v2_edit2ripple/{samples.jsonl,manifest.json}
  scripts/{fetch_pr_metadata.py,crawl_repo.sh,consistent_raw.py,finalize.py}
```

Not committed (large, regenerable, see "How to regenerate the corpus"): `data/corpus/` (chunked file texts, 202 and 113 commit corpora), `repos/` (bare snapshot caches), `baselines/`, `runs/`, `work/` (raw crawl and intermediate miner output).

`finalize.py` also writes `<task>.jsonl`, a byte copy of `samples.jsonl`; it is git-ignored. Each `chunks_path` in a corpus manifest is relative to the directory that holds `data/`, the same convention as the released manifests. So the regenerated tree lives in `$ARB_WORK/extdev/`.

## How to regenerate the corpus

```bash
export ARB_WORK=${ARB_WORK:-$HOME/.cache/cgraph-arb}
export PYTHONPATH=$ARB_WORK/arb/src PYTHONHASHSEED=0
EXT=$ARB_WORK/extdev
mkdir -p $EXT && cp -r bench/arb/extdev/data $EXT/ && cd $EXT

# 1. Bare snapshot caches for every base commit the samples name (needs network).
python3 <repo>/bench/arb/snapshots.py --cache repos --jobs 4 data/benchmark/*/samples.jsonl

# 2. ARB's corpus builder on the committed samples (reads the bare caches).
for t in code2test edit2ripple; do
  python3 -m agent_retrieval_bench.cli build-corpus data/benchmark/v2_$t/samples.jsonl \
    --repos-dir repos --out data/corpus/v2_$t --no-keep-list
done
```

`snapshots.py` takes `--cache DIR`, `--jobs N` and one or more `samples.jsonl` files; it makes one bare repo per upstream repo at `<cache>/<owner>__<name>.git`, the layout `build-corpus --repos-dir` reads.

## Running CGraph and scoring

```bash
cd $EXT
python3 <repo>/bench/arb/cgraph_eval.py --arb-src ../arb --data data --cache repos \
  --work /tmp/cgraph-arb-extdev-snapshots --out runs/cgraph-<rev> --cgraph-bin-dir <bin> \
  --tasks code2test edit2ripple
python3 <repo>/bench/arb/score.py --arb-src ../arb --data data --tasks code2test edit2ripple \
  --out runs/cgraph-<rev>/results.json --markdown runs/cgraph-<rev>/results.md \
  CGraph=runs/cgraph-<rev> RepoMap=baselines:repomap Lexical=baselines:lexical BM25=baselines:bm25
```

`--tasks` makes the scorer work on a dataset with only some tasks. Use the default `--split all`: `DEV_REPOS` in `score.py` names ARB repos, so it has no meaning here. For the baselines, run `run.sh` step 4 (`eval-repomap`, `eval-baseline --ranker lexical|bm25`, all with `--candidate-filter all_files --no-keep-list`) on `data/benchmark/v2_<task>` and `data/corpus/v2_<task>`, writing to `baselines/`.

## Repositories

prometheus/prometheus, labstack/echo, spf13/cobra, sharkdp/bat, psf/black, Textualize/rich, honojs/hono, colinhacks/zod, nushell/nushell, tokio-rs/axum.

hyperium/hyper and BurntSushi/ripgrep were first proposed. Neither appears in released samples or in ARB's PLAN.md, but both are in ARB's `configs/crawl_targets.json` `"reserve"` list. They were swapped for nushell/nushell and tokio-rs/axum, which keeps the Rust share. None of the 10 final repos appears in the released samples (`jq -r .repo data/benchmark/v2_*/samples.jsonl`), in PLAN.md, or under ARB's `configs/ docs/ scripts/ src/ tests/` (checked with grep).

## Pipeline

Run from `$ARB_WORK/extdev/work` with `PYTHONPATH=$ARB_WORK/arb/src PYTHONHASHSEED=0` and a GitHub token in `GH_TOKEN` or `GITHUB_TOKEN`. `arb` means `python3 -m agent_retrieval_bench.cli`. The scripts write their JSON summaries and logs under `work/` (for example `work/logs/`).

1. **PR metadata (GraphQL only).** `scripts/fetch_pr_metadata.py --raw raw --repo R --limit-prs 250`. It calls ARB's own `crawler._fetch_merged_prs_graphql` (MERGED_PR_QUERY, `pullRequests(states: MERGED ... orderBy: UPDATED_AT DESC)`) with crawl's default `--max-changed-files 20`. It writes the same `{"type": "pull_request", ...}` record `crawl_repo` writes, through ARB's `_upsert_jsonl`. It does not make crawl's per-PR REST calls (files, comments, commits, per-commit details). Total cost was about 60 GraphQL points.
2. **Files, commits and commit details from git.** `arb backfill-git-raw --raw raw --repo R --repos-dir git_raw_repos`, driven by `scripts/crawl_repo.sh R 250` (which also runs step 1). No REST.
3. **Diff-consistency filter** (`scripts/consistent_raw.py`, a deviation, see below): `raw` to `raw_consistent`.
4. **Mining with default filters.** `arb mine-code2test-prs --raw raw_consistent` and `arb mine-edit2ripple --raw raw_consistent` into `stage1/`.
5. **Preselection for corpus building** (`scripts/finalize.py --per-repo-limit 33` for code2test and `28` for edit2ripple, `--repo-share 1.0`) into `preselect/`. This only keeps corpus building bounded.
6. **Corpus.** `arb build-corpus preselect/<task>/samples.jsonl --repos-dir ../repos --out ../data/corpus/v2_<task> --no-keep-list`. Chunk files for commits shared by both tasks are hard-linked; ARB reuses existing chunk files through `existing_chunks_manifest`.
7. **Re-mining with ARB's corpus gates.** `arb mine-code2test-prs|mine-edit2ripple --raw raw_consistent --corpus-manifest data/corpus/v2_<task>/corpus_manifest.jsonl --require-corpus --require-gold-in-corpus` into `stage2/`.
8. **Final selection** (`scripts/finalize.py --task <task> --samples stage2/<task>/samples.jsonl --out data/benchmark/v2_<task> --per-repo-limit 25|20`). In order: ARB's fatal-leakage filter, a recency-ordered per-repo limit, the edit2ripple test-only quota, the 25% per-repo cap.
9. **Final corpus.** `build-corpus` re-run on the final samples to rewrite `corpus_manifest.jsonl`; chunk files no manifest references are deleted.
10. **Baselines**, as in "Running CGraph and scoring".

## Filter funnel

| repo | GraphQL PRs seen | kept (merged, <=20 files) | git-backfilled | diff-consistent |
|---|---:|---:|---:|---:|
| prometheus/prometheus | 252 | 250 | 250 | 200 |
| labstack/echo | 253 | 250 | 250 | 237 |
| spf13/cobra | 253 | 250 | 250 | 231 |
| sharkdp/bat | 251 | 250 | 249 | 248 |
| psf/black | 250 | 250 | 250 | 231 |
| Textualize/rich | 253 | 250 | 250 | 239 |
| honojs/hono | 257 | 250 | 250 | 193 |
| colinhacks/zod | 260 | 250 | 250 | 199 |
| nushell/nushell | 262 | 250 | 249 | 166 |
| tokio-rs/axum | 256 | 250 | 250 | 242 |

Two PRs are missing from the git-backfilled column:

- **bat:** one PR came back as `no_changed_files`.
- **nushell:** `backfill-git-raw` crashed on PR #18261 with a `UnicodeDecodeError`. ARB's `run_process` calls `subprocess.run(..., text=True ...)` (git_raw.py:311), and that PR's diff contains a byte (0xff) that is not valid UTF-8. It crashed twice. It was the last PR processed, so 249 records had already been written.

Every diff-consistency drop was `base_not_ancestor_of_head` (reported in `consistency_summary.json` by `consistent_raw.py`).

Miner drop reasons, summed over all repos:

- **code2test:** missing_impl_or_existing_tests 862, low_value_pr 637, test_path_leak 114, weak_query 54, too_many_tests 38, query_noise 1.
- **edit2ripple:** changed_file_limit 807, no_related_gold_or_query_leakage 310, missing_anchor 228, low_value_pr 227, rename_or_generated_only 2.

The corpus-gated re-mine also dropped `gold_missing_from_corpus` rows: 2 per task.

**code2test**

| repo | miner (no corpus) | preselected for corpus | miner with corpus gates | after ARB fatal-leakage filter | final |
|---|---:|---:|---:|---:|---:|
| prometheus/prometheus | 64 | 33 | 31 | 31 | 25 |
| labstack/echo | 66 | 33 | 33 | 33 | 25 |
| spf13/cobra | 30 | 30 | 30 | 30 | 25 |
| sharkdp/bat | 32 | 32 | 32 | 32 | 25 |
| psf/black | 32 | 32 | 32 | 32 | 25 |
| Textualize/rich | 80 | 33 | 33 | 33 | 25 |
| honojs/hono | 73 | 33 | 34 | 34 | 25 |
| colinhacks/zod | 73 | 33 | 34 | 34 | 25 |
| nushell/nushell | 26 | 26 | 26 | 26 | 25 |
| tokio-rs/axum | 4 | 4 | 4 | 4 | 4 |
| **total** | 480 | 289 | 289 | 289 | 229 |

**edit2ripple**

| repo | miner (no corpus) | preselected for corpus | miner with corpus gates | after ARB fatal-leakage filter | final |
|---|---:|---:|---:|---:|---:|
| prometheus/prometheus | 89 | 28 | 28 | 28 | 20 |
| labstack/echo | 110 | 28 | 28 | 28 | 17 |
| spf13/cobra | 66 | 28 | 31 | 31 | 20 |
| sharkdp/bat | 34 | 28 | 28 | 28 | 18 |
| psf/black | 77 | 28 | 29 | 29 | 20 |
| Textualize/rich | 115 | 28 | 28 | 28 | 19 |
| honojs/hono | 95 | 28 | 28 | 28 | 13 |
| colinhacks/zod | 111 | 28 | 28 | 28 | 20 |
| nushell/nushell | 64 | 28 | 28 | 28 | 20 |
| tokio-rs/axum | 50 | 28 | 28 | 28 | 20 |
| **total** | 811 | 280 | 284 | 284 | 187 |

- **Gated count above preselected:** the corpus-gated re-mine can return more rows than were preselected. Any extra row reuses a base commit whose corpus was already built.
- **Final edit2ripple below 20:** some repos end with fewer than 20 rows because their 28-row pool did not hold enough rows under the test-only quota.
- **Largest repo share:** 25 of 229 (10.9%) for code2test and 20 of 187 (10.7%) for edit2ripple, well under the 25% cap.

## Audit substitute (no human audit)

1. **ARB's mechanical miner filters at their CLI defaults.** code2test: `--max-changed-files 20` and `--max-tests 3` (cli.py:290-291), plus `missing_impl_or_existing_tests`, `too_many_tests`, `query_noise` and `test_path_leak` (code2test_pr.py:200, 202, 209, 211). edit2ripple: `--max-changed-files 8` and `--max-candidates-per-pr 2` (cli.py:305, 307); candidates with fatal leakage are discarded (`if leakage["fatal"]:`, edit2ripple.py:694).
2. **ARB's corpus gates:** `--require-corpus --require-gold-in-corpus`. Every gold file exists in its commit's corpus; re-verified after final selection: 0 missing.
3. **ARB's leakage filters.** A row is dropped if `dataset_validity.detect_query_leakage(row)["fatal"]` (fatal types `{"exact_gold_path", "raw_patch_marker", "review_leakage", "fix_commit_hash"}`, dataset_validity.py:34) or `baseline.query_has_leakage` (baseline.py:517, the check `eval-baseline` uses to skip a sample) is true. Result: 0 drops in both tasks, because the miners' own gates had already removed these rows. The same detector reports 0 fatal leaks on the released gin/caddy/clap rows.
4. **edit2ripple test-only policy.** At most `floor(0.25 x 20) = 5` test-only-gold rows per repo, following the released `strict25_policy` (`"max_test_only_ratio": 0.25`, `data/reports/v2_edit2ripple/edit2ripple_strict_expanded_v1/selection_summary.json:39`). The final test-only share is 0.235. `report-edit2ripple-pilot` defaults to the looser `max_test_only_ratio: float = 0.35` (edit2ripple.py:506).
5. **Lexical hardness pass (`hard-pool-filter` / `too_easy_lexical`): not applied.** The released v2 rows show no evidence of it:
   - None of the 17 PR-mined code2test dev rows carries hardness metadata. Their metadata keys are only `confidence, created_at, evidence, generated_at, merged_at, pr, pr_url`. The other 2 rows carry `hardmine_source` from the older derive path.
   - The released rows still contain samples that rule would have removed. hardness.py:293-296 labels a row `too_easy_lexical` when it is not hard and lexical `Recall@5 >= 1.0`. In the released lexical baseline details for the dev repos, 12 of 28 edit2ripple rows and 1 of 19 code2test rows meet that rule (R@5 = 1, R@20 = 1, MRR >= 0.25), and they were released anyway.
6. **Audit block.** edit2ripple rows keep the miner's audit block with `"verdict": "pending"` plus a note saying no human audit was done. Released rows say `"valid"` because they were audited; extdev does not claim that.

## Deviations from ARB's pipeline, and why

- **GraphQL-only metadata driver instead of `arb crawl --no-checks`.** It writes identical `pull_request` records but skips crawl's 3+N REST calls per PR, to keep API cost low. `backfill-git-raw` then fills files, commits and commit details from git.
- **Diff-consistency filter after `backfill-git-raw`.** ARB's backfill diffs `base..head` two-dot (`git diff --name-status --find-renames base head`, git_raw.py:203). GitHub's PR file list is the merge-base diff, so the two agree only when `baseRefOid` is an ancestor of `headRefOid`. In the hono pilot, PR #5233 had GraphQL `changedFiles=2`, but the git diff listed 311 files. Across the 40 pilot PRs, the ancestor test and an equal file count agreed on every one: all 10 inconsistent PRs failed the ancestor test.
  - The rule: keep a PR only if base is an ancestor of head and the git file count equals `changedFiles`. This removed between 0.8% (bat) and 34% (nushell) of PRs per repo, counting PRs the backfill did not produce; hono, zod and prometheus also lost 20% or more. The filter is biased against long-lived branches.
  - It is conservative: `fetch --depth 50` (git_raw.py:192) can hide ancestry, which can only cause a wrong drop, never a wrong keep.
- **Fixed per-repo limit (25 for code2test, 20 for edit2ripple) and recency ordering.** The raw yield was 480 code2test and 811 edit2ripple candidates. Equal per-repo limits keep the corpus and the CGraph run size reasonable and stay far below the 25% cap.
  - Rows are chosen by PR number descending, the order the miners walk PRs. An earlier pass used the miners' audit-priority `sample_sort_key`, which favors multi-file non-test gold. It produced an edit2ripple set with only 2% test-only gold and 2.71 gold files per sample, against the released dev rows' 43% and 1.54. Recency ordering gives the figures below.

    | statistic | released dev rows (gin/caddy/clap) | extdev |
    |---|---:|---:|
    | edit2ripple gold files per sample | 1.54 | 2.01 |
    | edit2ripple test-only gold | 42.9% | 23.5% |
    | edit2ripple changed files | 3.5 | 4.34 |
    | code2test tests per sample | 1.21 | 1.28 |
    | code2test implementation files | 1.89 | 2.02 |

- **Two-pass mining.** Mining runs once without the corpus, then again with the corpus gates after corpora are built only for a preselected superset. Building corpora for all 1,291 candidates was unnecessary.
- **Schema.** code2test rows add top-level `query_provenance: "pr_summary"`, which matches the released rows and `baseline.query_provenance`'s code2test default. Remaining gaps against the released gin/caddy/clap rows:
  - **code2test, `gold_blocks` / `gold_spans`:** from manual span annotations; they feed span and block metrics only, not R@k, MRR or BCY. ARB has no deterministic way to produce them.
  - **code2test, `hard_negative_files`:** present on 1 of 19 released rows.
  - **code2test, `query.changed_file` and `metadata.hardmine_source`:** only on the 2 released rows from the older derive path.
  - **edit2ripple, `metadata.source`, `source_samples`, `expansion_candidate_sources` and `improvement`:** provenance of the from-sample-commits and expansion paths, which do not apply to the raw-PR miner.

  All other keys match.
- **Manifests.** `manifest.json` files use `schema: "arb_extdev_v2"` and record the funnel and selection parameters.

## Baseline scores on extdev (ARB harness, all_files candidates, scored with `score.py --tasks code2test edit2ripple`)

| Method | Task | n | R@20 | MRR | BCY@8k |
|---|---|---:|---:|---:|---:|
| RepoMap | code2test | 229 | 0.8530 | 0.4980 | 0.6783 |
| Lexical | code2test | 229 | 0.7285 | 0.2058 | 0.4047 |
| BM25 | code2test | 229 | 0.6667 | 0.2516 | 0.3588 |
| RepoMap | edit2ripple | 187 | 0.7571 | 0.4008 | 0.4630 |
| Lexical | edit2ripple | 187 | 0.7086 | 0.2874 | 0.4661 |
| BM25 | edit2ripple | 187 | 0.5945 | 0.2842 | 0.3592 |
| RepoMap | overall | 416 | 0.8099 | 0.4543 | 0.5815 |
| Lexical | overall | 416 | 0.7196 | 0.2424 | 0.4323 |
| BM25 | overall | 416 | 0.6342 | 0.2663 | 0.3590 |

`score.py` also prints R@5/10, BCY@4k-32k and the given-file-excluded diagnostic. Every baseline summary reports `skipped: {}`, so all 229 and 187 samples were evaluated.

For reference only, the released dev split (gin/caddy/clap) scores as follows with the same baselines (R@20 / MRR):

| Method | code2test (n=19) | edit2ripple (n=28) |
|---|---:|---:|
| RepoMap | 0.947 / 0.360 | 0.768 / 0.312 |
| Lexical | 0.316 / 0.064 | 0.673 / 0.297 |
| BM25 | 0.316 / 0.137 | 0.750 / 0.220 |

On extdev, edit2ripple scores are close to the released dev split. code2test is noticeably easier for Lexical and BM25 on extdev.
