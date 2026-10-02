# Brownfield ideas behind a gate: scope warning, existing-helper hint, learned conventions

> **Status: not built. This is a recorded negative result, archived with no spec changes.** All three ideas were gated on 2026-10-02 against the same bar as `2026-09-28-preview-change-impact` (on the `design-preview-change` branch), and none passed. None of the behaviour below exists in the engine or in the main specs. Any future attempt should start a new change from this record, not revive it.

## Why

Phase 4 of the cross-service plan asked whether three "brownfield" aids would help agents working in mature codebases:

- **(a) Scope warning.** `change_context` flags changed files with no graph path to the task's stated seeds.
- **(b) Existing-helper hint.** Before a new function lands, list existing symbols with matching name stems and signatures (reusing the `report clones` fingerprints).
- **(c) Learned conventions.** Rules mined from history, as in Learning to Commit, served as explicit rules.

The plan admits an idea only if it beats both a same-directory baseline and git co-change on a time-split replay. An idea that ties co-change is not built.

## Protocol (fixed before any scoring)

- **Repos:** CGraph, turing-api, turing-webapp, ml-backend, turing-agents, idp, idp-front-end, passless-app. passless-cli has only one multi-commit PR and is excluded. 89–96% of the PRs are agent-authored, except passless-app's.
- **Replay:** merged multi-commit PRs. The input is what existed at the PR's first commit, and everything mined (co-change, rules) comes strictly from history before the merge base.
- **Fair comparison:** every arm returns the same number of items per PR. Ground truth comes from git, and its sha256 hash is recorded before any arm runs.
- **Statistics:** a paired PR-cluster bootstrap with 2,000 resamples and seed 7, reused from the preview gate. The harness reproduces the preview gate's co-change 0.22728 and same-directory 0.07222 exactly.
- **Pass bar:** a gain over both baselines with CI95 excluding zero, on at least 15 PRs. The gain must also hold with the most influential PR dropped and with each repo left out.

## Results

| Idea | Sample | Outcome |
|---|---|---|
| (a) Scope warning (target: a file the PR changed, then fully undid before merge) | 29 PRs | **Fails.** Catch rate 0.387 against same-directory 0.312 (+0.074, CI95 [−0.014, +0.146]) and co-change 0.319 (+0.068, CI95 [+0.027, +0.105]). Leaving turing-api out flips the gain negative against both (−0.124 and −0.075). All 11 turing-api targets are migration `.sql` files with no graph edges, so "no graph path" meant "is a migration". |
| (c1) Learned conventions as files (target: code files later commits added that the first commit lacked) | 497 PRs, 3,538 target files | **Not built: ties co-change.** Per-PR catch 0.059 against same-directory 0.013 (+0.047, CI95 [+0.032, +0.062]) and co-change 0.048 (+0.012, CI95 [+0.001, +0.024]). Total files caught tie: 110 against co-change's 111.65, difference −0.0005 with CI95 [−0.0055, +0.0049]. With the most influential PR dropped, the CI95 includes zero; leaving one repo out, it includes zero in 3 of 8 runs. Graph-derived rules caught nothing; the small gain comes from reliability-weighted co-change and path patterns, which belong to history ranking rather than the graph. |
| (b) Existing-helper hint (target: a new function later replaced by a call to a helper that already existed) | 523 candidate cases | **Untestable: one qualifying case against a 15-PR minimum.** Two independent judges (Claude and Codex) agreed on all 523 cases (kappa 1.0) and found one true case (turing-webapp `getAuthToken` against `getClientAuthToken`). The candidate examples fail the "helper already existed" rule: code was consolidated into a helper written later. |
| (c2) Learned conventions as patterns (review-comment conventions) | — | **Not run.** The protocol ran it only if (c1) passed. |

## Decision

None of the three is built. Redefining (b)'s label after seeing the labels, for example as "code later consolidated into a new helper", would test a different idea (refactor detection), so it was not done. If (c1)'s history ranking is wanted, it belongs outside the graph, and would need its own gate.

## Evidence

All scripts, frozen ground truth (with hashes) and results are under `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/phase4/` (`a/results.md`, `c1/results.md`, `b/labels/`), with the protocol in `phase4-protocol.md`. They are research artifacts and are not committed here.
