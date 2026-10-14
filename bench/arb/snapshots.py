"""Fetch and materialize the exact repository snapshots Agent Retrieval Bench samples name.

The released ARB corpus is chunked and truncated, so CGraph has to index real
checkouts. Each upstream repository gets one bare cache repo under
``<cache>/<owner>__<name>.git``; every needed base commit is fetched shallowly
(GitHub serves any reachable SHA) and later materialized into a plain directory
with ``read-tree`` + ``checkout-index`` (``git archive`` would drop files marked
``export-ignore``, which some repos use for their tests).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def repo_slug(repo: str) -> str:
    return repo.replace("/", "__")


def bare_path(cache: Path, repo: str) -> Path:
    return cache / f"{repo_slug(repo)}.git"


def git(*args: str, git_dir: Path | None = None, env: dict[str, str] | None = None) -> str:
    cmd = ["git"]
    if git_dir is not None:
        cmd += ["--git-dir", str(git_dir)]
    cmd += list(args)
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed ({proc.returncode}): {proc.stderr.strip()}")
    return proc.stdout


def has_commit(git_dir: Path, sha: str) -> bool:
    proc = subprocess.run(
        ["git", "--git-dir", str(git_dir), "cat-file", "-e", f"{sha}^{{commit}}"],
        capture_output=True,
    )
    return proc.returncode == 0


def ensure_repo(cache: Path, repo: str, shas: list[str]) -> tuple[str, int]:
    git_dir = bare_path(cache, repo)
    if not git_dir.exists():
        git("init", "--bare", "--quiet", str(git_dir))
        git("remote", "add", "origin", f"https://github.com/{repo}.git", git_dir=git_dir)
    missing = [sha for sha in shas if not has_commit(git_dir, sha)]
    # Batches keep the fetch command line short and let a transient failure retry cheaply.
    for start in range(0, len(missing), 16):
        batch = missing[start : start + 16]
        git("fetch", "--quiet", "--depth", "1", "--no-tags", "origin", *batch, git_dir=git_dir)
    still = [sha for sha in shas if not has_commit(git_dir, sha)]
    if still:
        raise RuntimeError(f"{repo}: commits not fetched: {still}")
    return repo, len(missing)


def materialize(cache: Path, repo: str, sha: str, dest: Path) -> Path:
    """Write the tree of ``sha`` into ``dest`` (which must not exist)."""
    git_dir = bare_path(cache, repo)
    dest.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="arb-index-") as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
        git("read-tree", sha, git_dir=git_dir, env=env)
        git("--work-tree", str(dest), "checkout-index", "--all", "--force", git_dir=git_dir, env=env)
    return dest


def remove_tree(path: Path) -> None:
    shutil.rmtree(path, ignore_errors=False)


def samples_by_repo(sample_files: list[Path]) -> dict[str, list[str]]:
    needed: dict[str, set[str]] = defaultdict(set)
    for path in sample_files:
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                row = json.loads(line)
                needed[row["repo"]].add(row["base_commit"])
    return {repo: sorted(shas) for repo, shas in needed.items()}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True, help="Directory for bare per-repo caches.")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("samples", nargs="+", type=Path, help="ARB samples.jsonl files.")
    args = parser.parse_args(argv)
    args.cache.mkdir(parents=True, exist_ok=True)
    needed = samples_by_repo(args.samples)
    total = sum(len(v) for v in needed.values())
    print(f"{len(needed)} repos, {total} snapshots", file=sys.stderr)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(ensure_repo, args.cache, repo, shas) for repo, shas in sorted(needed.items())]
        for future in futures:
            repo, fetched = future.result()
            print(f"{repo}: fetched {fetched}, have {len(needed[repo])}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
