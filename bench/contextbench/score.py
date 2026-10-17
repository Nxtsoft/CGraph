"""Score CGraph's ContextBench predictions with ContextBench's own file-level metric.

Gold files come from ContextBench's ``GoldLoader``/``Gold.files()`` (the union of
every gold-context span's file, path-normalized by ContextBench). Per-instance
file coverage (recall) and precision come from ContextBench's
``compute_granularity_metrics``: ``|pred & gold| / |gold|`` and
``|pred & gold| / |pred|``. The paper (arXiv 2602.05892v3, Appendix H) averages
every metric over tasks, so the headline is the macro mean; the micro aggregate
ContextBench's CLI prints (``aggregate_results``) is reported beside it. An
instance with an empty prediction is an error in ContextBench's evaluator
(``no_context_extracted``) and is left out of its averages; so is an instance CGraph
produced no output for (``no_prediction``, e.g. a failed build). Both are counted, and
the macro means with every such instance scored 0 are reported beside the headline.

Cutoffs are fixed before scoring and never chosen from the labels:

* ``default``: the file set of CGraph's ``context`` op at engine defaults (gate row).
* ``top-K`` for every K in 1..20 over the fused ranking: the full curve, no K picked.

95% confidence intervals are percentile bootstraps over instances (10,000
resamples, fixed seed).

Usage: score.py --contextbench-src DIR --gold PARQUET --instances instances.jsonl [--limit N]
                --details details.jsonl --out results.json [--markdown results.md]
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path
from typing import Any

BOOTSTRAP = 10_000
SEED = 2602
CURVE = range(1, 21)
TABLE_K = (1, 2, 3, 5, 10, 20)
GROUPS = ("overall", "Verified", "Pro", "Poly", "Multi")


def bootstrap_ci(values: list[float]) -> tuple[float, float]:
    rng = random.Random(SEED)
    n = len(values)
    means = sorted(sum(values[rng.randrange(n)] for _ in range(n)) / n for _ in range(BOOTSTRAP))
    return means[int(0.025 * BOOTSTRAP)], means[int(0.975 * BOOTSTRAP) - 1]


def summarize(results: list[dict[str, Any]], aggregate_results) -> dict[str, Any]:
    valid = [r for r in results if "error" not in r]
    out: dict[str, Any] = {"n": len(results), "valid": len(valid), "errors": len(results) - len(valid)}
    if not valid:
        return out
    for key in ("precision", "coverage"):
        values = [r["final"]["file"][key] for r in valid]
        lo, hi = bootstrap_ci(values)
        out[key] = {"mean": sum(values) / len(values), "ci95": [lo, hi],
                    "mean_errors_as_zero": sum(values) / len(results)}
    f1 = []
    for r in valid:
        p, c = r["final"]["file"]["precision"], r["final"]["file"]["coverage"]
        f1.append(2 * p * c / (p + c) if p + c > 0 else 0.0)
    out["f1"] = {"mean": sum(f1) / len(f1)}
    out["mean_files"] = sum(r["final"]["file"]["pred_size"] for r in valid) / len(valid)
    out["micro"] = aggregate_results(results)["final_file"]
    return out


def score_prediction(pred: list[str], gold_files: set[str], compute_granularity_metrics) -> dict[str, Any]:
    if not pred:
        return {"error": "no_context_extracted"}
    metrics = compute_granularity_metrics(set(pred), set(), {}, gold_files, set(), {})
    return {"final": {"file": metrics["file"]}}


def render_markdown(result: dict[str, Any]) -> str:
    lines = [
        "| Prediction | Group | n (valid) | Mean files | Precision [95% CI] | Recall [95% CI] | F1 | Micro P | Micro R | P, errors as 0 | R, errors as 0 |",
        "|---|---|---:|---:|---|---|---:|---:|---:|---:|---:|",
    ]
    for name in ["default", *(f"top-{k}" for k in TABLE_K)]:
        for group in GROUPS:
            row = result["predictions"][name][group]
            if not row.get("valid"):
                continue
            p, r = row["precision"], row["coverage"]
            lines.append(
                f"| {name} | {group} | {row['n']} ({row['valid']}) | {row['mean_files']:.2f} "
                f"| {p['mean']:.3f} [{p['ci95'][0]:.3f}, {p['ci95'][1]:.3f}] "
                f"| {r['mean']:.3f} [{r['ci95'][0]:.3f}, {r['ci95'][1]:.3f}] | {row['f1']['mean']:.3f} "
                f"| {row['micro']['precision']:.3f} | {row['micro']['coverage']:.3f} "
                f"| {p['mean_errors_as_zero']:.3f} | {r['mean_errors_as_zero']:.3f} |"
            )
    if result["no_prediction"]:
        lines += ["", "No CGraph output (scored as errors): " + ", ".join(result["no_prediction"])]
    lines += ["", "Full curve (overall, macro):", "", "| K | Precision | Recall | F1 |", "|---:|---:|---:|---:|"]
    for k in CURVE:
        row = result["predictions"][f"top-{k}"]["overall"]
        lines.append(f"| {k} | {row['precision']['mean']:.3f} | {row['coverage']['mean']:.3f} | {row['f1']['mean']:.3f} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--contextbench-src", type=Path, required=True, help="Checkout of EuniAI/ContextBench.")
    parser.add_argument("--gold", type=Path, required=True, help="ContextBench gold parquet.")
    parser.add_argument("--instances", type=Path, required=True, help="instances.jsonl from instances.py: the instances to score.")
    parser.add_argument("--limit", type=int, default=0, help="Score only the first N instances (as cgraph_eval.py --limit).")
    parser.add_argument("--details", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args(argv)
    sys.path.insert(0, str(args.contextbench_src))
    from contextbench.evaluate import aggregate_results  # noqa: PLC0415
    from contextbench.metrics import compute_granularity_metrics  # noqa: PLC0415
    from contextbench.parsers import GoldLoader  # noqa: PLC0415

    gold = GoldLoader(str(args.gold))
    universe = [json.loads(line) for line in args.instances.read_text(encoding="utf-8").splitlines() if line.strip()]
    if args.limit:
        universe = universe[: args.limit]
    details = [json.loads(line) for line in args.details.read_text(encoding="utf-8").splitlines() if line.strip()]
    by_id = {d["instance_id"]: d for d in details}
    unknown = set(by_id) - {u["id"] for u in universe}
    if unknown:
        raise SystemExit(f"details name instances not in {args.instances}: {sorted(unknown)[:5]}")
    predictions: dict[str, Any] = {}
    per_instance: dict[str, list[dict[str, Any]]] = {}

    def preds(d: dict[str, Any]) -> dict[str, list[str]]:
        out = {"default": d["default_context_files"]}
        for k in CURVE:
            out[f"top-{k}"] = d["ranked_files"][:k]
        return out

    missing = []
    for u in universe:
        d = by_id.get(u["id"])
        if d is None:
            missing.append(u["id"])
            for name in ("default", *(f"top-{k}" for k in CURVE)):
                per_instance.setdefault(name, []).append({"error": "no_prediction", "source": u["source"]})
            continue
        gold_files = set(gold.get(u["id"]).files())
        for name, pred in preds(d).items():
            row = score_prediction(pred, gold_files, compute_granularity_metrics)
            row["source"] = u["source"]
            per_instance.setdefault(name, []).append(row)
    for name, rows in per_instance.items():
        predictions[name] = {
            group: summarize(rows if group == "overall" else [r for r in rows if r["source"] == group], aggregate_results)
            for group in GROUPS
        }
    result = {"instances": len(universe), "no_prediction": missing, "predictions": predictions}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.markdown:
        args.markdown.write_text(render_markdown(result), encoding="utf-8")
    for name in ("default", *(f"top-{k}" for k in TABLE_K)):
        row = predictions[name]["overall"]
        if row.get("valid"):
            print(f"{name:8s} n={row['valid']}/{row['n']} files={row['mean_files']:.2f} "
                  f"P={row['precision']['mean']:.3f} {row['precision']['ci95']} "
                  f"R={row['coverage']['mean']:.3f} {row['coverage']['ci95']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
