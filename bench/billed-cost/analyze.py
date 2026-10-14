"""Paired analysis of the A/B runs: per-task cgraph-minus-baseline differences.

Each task's value for an arm is the mean over its seeds; the paired difference is
cgraph minus baseline per task. Means and medians of those differences carry a
95% percentile bootstrap CI (tasks resampled with replacement, fixed RNG seed).

usage: analyze.py --runs <work>/runs --tasks tasks.json --out results.json [--arm-b cgraph]
--runs also takes the runs/ dir extracted from the bench-cgr29 release tarball.
--arm-b cgraph_directed compares the diagnostic directed arm with the same baseline.
"""

import argparse
import gzip
import json
import statistics
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

ARMS = ["baseline", "cgraph"]
BOOT = 10000
RNG_SEED = 29
METRICS = ("total_cost_usd", "num_turns", "wall_s", "resolved",
           "input_tokens", "output_tokens", "cache_read_input_tokens",
           "cache_creation_input_tokens", "tool_calls_total")


def stream_facts(run_dir):
    """Claude Code version, and the prompt prefix of the first model request (cache
    write + cache read tokens): the system prompt and tool definitions the arm brings
    before any task work."""
    version = None
    with gzip.open(run_dir / "stream.jsonl.gz", "rt") as f:
        for line in f:
            ev = json.loads(line)
            if ev.get("subtype") == "init":
                version = ev["claude_code_version"]
            if ev.get("type") == "assistant":
                u = ev["message"]["usage"]
                return version, u["cache_creation_input_tokens"] + u["cache_read_input_tokens"]
    raise SystemExit(f"{run_dir}: no model request in the stream")


def load(runs_dir, ids):
    runs = []
    for iid in ids:
        for res in sorted((runs_dir / iid).glob("*/result.json")):
            rec = json.loads(res.read_text())
            if rec["arm"] not in ARMS:
                continue
            if "resolved" not in rec:
                raise SystemExit(f"{res} is not scored yet; run evaluate.py first")
            for k in ("input_tokens", "output_tokens", "cache_read_input_tokens",
                      "cache_creation_input_tokens"):
                rec[k] = rec["usage"][k]
            rec["resolved"] = float(rec["resolved"])
            rec["claude_code_version"], rec["first_request_prefix_tokens"] = stream_facts(res.parent)
            runs.append(rec)
    return runs


def boot_ci(values, stat):
    rng = np.random.default_rng(RNG_SEED)
    arr = np.asarray(values, dtype=float)
    idx = rng.integers(0, len(arr), size=(BOOT, len(arr)))
    samples = stat(arr[idx], axis=1)
    return float(np.percentile(samples, 2.5)), float(np.percentile(samples, 97.5))


def ratio_ci(base, cg):
    rng = np.random.default_rng(RNG_SEED)
    b, c = np.asarray(base), np.asarray(cg)
    idx = rng.integers(0, len(b), size=(BOOT, len(b)))
    r = c[idx].sum(axis=1) / b[idx].sum(axis=1)
    return float(c.sum() / b.sum()), float(np.percentile(r, 2.5)), float(np.percentile(r, 97.5))


