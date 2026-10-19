"""Post-hoc op ablation for a CGraph ARB run.

cgraph_eval.py stores each op's file list (first 50 files) per sample. This
re-fuses subsets of those lists the same way the adapter does (RRF, corpus
candidates only) and reports Recall@20 and MRR. Because the
stored lists are cut at 50 files, the "all ops" row approximates the scored run
rather than reproducing it exactly, and MRR only sees the first 50 files.
Diagnostic only: the scored recipe was fixed before these numbers existed.
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

import cgraph_eval
from score import POSITIVE_TASKS, merged_manifest, read_jsonl

VARIANTS = {
    "context(q) only": lambda lists: [lists["context_text"]],
    "query(q) only": lambda lists: [lists["query_text"]],
    "context(q) + query(q)": lambda lists: [lists["context_text"], lists["query_text"]],
    "anchor ops only": lambda lists: [v for k, v in lists.items() if k.startswith("context_anchor") or k == "impact_changed"],
    "all ops, one vote each": lambda lists: list(lists.values()),
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arb-src", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("run", type=Path, help="cgraph_eval.py output dir")
    args = parser.parse_args(argv)
    sys.path.insert(0, str(args.arb_src / "src"))
    from agent_retrieval_bench import bcy_curve  # noqa: PLC0415

    manifest = merged_manifest(args.data, bcy_curve)
    candidates: dict[Path, set[str]] = {}
    scores: dict[str, dict[str, list[tuple[float, float]]]] = defaultdict(lambda: defaultdict(list))
    for task in POSITIVE_TASKS:
        for row in read_jsonl(args.run / f"{task}_details.jsonl"):
            chunks = manifest[(row["repo"], row["base_commit"])]
            if chunks not in candidates:
                candidates[chunks] = set(bcy_curve.load_file_texts(chunks))
            files = candidates[chunks]
            gold = set(row["gold_files"])
            for name, pick in [*VARIANTS.items(), ("scored recipe (cgraph_eval.fuse)", None)]:
                if pick is None:
                    fused = cgraph_eval.fuse(row["cgraph"]["lists"])
                else:
                    lists = pick(row["cgraph"]["lists"])
                    fused = cgraph_eval.rrf(lists) if lists else []
                ranked = [p for p in fused if p in files]
                recall = len(gold & set(ranked[:20])) / len(gold)
                rr = next((1.0 / i for i, p in enumerate(ranked, start=1) if p in gold), 0.0)
                scores[name]["overall"].append((recall, rr))
                scores[name][task].append((recall, rr))
    groups = ("overall", *POSITIVE_TASKS)
    print("| Ops fused | " + " | ".join(f"{g} R@20 / MRR" for g in groups) + " |")
    print("|---|" + "---:|" * len(groups))
    for name, by_group in scores.items():
        cells = []
        for g in groups:
            vals = by_group[g]
            if not vals:  # a split run (--split dev) can hold no sample of a task
                cells.append("-")
                continue
            cells.append(f"{sum(v[0] for v in vals) / len(vals):.4f} / {sum(v[1] for v in vals) / len(vals):.4f}")
        print(f"| {name} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
