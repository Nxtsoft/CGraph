"""Agent Retrieval Bench (ARB) adapter for CGraph.

For every ARB sample this builds a CGraph graph of the real repository at the
sample's base commit, asks the resident daemon (through ``cgraph-client``) the
same things an agent host would ask, collapses the ranked file:line hits into a
deduplicated file ranking, and scores it with ARB's own ``sample_metrics`` so the
numbers sit on the same scale as the published leaderboard.

Retrieval recipe (fixed for every sample; nothing reads the gold labels):

* text: the sample's query fields, joined. ``context`` with ``q=text`` and
  ``query`` with ``q=text`` each yield a ranked node list.
* anchors: file references the query itself carries (``given_file``/``path``,
  ``anchor_file``, ``changed_file``, ``implementation_files`` and any snapshot
  path quoted in the text, e.g. a trace frame ``pkg/x.go:42``). A reference with
  a line resolves to the innermost symbol spanning that line, otherwise to the
  file node. Each anchor seed gets ``context`` with ``id=seed``.
* edit2ripple: the anchor diff's hunk headers name the edited base lines; the
  symbols overlapping them are the changed symbols and each gets ``impact``
  (dependents). This is change-context's impact stage; ``cgraph change-context``
  itself needs an applicable diff, and ARB's anchor diffs are excerpts (30 of 58
  end in ``...[truncated]``) that ``git apply`` rejects.
* Each op's node list is collapsed to files in first-hit order and the lists are
  merged with reciprocal-rank fusion (k=60). Files the task already hands the
  agent (comment2context ``given_file``, edit2ripple ``anchor_file``) are dropped,
  since the task asks for *other* files. The ranking is restricted to ARB's
  official ``all_files`` candidate set (the files in the released corpus snapshot),
  the same candidate set every leaderboard method ranks.

No-gold (abstention) samples go through the identical recipe. CGraph has no
abstention signal; it abstains only when every op returns nothing.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

import snapshots

RRF_K = 60
CONTEXT_BUDGET = 8000
QUERY_LIMIT = 100
IMPACT_LIMIT = 200
MAX_ANCHOR_SEEDS = 8
TASKS = ("code2test", "comment2context", "trace2code", "edit2ripple", "abstention")
HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@", re.MULTILINE)
PATH_LINE_RE = re.compile(r"([A-Za-z0-9_./\-]+\.[A-Za-z0-9]+)(?::(\d+))?")


def load_arb(arb_src: Path):
    sys.path.insert(0, str(arb_src / "src"))
    from agent_retrieval_bench import baseline, bcy_curve  # noqa: PLC0415

    return baseline, bcy_curve


class Daemon:
    """One graph per snapshot: build with ``cgraph``, serve with ``graphd`` via ``cgraph-client``."""

    def __init__(self, bin_dir: Path, root: Path) -> None:
        self.bin_dir = bin_dir
        self.root = root

    def build(self) -> dict[str, Any]:
        started = time.monotonic()
        proc = subprocess.run(
            [str(self.bin_dir / "cgraph"), "--root", str(self.root), "--out", str(self.root / "cgraph-out")],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"cgraph build failed for {self.root}: {proc.stderr[-2000:]}")
        stats = json.loads((self.root / "cgraph-out" / "stats.json").read_text())
        return {"build_seconds": time.monotonic() - started, "stats": stats}

    def call(self, op: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        cmd = [
            str(self.bin_dir / "cgraph-client"),
            "--root",
            str(self.root),
            "--daemon",
            str(self.bin_dir / "graphd"),
            op,
        ]
        if params is not None:
            cmd.append(json.dumps(params))
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"cgraph-client {op} failed ({proc.returncode}): {proc.stderr[-2000:]}")
        reply = json.loads(proc.stdout)
        if not reply.get("ok"):
            raise RuntimeError(f"cgraph-client {op} not ok: {proc.stdout[-2000:]}")
        return reply["result"]

    def wait_ready(self, timeout: float = 1800.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while True:
            status = self.call("status")
            if status.get("build_state") == "ready":
                return status
            if time.monotonic() > deadline:
                raise TimeoutError(f"graphd for {self.root} not ready: {status.get('build_state')}")
            time.sleep(0.5)

    def shutdown(self, pid: int | None) -> None:
        self.call("shutdown")
        # The reply precedes the exit; graphd still writes under <root>/cgraph-out while
        # it stops, so wait for it before the snapshot directory is removed.
        deadline = time.monotonic() + 60
        while pid and Path(f"/proc/{pid}").exists() and time.monotonic() < deadline:
            time.sleep(0.1)


def query_text(sample: dict[str, Any]) -> str:
    parts: list[str] = []
    for key, value in sorted((sample.get("query") or {}).items()):
        if isinstance(value, str):
            parts.append(value)
        elif isinstance(value, list):
            parts.extend(str(item) for item in value if isinstance(item, str))
    return "\n".join(parts)


def given_paths(sample: dict[str, Any]) -> set[str]:
    query = sample.get("query") or {}
    task = sample.get("task_type")
    if task == "comment2context":
        return {p for p in (query.get("given_file"), query.get("path")) if p}
    if task == "edit2ripple":
        return {query["anchor_file"]} if query.get("anchor_file") else set()
    return set()


class SnapshotIndex:
    """File -> symbol spans from the build's graph.json, keyed by repo-relative path."""

    def __init__(self, root: Path) -> None:
        graph = json.loads((root / "cgraph-out" / "graph.json").read_text())
        prefix = str(root.resolve()) + "/"
        self.file_node: dict[str, str] = {}
        self.spans: dict[str, list[tuple[int, int, str]]] = defaultdict(list)
        self.node_count = len(graph["nodes"])
        self.edge_count = len(graph["links"])
        for node in graph["nodes"]:
            source = str(node.get("source_file") or "")
            if not source.startswith(prefix):
                continue
            rel = source[len(prefix) :]
            if node.get("type") == "file":
                self.file_node[rel] = node["id"]
                continue
            loc = node.get("source_location") or {}
            start, end = int(loc.get("start_line") or 0), int(loc.get("end_line") or 0)
            if start > 0 and end >= start:
                self.spans[rel].append((start, end, node["id"]))

    def symbol_at(self, path: str, line: int) -> str | None:
        best: tuple[int, str] | None = None
        for start, end, node_id in self.spans.get(path, []):
            if start <= line <= end and (best is None or end - start < best[0]):
                best = (end - start, node_id)
        return best[1] if best else None

    def symbols_overlapping(self, path: str, lo: int, hi: int) -> list[str]:
        hits = [(start, node_id) for start, end, node_id in self.spans.get(path, []) if start <= hi and end >= lo]
        # Outer containers (a class around an edited method) overlap too; keep the innermost
        # symbols by dropping any span that strictly contains another hit.
        spans = {node_id: (start, end) for start, end, node_id in self.spans.get(path, [])}
        inner = [
            node_id
            for _, node_id in sorted(hits)
            if not any(
                other != node_id
                and spans[node_id][0] <= spans[other][0]
                and spans[other][1] <= spans[node_id][1]
                and spans[other] != spans[node_id]
                for _, other in hits
            )
        ]
        return inner


