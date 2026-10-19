"""Pick an A/B task set from SWE-bench Verified instances that are also in the
ContextBench Lite selection. Writes the instance ids only; the task data is read
from the SWE-bench Verified dataset at run time.

  main  difficulty "15 min - 1 hour", a fixed count per repo, sampled with a fixed seed
  hard  every instance of difficulty "1-4 hours" (a supplementary set where
        navigation should matter more)
  multi every SWE-bench Verified instance whose gold patch edits at least two files,
        with no ContextBench filter: the fix spans files the agent has to find

usage: select_tasks.py <main|hard> <contextbench selected_500_instances.csv> <out.json>
       select_tasks.py multi <out.json>
"""

import json
import random
import re
import sys

import pandas as pd
from datasets import load_dataset

PER_REPO = {
    "django/django": 10,
    "sympy/sympy": 8,
    "scikit-learn/scikit-learn": 6,
    "sphinx-doc/sphinx": 6,
    "pydata/xarray": 2,
}
DIFFICULTY = {"main": "15 min - 1 hour", "hard": "1-4 hours"}
SEED = 29


MIN_GOLD_FILES = 2


def multi(out_path):
    verified = load_dataset("SWE-bench/SWE-bench_Verified", split="test").to_pandas()
    files = verified.patch.map(lambda p: len(re.findall(r"^diff --git a/", p, re.M)))
    picked = sorted(verified[files >= MIN_GOLD_FILES].instance_id)
    meta = {"dataset": "SWE-bench/SWE-bench_Verified", "min_gold_patch_files": MIN_GOLD_FILES}
    json.dump({**meta, "instance_ids": picked}, open(out_path, "w"), indent=2)
    print(f"{len(picked)} tasks written to {out_path}")


def main(which, csv_path, out_path):
    selected = pd.read_csv(csv_path)
    lite = set(selected[selected.bench == "Verified"].original_inst_id)
    verified = load_dataset("SWE-bench/SWE-bench_Verified", split="test").to_pandas()
    pool = verified[verified.instance_id.isin(lite) & (verified.difficulty == DIFFICULTY[which])]
    meta = {"dataset": "SWE-bench/SWE-bench_Verified", "difficulty": DIFFICULTY[which]}
    if which == "main":
        rng = random.Random(SEED)
        picked = []
        for repo, count in PER_REPO.items():
            ids = sorted(pool[pool.repo == repo].instance_id)
            picked += sorted(rng.sample(ids, count))
        meta.update(seed=SEED, per_repo=PER_REPO)
    else:
        picked = sorted(pool.instance_id)
    json.dump({**meta, "instance_ids": picked}, open(out_path, "w"), indent=2)
    print(f"{len(picked)} tasks written to {out_path}")


if __name__ == "__main__":
    if sys.argv[1] == "multi":
        multi(sys.argv[2])
    else:
        main(*sys.argv[1:])
