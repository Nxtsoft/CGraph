"""Score every finished run with the official SWE-bench harness (Docker).

For each arm x seed it writes a predictions file from the runs' patch.diff, runs
`swebench.harness.run_evaluation` (applies the patch, applies the instance's test
patch, runs FAIL_TO_PASS and PASS_TO_PASS), and records `resolved` back into the
run's result.json. An empty patch is unresolved without running anything.

usage: evaluate.py --work /var/tmp/cgr29 --tasks tasks.json [--jobs 6]
"""

import argparse
import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

DATASET = "SWE-bench/SWE-bench_Verified"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--tasks", type=Path, required=True)
    ap.add_argument("--jobs", type=int, default=6)
    args = ap.parse_args()
    ids = json.load(open(args.tasks))["instance_ids"]
    eval_dir = args.work / "eval"
    eval_dir.mkdir(exist_ok=True)

    groups = defaultdict(list)  # model name -> [(result path, record)]
    for iid in ids:
        for result in sorted((args.work / "runs" / iid).glob("*/result.json")):
            rec = json.loads(result.read_text())
            groups[f"{rec['arm']}-s{rec['seed']}"].append((result, rec))

    for model, runs in sorted(groups.items()):
        todo = [(p, r) for p, r in runs if "resolved" not in r]
        if not todo:
            continue
        preds = [{"instance_id": r["instance_id"], "model_name_or_path": model,
                  "model_patch": (p.parent / "patch.diff").read_text()} for p, r in todo]
        pred_path = eval_dir / f"{model}.json"
        pred_path.write_text(json.dumps(preds))
        nonempty = [x["instance_id"] for x in preds if x["model_patch"].strip()]
        status = {x["instance_id"]: "empty_patch" for x in preds if not x["model_patch"].strip()}
        if nonempty:
            subprocess.run([sys.executable, "-m", "swebench.harness.run_evaluation",
                            "-d", DATASET, "-p", str(pred_path), "-id", model,
                            "--max_workers", str(args.jobs), "-i", *nonempty],
                           cwd=eval_dir, check=True)
            summary = json.loads((eval_dir / f"{model}.{model}.json").read_text())
            for key in ("resolved", "unresolved", "error", "infra_failure", "ambiguous_failure"):
                for iid in summary[f"{key}_ids"]:
                    status[iid] = key
            # "ambiguous" is the harness's guess at a cause (e.g. missing_module) for a
            # graded run; the instance report still holds the graded outcome, and the
            # gold patch resolves these instances, so the grade stands.
            for iid, reason in summary["failure_reasons"].items():
                status[iid] = f"{status[iid]}:{reason}"
        retry = [iid for iid in nonempty if iid not in status or status[iid].startswith("infra_failure")]
        for path, rec in todo:
            iid = rec["instance_id"]
            if iid in retry:
                continue  # infrastructure failure: left unscored so the next pass re-runs it
            rec["eval_status"] = status[iid]
            if status[iid].startswith("ambiguous_failure"):
                report = eval_dir / "logs" / "run_evaluation" / model / model / iid / "report.json"
                rec["resolved"] = json.loads(report.read_text())[iid]["resolved"]
            else:
                rec["resolved"] = status[iid] == "resolved"
            path.write_text(json.dumps(rec, indent=2))
        if retry:
            print(f"{model}: not scored, re-run evaluate.py: {retry}")
        solved = sum(r.get("resolved", False) for _, r in todo)
        print(f"{model}: {solved}/{len(todo)} resolved")


if __name__ == "__main__":
    main()
