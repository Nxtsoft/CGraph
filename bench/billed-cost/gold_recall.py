"""Did the agent's patch reach every file the fix needs?

For each run, compares the source files its patch edits with the source (.py)
files the instance's gold patch edits. Reports per arm the share of runs whose
patch misses at least one gold source file, and mean gold-file recall, paired by
task (mean of each task's seeds) with bootstrap CIs, like analyze.py. Tasks whose
gold patch edits fewer than two source files are skipped: there is nothing to
miss once the one file is found.

This is the pre-edit hook's own measure: the hook names the files likely to
change with an edit, so it can only help a run that would otherwise leave one out.

usage: gold_recall.py --runs <work>/runs --tasks tasks-multi.json [--arm-b cgraph] --out out.json
"""

import argparse
import json
import re
from pathlib import Path

import numpy as np
from datasets import load_dataset

from analyze import boot_ci

DIFF_FILE = re.compile(r"^diff --git a/(\S+)", re.M)


def source_files(patch):
    return {f for f in DIFF_FILE.findall(patch) if f.endswith(".py")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=Path, required=True)
    ap.add_argument("--tasks", type=Path, required=True)
    ap.add_argument("--arm-b", default="cgraph")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    arms = ("baseline", args.arm_b)
    gold = {r["instance_id"]: source_files(r["patch"])
            for r in load_dataset("SWE-bench/SWE-bench_Verified", split="test")}
    ids = [i for i in json.load(open(args.tasks))["instance_ids"] if len(gold[i]) >= 2]
    per_task = []
    for iid in ids:
        row = {"instance_id": iid}
        for arm in arms:
            miss, recall = [], []
            for res in sorted((args.runs / iid).glob(f"{arm}-s*/result.json")):
                touched = source_files((res.parent / "patch.diff").read_text())
                miss.append(not gold[iid] <= touched)
                recall.append(len(touched & gold[iid]) / len(gold[iid]))
            if not miss:
                raise SystemExit(f"{iid}: no {arm} runs")
            row[f"{arm}_miss"], row[f"{arm}_recall"] = float(np.mean(miss)), float(np.mean(recall))
        per_task.append(row)
    out = {"arms": arms, "n_tasks": len(per_task), "metrics": {}}
    for metric in ("miss", "recall"):
        b = np.array([t[f"{arms[0]}_{metric}"] for t in per_task])
        c = np.array([t[f"{arms[1]}_{metric}"] for t in per_task])
        out["metrics"][metric] = {"baseline_mean": float(b.mean()), "arm_b_mean": float(c.mean()),
                                  "diff_mean": float((c - b).mean()), "diff_mean_ci95": boot_ci(c - b, np.mean)}
    out["per_task"] = per_task
    args.out.write_text(json.dumps(out, indent=2) + "\n")
    for metric, m in out["metrics"].items():
        print(f"{metric}: baseline {m['baseline_mean']:.3f}, {arms[1]} {m['arm_b_mean']:.3f}, "
              f"paired diff {m['diff_mean']:+.3f} [{m['diff_mean_ci95'][0]:+.3f}, {m['diff_mean_ci95'][1]:+.3f}]")


if __name__ == "__main__":
    main()