def resolve_path(ref: str, files: set[str], by_suffix: dict[str, list[str]]) -> str | None:
    while ref.startswith("./"):
        ref = ref[2:]
    if ref in files:
        return ref
    candidates = by_suffix.get(ref.rsplit("/", 1)[-1], [])
    matches = [path for path in candidates if path.endswith("/" + ref) or path == ref]
    return matches[0] if len(matches) == 1 else None


def anchor_seeds(sample: dict[str, Any], text: str, index: SnapshotIndex, files: set[str]) -> list[str]:
    by_suffix: dict[str, list[str]] = defaultdict(list)
    for path in files:
        by_suffix[path.rsplit("/", 1)[-1]].append(path)
    query = sample.get("query") or {}
    refs: list[tuple[str, int | None]] = []
    if query.get("given_file") or query.get("path"):
        line = query.get("line")
        refs.append((query.get("given_file") or query.get("path"), int(line) if isinstance(line, int) else None))
    for key in ("anchor_file", "changed_file"):
        if isinstance(query.get(key), str):
            refs.append((query[key], None))
    for value in query.get("implementation_files") or []:
        if isinstance(value, str):
            refs.append((value, None))
    for match in PATH_LINE_RE.finditer(text):
        refs.append((match.group(1), int(match.group(2)) if match.group(2) else None))
    seeds: list[str] = []
    for ref, line in refs:
        path = resolve_path(ref, files, by_suffix)
        if path is None:
            continue
        seed = index.symbol_at(path, line) if line else None
        seed = seed or index.file_node.get(path)
        if seed and seed not in seeds:
            seeds.append(seed)
        if len(seeds) >= MAX_ANCHOR_SEEDS:
            break
    return seeds


