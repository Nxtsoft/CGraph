"""Score ARB details files with the benchmark's own metric code.

Rank metrics (Recall@5/10/20, MRR) are the per-sample ``sample_metrics`` values
each evaluator stored, averaged per task and over all positive samples (the
leaderboard's 345-sample "overall"). BCY is recomputed for every method with
ARB's canonical ``bcy_curve.evaluate_run`` (token-based greedy file packing of
the stored top-20 ranking against the released corpus file texts), so CGraph and
the reproduced baselines are packed by exactly the same code.

Usage: score.py --arb-src DIR --data DIR --out results.json METHOD=DETAILS_DIR ...
where DETAILS_DIR holds ``<task>_details.jsonl`` (CGraph) or
``<task>_<method>_details.jsonl`` files (pass ``METHOD=DIR:suffix``).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

POSITIVE_TASKS = ("code2test", "comment2context", "trace2code", "edit2ripple")
RANK_KEYS = ("Recall@5", "Recall@10", "Recall@20", "MRR")
BUDGETS = (4_000, 8_000, 16_000, 32_000)


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def merged_manifest(data: Path, bcy_curve) -> dict[tuple[str, str], Path]:
    manifest: dict[tuple[str, str], Path] = {}
    for task in POSITIVE_TASKS:
        for key, rel in bcy_curve.load_corpus_manifest(data / "corpus" / f"v2_{task}" / "corpus_manifest.jsonl").items():
            manifest[key] = data.parent / rel
    return manifest


def score_method(details: list[dict[str, Any]], cache, bcy_curve) -> dict[str, Any]:
    positives = [d for d in details if d.get("gold_files")]
    bcy = bcy_curve.evaluate_run("method", "", Path("."), positives, cache, BUDGETS)
    out: dict[str, Any] = {}
    groups = {"overall": positives, **{t: [d for d in positives if d["task_type"] == t] for t in POSITIVE_TASKS}}
    for name, rows in groups.items():
        bcy_row = bcy["overall"] if name == "overall" else bcy["by_task"].get(name, {})
        out[name] = {
            "samples": len(rows),
            **{key: mean([float(r["metrics"][key]) for r in rows]) for key in RANK_KEYS},
            **{f"BCY@{b // 1000}k": float(bcy_row.get(f"BCY@{b}", 0.0)) for b in BUDGETS},
        }
    return out


def given_files(sample: dict[str, Any]) -> set[str]:
    """Files the query itself hands the agent: comment2context's given file, edit2ripple's anchor."""
    query = sample.get("query") or {}
    if sample.get("task_type") == "comment2context":
        return {p for p in (query.get("given_file"), query.get("path")) if p}
    if sample.get("task_type") == "edit2ripple":
        return {query["anchor_file"]} if query.get("anchor_file") else set()
    return set()


def score_given_excluded(details: list[dict[str, Any]], samples: dict[str, Any], cache, bcy_curve) -> dict[str, Any]:
    """Every method scored identically on its stored top 20 with the query's given/anchor file removed.

    Only the top 20 of each ranking is stored, so after removal a list can hold 19 files:
    Recall@20 here is a lower bound and MRR only sees ranks within the stored 20.
    """
    positives = []
    for d in details:
        if not d.get("gold_files"):
            continue
        drop = given_files(samples[d["sample_id"]])
        positives.append({**d, "top_files": [p for p in d.get("top_files") or [] if p not in drop]})
    bcy = bcy_curve.evaluate_run("method", "", Path("."), positives, cache, BUDGETS)
    out: dict[str, Any] = {}
    for name in ("overall", "comment2context", "edit2ripple"):
        rows = positives if name == "overall" else [d for d in positives if d["task_type"] == name]
        bcy_row = bcy["overall"] if name == "overall" else bcy["by_task"].get(name, {})

        def rr(d: dict[str, Any]) -> float:
            gold = set(d["gold_files"])
            return next((1.0 / i for i, p in enumerate(d["top_files"], start=1) if p in gold), 0.0)

        out[name] = {
            "samples": len(rows),
            "Recall@20 (lower bound)": mean([len(set(d["gold_files"]) & set(d["top_files"][:20])) / len(set(d["gold_files"])) for d in rows]),
            "MRR@20": mean([rr(d) for d in rows]),
            "BCY@8k": float(bcy_row.get("BCY@8000", 0.0)),
        }
    return out


def selective(details: list[dict[str, Any]]) -> dict[str, Any]:
    no_gold = [d for d in details if d.get("no_gold")]
    natural = [d for d in no_gold if d.get("organic")]
    counterfactual = [d for d in no_gold if not d.get("organic")]
    positives = [d for d in details if d.get("gold_files")]

    def hit20(d: dict[str, Any]) -> bool:
        return any(rank is not None and rank <= 20 for rank in (d.get("gold_ranks") or {}).values())

    return {
        "natural_no_gold": len(natural),
        "natural_abstained": sum(1 for d in natural if d.get("abstained")),
        "counterfactual_no_gold": len(counterfactual),
        "counterfactual_abstained": sum(1 for d in counterfactual if d.get("abstained")),
        "positive": len(positives),
        "positive_abstained": sum(1 for d in positives if d.get("abstained")),
        "positive_any_gold@20": sum(1 for d in positives if not d.get("abstained") and hit20(d)),
    }


def render_markdown(result: dict[str, Any]) -> str:
    cols = (*RANK_KEYS, *(f"BCY@{b // 1000}k" for b in BUDGETS))
    lines: list[str] = []
    for group in ("overall", *POSITIVE_TASKS):
        lines += [f"### {group}", "", "| Method | n | " + " | ".join(cols) + " |", "|---|---:|" + "---:|" * len(cols)]
        for label, entry in result["methods"].items():
            row = entry["metrics"][group]
            lines.append(f"| {label} | {row['samples']} | " + " | ".join(f"{row[c]:.4f}" for c in cols) + " |")
        lines.append("")
    lines += [
        "### Diagnostic: given/anchor file removed from every method's stored top 20",
        "",
        "Not the ARB protocol. Same rule applied to every row; Recall@20 is a lower bound and MRR sees ranks 1-20 only.",
        "",
        "| Method | Group | n | Recall@20 (lower bound) | MRR@20 | BCY@8k |",
        "|---|---|---:|---:|---:|---:|",
    ]
    for group in ("overall", "comment2context", "edit2ripple"):
        for label, entry in result["methods"].items():
            row = entry["given_excluded"][group]
            lines.append(
                f"| {label} | {group} | {row['samples']} | {row['Recall@20 (lower bound)']:.4f} | {row['MRR@20']:.4f} | {row['BCY@8k']:.4f} |"
            )
    lines.append("")
    for label, entry in result["methods"].items():
        if "selective" in entry:
            s = entry["selective"]
            lines += [
                f"### selective retrieval ({label})",
                "",
                "| Stratum | Samples | Abstained |",
                "|---|---:|---:|",
                f"| natural no-gold | {s['natural_no_gold']} | {s['natural_abstained']} |",
                f"| counterfactual no-gold | {s['counterfactual_no_gold']} | {s['counterfactual_abstained']} |",
                f"| positive | {s['positive']} | {s['positive_abstained']} (any gold in top 20: {s['positive_any_gold@20']}) |",
                "",
            ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arb-src", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, help="Also write the tables as Markdown.")
    parser.add_argument("methods", nargs="+", help="LABEL=DIR (CGraph layout) or LABEL=DIR:suffix (ARB baseline layout)")
    args = parser.parse_args(argv)
    sys.path.insert(0, str(args.arb_src / "src"))
    from agent_retrieval_bench import bcy_curve  # noqa: PLC0415

    cache = bcy_curve.CorpusFileCache(merged_manifest(args.data, bcy_curve))
    samples = {
        row["id"]: row
        for task in POSITIVE_TASKS
        for row in read_jsonl(args.data / "benchmark" / f"v2_{task}" / "samples.jsonl")
    }
    result: dict[str, Any] = {"methods": {}}
    for spec in args.methods:
        label, location = spec.split("=", 1)
        directory, _, suffix = location.partition(":")
        details: list[dict[str, Any]] = []
        for task in (*POSITIVE_TASKS, "abstention"):
            name = f"{task}_{suffix}_details.jsonl" if suffix else f"{task}_details.jsonl"
            path = Path(directory) / name
            if path.exists():
                details.extend(read_jsonl(path))
            elif task != "abstention":
                parser.error(f"{path} missing")
        entry = {
            "metrics": score_method(details, cache, bcy_curve),
            "given_excluded": score_given_excluded(details, samples, cache, bcy_curve),
        }
        if any(d.get("no_gold") for d in details):
            entry["selective"] = selective(details)
        result["methods"][label] = entry
        overall = entry["metrics"]["overall"]
        print(
            f"{label:12s} n={overall['samples']} "
            + " ".join(f"{k}={overall[k]:.4f}" for k in (*RANK_KEYS, "BCY@8k")),
            file=sys.stderr,
        )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.markdown:
        args.markdown.write_text(render_markdown(result), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
