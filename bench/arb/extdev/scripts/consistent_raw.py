"""Copy ARB raw records for PRs whose git-reconstructed diff equals the GitHub PR diff.

`arb backfill-git-raw` diffs `baseRefOid..headRefOid` (git_raw.py: `git diff --name-status --find-renames base head`).
GitHub's PR file list is the merge-base (three-dot) diff. The two agree exactly when baseRefOid is an ancestor of
headRefOid; otherwise the two-dot diff also contains every base-branch change made after the PR branched (pilot:
hono #5233 GraphQL changedFiles=2, git diff=311 files). Keep a PR only if base is an ancestor of head AND the git file
count equals GraphQL `changedFiles`. Writes pull_requests/pull_files/pull_commits/commit_details/pull_file_summary
for kept PRs into --out/<slug>/ and a consistency_summary.json with drop counts.
"""
from __future__ import annotations

import argparse
import json
import subprocess
from collections import Counter
from pathlib import Path

FILES = ("pull_requests.jsonl", "pull_files.jsonl", "pull_file_summary.jsonl", "pull_commits.jsonl", "commit_details.jsonl")


def pr_num(rec):
    return int(rec.get("pr_number") or (rec.get("data") or {}).get("number"))


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--raw", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--repos-dir", type=Path, required=True)
    p.add_argument("--repo", action="append", required=True)
    a = p.parse_args()
    total = {}
    for repo in a.repo:
        slug = repo.replace("/", "__", 1)
        src, dst, wt = a.raw / slug, a.out / slug, a.repos_dir / slug
        prs = {pr_num(r): r["data"] for r in map(json.loads, open(src / "pull_requests.jsonl"))}
        files = {pr_num(r): r["data"] for r in map(json.loads, open(src / "pull_files.jsonl"))}
        keep, why = set(), Counter()
        for n, pr in prs.items():
            if n not in files:
                why["no_git_backfill"] += 1
                continue
            r = subprocess.run(["git", "-C", str(wt), "merge-base", "--is-ancestor", pr["baseRefOid"], pr["headRefOid"]], capture_output=True)
            if r.returncode == 1:
                why["base_not_ancestor_of_head"] += 1
                continue
            if r.returncode != 0:
                why["ancestry_unknown"] += 1
                continue
            if len(files[n]) != int(pr.get("changedFiles") or -1):
                why["file_count_mismatch"] += 1
                continue
            keep.add(n)
        dst.mkdir(parents=True, exist_ok=True)
        for name in FILES:
            rows = [l for l in open(src / name) if l.strip() and pr_num(json.loads(l)) in keep]
            (dst / name).write_text("".join(rows))
        total[repo] = {"pull_requests": len(prs), "kept": len(keep), "dropped": dict(why)}
        (dst / "consistency_summary.json").write_text(json.dumps(total[repo], indent=2) + "\n")
    print(json.dumps(total, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