def changed_symbols(sample: dict[str, Any], index: SnapshotIndex) -> list[str]:
    query = sample.get("query") or {}
    path = query.get("anchor_file")
    out: list[str] = []
    for match in HUNK_RE.finditer(query.get("anchor_diff") or ""):
        lo = int(match.group(1))
        hi = lo + max(int(match.group(2) or 1), 1) - 1
        for node_id in index.symbols_overlapping(path, lo, hi):
            if node_id not in out:
                out.append(node_id)
    return out


def node_files(nodes: list[dict[str, Any]], prefix: str) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for node in nodes:
        source = str(node.get("source_file") or "")
        if not source.startswith(prefix):
            continue
        rel = source[len(prefix) :]
        if rel not in seen:
            seen.add(rel)
            out.append(rel)
    return out


def context_nodes(result: dict[str, Any]) -> list[dict[str, Any]]:
    focus = result.get("focus")
    return ([focus] if isinstance(focus, dict) else []) + list(result.get("included") or [])


def rrf(lists: list[list[str]]) -> list[str]:
    score: dict[str, float] = defaultdict(float)
    first: dict[str, tuple[int, int]] = {}
    for li, ranked in enumerate(lists):
        for rank, path in enumerate(ranked, start=1):
            score[path] += 1.0 / (RRF_K + rank)
            first.setdefault(path, (rank, li))
    return sorted(score, key=lambda p: (-score[p], first[p], p))


def retrieve(daemon: Daemon, sample: dict[str, Any], index: SnapshotIndex, files: set[str]) -> dict[str, Any]:
    prefix = str(daemon.root.resolve()) + "/"
    text = query_text(sample)
    lists: dict[str, list[str]] = {}
    ops: dict[str, Any] = {}

    ctx = daemon.call("context", {"q": text, "budget": CONTEXT_BUDGET})
    lists["context_text"] = node_files(context_nodes(ctx), prefix)
    ops["context_text_included"] = len(ctx.get("included") or [])

    qry = daemon.call("query", {"q": text, "limit": QUERY_LIMIT})
    lists["query_text"] = node_files(qry.get("nodes") or [], prefix)
    ops["query_route"] = qry.get("route")
    ops["query_total"] = qry.get("total")

    seeds = anchor_seeds(sample, text, index, files)
    ops["anchor_seeds"] = seeds
    for i, seed in enumerate(seeds):
        res = daemon.call("context", {"id": seed, "budget": CONTEXT_BUDGET})
        lists[f"context_anchor_{i}"] = node_files(context_nodes(res), prefix)

    if sample.get("task_type") == "edit2ripple":
        changed = changed_symbols(sample, index)
        ops["changed_symbols"] = changed
        impacted: list[dict[str, Any]] = []
        for seed in changed:
            impacted.extend(daemon.call("impact", {"id": seed, "direction": "dependents", "limit": IMPACT_LIMIT})["nodes"])
        impacted.sort(key=lambda node: int(node.get("depth") or 0))  # stable: per-seed order kept within a depth
        lists["impact_changed"] = node_files(impacted, prefix)

    drop = given_paths(sample)
    fused = [path for path in rrf(list(lists.values())) if path not in drop]
    in_candidates = [path for path in fused if path in files]
    ops["dropped_non_candidate"] = len(fused) - len(in_candidates)
    return {"ranked_files": in_candidates, "lists": {k: v[:50] for k, v in lists.items()}, "ops": ops}


