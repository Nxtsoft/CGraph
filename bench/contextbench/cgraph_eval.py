"""ContextBench adapter for CGraph.

For every ContextBench Lite instance this checks the real repository out at the
instance's base commit, builds a CGraph graph, and asks the resident daemon
(through ``cgraph-client``) about the issue text. It reuses the Agent Retrieval
Bench adapter (``bench/arb/cgraph_eval.py``) for the snapshot lifecycle and the
retrieval recipe, so both benches measure the same CGraph behaviour.

Two predictions are stored per instance; nothing reads the gold labels (the
input file carries only repo, commit and issue text, see ``instances.py``):

* ``default_context_files``: the files of one ``context`` call with ``q`` = the
  issue text and every other parameter at the engine default (budget 6000
  tokens). This is the context CGraph hands an agent when asked about the issue,
  and the file set the gate is judged on.
* ``ranked_files``: the ARB recipe's fused file ranking (``context`` at budget
  8000, ``query`` with limit 100, and ``context`` around any file the issue text
  names, merged by reciprocal-rank fusion), restricted to files in the checked-out
  tree. score.py reports precision and recall of its top K for every K from 1 to 20.

Usage: cgraph_eval.py --instances instances.jsonl --cache DIR --work DIR --out DIR [--cgraph-bin-dir DIR] [--jobs N] [--limit N]
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "arb"))
import cgraph_eval as arb  # noqa: E402
import snapshots  # noqa: E402


def tree_files(cache: Path, repo: str, sha: str) -> set[str]:
    """Regular files and symlinks in the commit's tree: the paths ContextBench accepts as
    repository files (its evaluator drops predicted paths that are not files in the checkout)."""
    out = snapshots.git("ls-tree", "-r", "-z", "--full-tree", sha, git_dir=snapshots.bare_path(cache, repo))
    files: set[str] = set()
    for entry in out.split("\0"):
        if not entry:
            continue
        meta, path = entry.split("\t", 1)
        if meta.split()[1] == "blob":
            files.add(path)
    return files


def evaluate_instance(args: argparse.Namespace, inst: dict[str, Any]) -> dict[str, Any]:
    repo, sha = inst["repo"], inst["base_commit"]
    root = args.work / f"{snapshots.repo_slug(repo)}-{sha[:12]}"
    files = tree_files(args.cache, repo, sha)
    with arb.served_snapshot(args.cgraph_bin_dir, args.cache, repo, sha, root, args.keep_snapshots) as (daemon, build, status):
        index = arb.SnapshotIndex(root)
        prefix = str(root.resolve()) + "/"
        text = inst["problem_statement"]
        started = time.monotonic()
        default = daemon.call("context", {"q": text})
        default_files = [p for p in arb.context_files(arb.context_nodes(default), prefix) if p in files]
        sample = {"id": inst["id"], "task_type": "contextbench", "query": {"problem_statement": text}}
        got = arb.retrieve(daemon, sample, index, files)
        elapsed = time.monotonic() - started
        return {
            "instance_id": inst["id"],
            "original_inst_id": inst["original_inst_id"],
            "repo": repo,
            "base_commit": sha,
            "source": inst["source"],
            "language": inst["language"],
            "default_context_files": default_files,
            "default_context": {
                "budget": default.get("budget"),
                "tokens_used": default.get("tokens_used"),
                "included": len(default.get("included") or []),
                "focus": (default.get("focus") or {}).get("id"),
            },
            "ranked_files": got["ranked_files"][:50],
            "ranked_files_returned": len(got["ranked_files"]),
            "cgraph": {
                "ops": got["ops"],
                "lists": got["lists"],
                "retrieve_seconds": elapsed,
                "graph_nodes": index.node_count,
                "graph_edges": index.edge_count,
                "tree_files": len(files),
                "files_extracted": build["stats"]["files_total"],
                "build_seconds": build["build_seconds"],
                "engine_revision": status.get("engine_revision"),
                "engine_version": status.get("engine_version"),
            },
        }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--instances", type=Path, required=True, help="instances.jsonl from instances.py.")
    parser.add_argument("--cache", type=Path, required=True, help="Bare repo cache from bench/arb/snapshots.py.")
    parser.add_argument("--work", type=Path, required=True, help="Scratch dir for materialized snapshots.")
    parser.add_argument("--out", type=Path, required=True, help="Output dir for details.jsonl.")
    installed = shutil.which("cgraph")
    parser.add_argument("--cgraph-bin-dir", type=Path, default=Path(installed).resolve().parent if installed else None,
                        help="Dir holding cgraph, cgraph-client and graphd (default: the release `cgraph` on PATH resolves to).")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--limit", type=int, default=0, help="Evaluate only the first N instances (0 = all).")
    parser.add_argument("--keep-snapshots", action="store_true")
    args = parser.parse_args(argv)
    if args.cgraph_bin_dir is None:
        parser.error("no cgraph on PATH; pass --cgraph-bin-dir")
    args.cgraph_bin_dir = args.cgraph_bin_dir.resolve()
    args.work = args.work.resolve()
    args.cache = args.cache.resolve()
    for name in ("cgraph", "cgraph-client", "graphd"):
        if not (args.cgraph_bin_dir / name).is_file():
            parser.error(f"{args.cgraph_bin_dir / name} not found")
    args.work.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)

    details_path = args.out / "details.jsonl"
    done: set[str] = set()
    if details_path.exists():
        done = {json.loads(line)["instance_id"] for line in details_path.read_text().splitlines() if line.strip()}
    instances = [json.loads(line) for line in args.instances.read_text(encoding="utf-8").splitlines() if line.strip()]
    if args.limit:
        instances = instances[: args.limit]
    todo = [inst for inst in instances if inst["id"] not in done]
    print(f"{len(todo)} instances to evaluate ({len(done)} already done)", file=sys.stderr)

    lock = threading.Lock()
    finished = 0
    failures: list[str] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(evaluate_instance, args, inst): inst for inst in todo}
        for future in as_completed(futures):
            inst = futures[future]
            try:
                row = future.result()
            except Exception as exc:  # report and keep going; the run exits non-zero
                failures.append(f"{inst['id']} ({inst['repo']}@{inst['base_commit']}): {exc}")
                print(f"FAILED {inst['id']}: {exc}", file=sys.stderr)
                continue
            with lock:
                with details_path.open("a", encoding="utf-8") as handle:
                    handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")
                finished += 1
                print(f"[{finished}/{len(todo)}] {inst['id']} {inst['repo']}@{inst['base_commit'][:12]} "
                      f"build {row['cgraph']['build_seconds']:.0f}s", file=sys.stderr)
    if failures:
        print(f"{len(failures)} instance(s) failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