def fit_prices(runs):
    """Least-squares $/MTok per token class from the runs' own billed totals, so the
    cost breakdown uses the prices the bill actually applied."""
    cols = ("input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens")
    x = np.array([[r[c] for c in cols] for r in runs], dtype=float)
    y = np.array([r["total_cost_usd"] for r in runs], dtype=float)
    coef, *_ = np.linalg.lstsq(x, y, rcond=None)
    resid = y - x @ coef
    return {c: float(v * 1e6) for c, v in zip(cols, coef)}, float(np.abs(resid).max())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=Path, required=True)
    ap.add_argument("--tasks", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--arm-b", default="cgraph")
    args = ap.parse_args()
    ARMS[1] = args.arm_b
    ids = json.load(open(args.tasks))["instance_ids"]
    runs = load(args.runs, ids)

    per = defaultdict(lambda: defaultdict(list))  # (iid, arm) -> metric -> [values over seeds]
    for r in runs:
        for m in METRICS + ("first_request_prefix_tokens",):
            per[(r["instance_id"], r["arm"])][m].append(r[m])
    tasks = [i for i in ids if all((i, a) in per for a in ARMS)]
    seeds = {a: Counter(len(per[(i, a)]["total_cost_usd"]) for i in tasks) for a in ARMS}

    out = {"arms": ARMS, "n_tasks": len(tasks), "n_runs": len(runs), "seeds_per_task": seeds,
           "claude_code_versions": dict(Counter(r["claude_code_version"] for r in runs)),
           "models": dict(Counter(r["init_model"] for r in runs)),
           "repos": sorted({r["repo"] for r in runs}), "metrics": {}}
    for m in METRICS + ("first_request_prefix_tokens",):
        base = [statistics.mean(per[(i, "baseline")][m]) for i in tasks]
        cg = [statistics.mean(per[(i, ARMS[1])][m]) for i in tasks]
        diff = [c - b for b, c in zip(base, cg)]
        entry = {
            "baseline_mean": statistics.mean(base), "cgraph_mean": statistics.mean(cg),
            "baseline_median": statistics.median(base), "cgraph_median": statistics.median(cg),
            "diff_mean": statistics.mean(diff), "diff_mean_ci95": boot_ci(diff, np.mean),
            "diff_median": statistics.median(diff), "diff_median_ci95": boot_ci(diff, np.median),
            "tasks_cgraph_lower": sum(d < 0 for d in diff), "tasks_cgraph_higher": sum(d > 0 for d in diff),
        }
        if m != "resolved":
            entry["ratio_of_totals"], entry["ratio_lo"], entry["ratio_hi"] = ratio_ci(base, cg)
        out["metrics"][m] = entry

    totals = {a: sum(r["total_cost_usd"] for r in runs if r["arm"] == a) for a in ARMS}
    out["spend_usd"] = {**totals, "all": sum(totals.values())}
    out["solved_runs"] = {a: f"{int(sum(r['resolved'] for r in runs if r['arm'] == a))}/"
                             f"{sum(r['arm'] == a for r in runs)}" for a in ARMS}

    cg_runs = [r for r in runs if r["arm"] == ARMS[1]]
    tool_names = Counter()
    for r in cg_runs:
        tool_names.update({k: v for k, v in r["tool_calls"].items() if k.startswith("mcp__plugin_cgraph")})
    used = [r for r in cg_runs if r["cgraph_tool_calls"] > 0]
    out["cgraph_usage"] = {
        "runs_with_any_cgraph_call": len(used), "cgraph_runs": len(cg_runs),
        "total_cgraph_calls": sum(r["cgraph_tool_calls"] for r in cg_runs),
        "skill_calls": sum(r["skill_calls"] for r in cg_runs),
        "by_tool": dict(tool_names.most_common()),
        "mcp_connected_runs": sum(any(s["status"] == "connected" for s in (r["init_mcp_servers"] or []))
                                  for r in cg_runs),
    }
    # Paired by (task, seed): cost delta when CGraph was and was not called.
    base_by = {(r["instance_id"], r["seed"]): r for r in runs if r["arm"] == "baseline"}
    split = {"called": [], "not_called": []}
    for r in cg_runs:
        b = base_by.get((r["instance_id"], r["seed"]))
        if b:
            key = "called" if r["cgraph_tool_calls"] > 0 else "not_called"
            split[key].append((r["total_cost_usd"] - b["total_cost_usd"], r["num_turns"] - b["num_turns"],
                               r["resolved"] - b["resolved"]))
    out["paired_by_cgraph_use"] = {
        k: {"n": len(v),
            "cost_diff_mean": statistics.mean(x[0] for x in v) if v else None,
            "turns_diff_mean": statistics.mean(x[1] for x in v) if v else None,
            "resolved_diff_mean": statistics.mean(x[2] for x in v) if v else None}
        for k, v in split.items()}

    prices, max_resid = fit_prices(runs)
    out["fitted_price_per_mtok"] = prices
    out["fitted_price_max_abs_residual_usd"] = max_resid
    breakdown = {}
    for a in ARMS:
        ar = [r for r in runs if r["arm"] == a]
        breakdown[a] = {c: sum(r[c] for r in ar) * prices[c] / 1e6 for c in prices}
    out["cost_breakdown_usd"] = breakdown

    out["per_task"] = [{"instance_id": i, **{f"{a}_{m}": statistics.mean(per[(i, a)][m])
                                             for a in ARMS for m in ("total_cost_usd", "num_turns", "wall_s", "resolved")}}
                       for i in tasks]
    args.out.write_text(json.dumps(out, indent=2))
    print(json.dumps({k: v for k, v in out.items() if k != "per_task"}, indent=2))


if __name__ == "__main__":
    main()