def load_samples(data: Path, task: str) -> list[dict[str, Any]]:
    path = data / "benchmark" / f"v2_{task}" / "samples.jsonl"
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def load_manifest(data: Path, task: str) -> dict[tuple[str, str], Path]:
    out: dict[tuple[str, str], Path] = {}
    for line in (data / "corpus" / f"v2_{task}" / "corpus_manifest.jsonl").read_text().splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        if row.get("status") == "ok":
            # chunks_path is recorded relative to the directory holding data/.
            out[(row["repo"], row["base_commit"])] = data.parent / row["chunks_path"]
    return out


def evaluate_snapshot(
    args: argparse.Namespace,
    baseline,
    bcy_curve,
    repo: str,
    sha: str,
    jobs: list[tuple[str, dict[str, Any], Path]],
) -> list[tuple[str, dict[str, Any]]]:
    root = args.work / f"{snapshots.repo_slug(repo)}-{sha[:12]}"
    if root.exists():
        snapshots.remove_tree(root)
    snapshots.materialize(args.cache, repo, sha, root)
    daemon = Daemon(args.cgraph_bin_dir, root)
    out: list[tuple[str, dict[str, Any]]] = []
    serving = False
    pid: int | None = None
    try:
        build = daemon.build()
        serving = True  # the first client call spawns graphd
        status = daemon.wait_ready()
        pid = int(status["pid"])
        index = SnapshotIndex(root)
        corpus_texts: dict[Path, dict[str, str]] = {}
        for task, sample, chunks_path in jobs:
            if chunks_path not in corpus_texts:
                corpus_texts[chunks_path] = bcy_curve.load_file_texts(chunks_path)
            texts = corpus_texts[chunks_path]
            files = set(texts)
            started = time.monotonic()
            got = retrieve(daemon, sample, index, files)
            elapsed = time.monotonic() - started
            ranked = got["ranked_files"]
            gold = baseline.target_gold_files(sample)
            hard_neg = baseline.hard_negative_files(sample)
            ranked_chunks = [{"path": p, "kind": "file", "symbol": "", "text": texts[p] or p} for p in ranked]
            detail = {
                "sample_id": sample.get("id"),
                "task_type": sample.get("task_type"),
                "repo": repo,
                "base_commit": sha,
                "candidate_filter": "all_files",
                "gold_files": gold,
                "hard_negative_files": hard_neg,
                "query_provenance": baseline.query_provenance(sample),
                "gold_ranks": baseline.gold_file_ranks(gold, ranked_chunks),
                "top_files": ranked[:20],
                "ranked_files_returned": len(ranked),
                # Diagnostic only, computed after ranking: is each gold file a node in the graph at all?
                "gold_in_graph": {path: path in index.file_node for path in gold},
                "abstained": not ranked,
                "no_gold": not gold,
                "organic": bool((sample.get("metadata") or {}).get("organic")),
                "metrics": baseline.sample_metrics(gold, ranked_chunks, hard_negative_files=hard_neg) if gold else None,
                "cgraph": {
                    "ops": got["ops"],
                    "lists": got["lists"],
                    "retrieve_seconds": elapsed,
                    "graph_nodes": index.node_count,
                    "graph_edges": index.edge_count,
                    "files_extracted": build["stats"]["files_total"],
                    "build_seconds": build["build_seconds"],
                    "engine_revision": status.get("engine_revision"),
                    "engine_version": status.get("engine_version"),
                },
            }
            out.append((task, detail))
    finally:
        try:
            # cgraph-client spawns a daemon when none is running, so only stop one we started.
            if serving:
                daemon.shutdown(pid)
        finally:
            if not args.keep_snapshots:
                snapshots.remove_tree(root)
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arb-src", type=Path, required=True, help="Checkout of eyuansu62/agent-retrieval-bench.")
    parser.add_argument("--data", type=Path, required=True, help="ARB data dir (benchmark/ and corpus/ releases).")
    parser.add_argument("--cache", type=Path, required=True, help="Bare repo cache from snapshots.py.")
    parser.add_argument("--work", type=Path, required=True, help="Scratch dir for materialized snapshots.")
    parser.add_argument("--out", type=Path, required=True, help="Output dir for <task>_details.jsonl.")
    installed = shutil.which("cgraph")
    parser.add_argument("--cgraph-bin-dir", type=Path, default=Path(installed).resolve().parent if installed else None,
                        help="Dir holding cgraph, cgraph-client and graphd (default: the release `cgraph` on PATH resolves to).")
    parser.add_argument("--tasks", nargs="+", default=list(TASKS), choices=TASKS)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--limit", type=int, default=0, help="Evaluate at most N samples per task (0 = all).")
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
    baseline, bcy_curve = load_arb(args.arb_src)
    args.work.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)

    done: dict[str, set[str]] = {}
    groups: dict[tuple[str, str], list[tuple[str, dict[str, Any], Path]]] = defaultdict(list)
    skipped: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    for task in args.tasks:
        details_path = args.out / f"{task}_details.jsonl"
        done[task] = set()
        if details_path.exists():
            done[task] = {json.loads(l)["sample_id"] for l in details_path.read_text().splitlines() if l.strip()}
        manifest = load_manifest(args.data, task)
        samples = load_samples(args.data, task)
        if args.limit:
            samples = samples[: args.limit]
        for sample in samples:
            if sample["id"] in done[task]:
                continue
            if task != "abstention" and baseline.query_has_leakage(sample, baseline.query_text_for_eval(sample)):
                skipped[task]["query_leakage"] += 1
                continue
            chunks = manifest.get((sample["repo"], sample["base_commit"]))
            if chunks is None or not chunks.exists():
                skipped[task]["missing_corpus"] += 1
                continue
            groups[(sample["repo"], sample["base_commit"])].append((task, sample, chunks))
    total = sum(len(v) for v in groups.values())
    print(f"{total} samples over {len(groups)} snapshots to evaluate; skipped {json.loads(json.dumps(skipped))}", file=sys.stderr)

    lock = threading.Lock()
    finished = 0
    failures: list[str] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(evaluate_snapshot, args, baseline, bcy_curve, repo, sha, jobs): (repo, sha)
            for (repo, sha), jobs in sorted(groups.items(), key=lambda item: -len(item[1]))
        }
        for future in as_completed(futures):
            repo, sha = futures[future]
            try:
                rows = future.result()
            except Exception as exc:  # report and keep the other snapshots going; the run exits non-zero
                failures.append(f"{repo}@{sha}: {exc}")
                print(f"FAILED {repo}@{sha}: {exc}", file=sys.stderr)
                continue
            with lock:
                for task, detail in rows:
                    with (args.out / f"{task}_details.jsonl").open("a", encoding="utf-8") as handle:
                        handle.write(json.dumps(detail, ensure_ascii=False, sort_keys=True) + "\n")
                finished += len(rows)
                print(f"[{finished}/{total}] {repo}@{sha[:12]} ({len(rows)} samples)", file=sys.stderr)
    if failures:
        print(f"{len(failures)} snapshot(s) failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
