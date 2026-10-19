"""Post-miner selection for the extdev set: ARB leakage filter, edit2ripple test-only policy, per-repo cap, schema parity.

Inputs are ARB miner outputs (samples.jsonl written by mine-code2test-prs / mine-edit2ripple with corpus gates on).
Every step is deterministic. Rows are ordered by PR number descending (default --order recency) or by the miners'
sample_sort_key (--order miner); per-repo selection keeps the first rows in that order.

1. ARB leakage: drop rows where dataset_validity.detect_query_leakage(...)["fatal"] (FATAL_LEAKAGE_TYPES) or
   baseline.query_has_leakage(...) (the check eval-baseline uses to skip a sample) is true.
2. edit2ripple only: test-only-gold share <= 25% (released v2_edit2ripple selection_summary.json strict25_policy,
   max_test_only_ratio 0.25); drops the last test-only rows in the chosen order.
2b. Optional fixed --per-repo-limit (first N per repo in the chosen order), applied before step 2; for edit2ripple
    at most floor(max_test_only_ratio * N) of a repo's N rows may have test-only gold.
3. Per-repo cap: largest c with every repo <= c and c <= 25% of the task total; keeps each repo's first c rows.
4. Schema parity with released rows: code2test gets top-level query_provenance="pr_summary" (released rows carry it;
   baseline.query_provenance defaults code2test to the same value). edit2ripple keeps the miner's audit block with
   verdict "pending" and a note (no human audit was done).
"""
from __future__ import annotations

import argparse
import json
import math
from collections import Counter, defaultdict
from pathlib import Path

from agent_retrieval_bench import baseline
from agent_retrieval_bench.dataset_validity import detect_query_leakage
from agent_retrieval_bench.filters import is_test_file

AUDIT_NOTE = "extdev: no human audit; ARB miner filters + corpus gates + ARB fatal-leakage filter only"


def read(path: Path) -> list[dict]:
    return [json.loads(l) for l in path.read_text().splitlines() if l.strip()]


def cap_rows(rows: list[dict], share: float) -> tuple[list[dict], int]:
    by_repo: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        by_repo[r["repo"]].append(r)
    best = 0
    for c in range(1, max((len(v) for v in by_repo.values()), default=0) + 1):
        total = sum(min(len(v), c) for v in by_repo.values())
        if c <= share * total + 1e-9:
            best = c
    keep = {id(r) for v in by_repo.values() for r in v[:best]}
    return [r for r in rows if id(r) in keep], best


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--task", choices=("code2test", "edit2ripple"), required=True)
    p.add_argument("--samples", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True, help="benchmark dir, e.g. data/benchmark/v2_code2test")
    p.add_argument("--repo-share", type=float, default=0.25)
    p.add_argument("--max-test-only-ratio", type=float, default=0.25)
    p.add_argument("--order", choices=("recency", "miner"), default="recency",
                   help="row order for per-repo selection: recency = PR number descending (the order ARB's miners walk PRs), "
                        "miner = the miners' audit-priority sample_sort_key (biased to multi-file non-test gold)")
    p.add_argument("--per-repo-limit", type=int, help="keep at most N rows per repo (first N in miner order), applied before the test-only policy")
    a = p.parse_args()
    rows = read(a.samples)
    if a.order == "recency":
        rows.sort(key=lambda r: (-int((r.get("metadata") or {}).get("pr") or 0), r["id"]))
    funnel = {"mined_with_corpus_gates": Counter(r["repo"] for r in rows)}
    leak_drop: Counter = Counter()
    kept = []
    for r in rows:
        diag = detect_query_leakage(r)
        if diag["fatal"] or baseline.query_has_leakage(r, baseline.query_text_for_eval(r)):
            leak_drop.update(t for t, f in ((x["type"], x["fatal"]) for x in diag["flags"]) if f)
            continue
        kept.append(r)
    rows = kept
    funnel["after_arb_leakage_filter"] = Counter(r["repo"] for r in rows)
    def test_only(r):
        g = r["gold"].get("files") or []
        return bool(g) and all(is_test_file(x) for x in g)
    if a.per_repo_limit:
        # edit2ripple: at most floor(ratio * limit) test-only-gold rows per repo, so the test-only policy holds
        # per repo and globally instead of being enforced by dropping rows from whichever repos sort last.
        test_quota = math.floor(a.max_test_only_ratio * a.per_repo_limit) if a.task == "edit2ripple" else a.per_repo_limit
        seen: Counter = Counter()
        seen_test: Counter = Counter()
        limited = []
        for r in rows:
            if seen[r["repo"]] >= a.per_repo_limit:
                continue
            if a.task == "edit2ripple" and test_only(r):
                if seen_test[r["repo"]] >= test_quota:
                    continue
                seen_test[r["repo"]] += 1
            limited.append(r)
            seen[r["repo"]] += 1
        rows = limited
        funnel[f"after_per_repo_limit_{a.per_repo_limit}"] = Counter(r["repo"] for r in rows)
    test_only_dropped = 0
    cap = None
    for _ in range(20):
        if a.task == "edit2ripple":
            while rows and sum(map(test_only, rows)) / len(rows) > a.max_test_only_ratio:
                idx = max(i for i, r in enumerate(rows) if test_only(r))
                rows.pop(idx)
                test_only_dropped += 1
        before = len(rows)
        rows, cap = cap_rows(rows, a.repo_share)
        if len(rows) == before:
            break
    funnel["after_test_only_policy_and_repo_cap"] = Counter(r["repo"] for r in rows)
    for r in rows:
        if a.task == "code2test":
            r["query_provenance"] = "pr_summary"
        else:
            r["audit"] = {"notes": AUDIT_NOTE, "verdict": "pending"}
    a.out.mkdir(parents=True, exist_ok=True)
    body = "".join(json.dumps(r, ensure_ascii=False, sort_keys=True) + "\n" for r in rows)
    (a.out / "samples.jsonl").write_text(body)
    (a.out / f"{a.task}.jsonl").write_text(body)
    manifest = {
        "name": f"v2_{a.task}",
        "schema": "arb_extdev_v2",
        "task_type": a.task,
        "sample_count": len(rows),
        "repo_counts": dict(sorted(Counter(r["repo"] for r in rows).items())),
        "task_counts": {a.task: len(rows)},
        "paths": {"benchmark": f"data/benchmark/v2_{a.task}", "corpus": f"data/corpus/v2_{a.task}"},
        "selection": {
            "fatal_leakage_dropped_by_type": dict(leak_drop),
            "test_only_dropped": test_only_dropped,
            "test_only_ratio": (sum(map(test_only, rows)) / len(rows)) if a.task == "edit2ripple" and rows else None,
            "repo_cap": cap,
            "repo_share": a.repo_share,
            "per_repo_limit": a.per_repo_limit,
            "order": a.order,
        },
        "funnel": {k: dict(sorted(v.items())) for k, v in funnel.items()},
        "audit": AUDIT_NOTE,
    }
    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
