"""Offline replay of the pre-edit hook on Agent Retrieval Bench samples (CGR-30).

For every edit2ripple and comment2context sample this treats the sample's
anchor file (edit2ripple ``anchor_file``, comment2context ``given_file``) as the
file an agent is about to edit, feeds the real ``cgraph-client pre-edit`` hook
the Claude Code PreToolUse input that edit would produce, and checks whether
the files the hook names include at least one gold file. Without the hook an
agent is shown no file before it edits, so the no-injection baseline names a
gold file on 0 samples; the share of samples where the injected list names
none is the hook's miss-all rate.

The simulated edit comes from the query, never from the labels:

* edit2ripple: each hunk header of ``anchor_diff`` names the base lines the
  change replaced; each becomes one ``old_string`` (a MultiEdit when there are
  several hunks).
* comment2context: the review comment's ``line`` in ``given_file``. GitHub
  numbers that line on the pull request's head side, and the snapshot is the
  base, so it is moved by the offset of the comment's own diff hunk
  (``@@ -old,n +new,m @@``: ``line + old - new``). The hunks ARB ships are cut
  short before the commented line, so the shift inside the hunk is unknown;
  a sample whose mapped line falls outside the base file is skipped.

An ``old_string`` that occurs more than once in the file is widened a line at a
time until it is unique, as Claude Code's Edit tool requires. Each snapshot gets
an empty ``.git`` directory so the hook finds the repository root, is built with
``cgraph``, and is served by a warm ``graphd`` (the hook runs after the daemon
reports ready), so the latency recorded is the warm-daemon hook latency,
process start included. ``--latency-repeats N`` reruns each hook N more times
and records their median (``latency_warm_ms``); run with ``--jobs 1`` so the
timings do not include other snapshots' builds competing for the CPU.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import subprocess
import sys
import threading
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

import cgraph_eval
import snapshots

TASKS = ("edit2ripple", "comment2context")
HUNK_HEADER_RE = re.compile(r"^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@")
LISTED_RE = re.compile(r"^- (.+?): ", re.MULTILINE)


def unique_old_string(lines: list[str], first: int, last: int, text: str) -> str:
    """Lines first..last (1-based, clamped), widened until they occur once in `text`."""
    first = max(1, min(first, len(lines)))
    last = max(first, min(last, len(lines)))
    while True:
        old = "\n".join(lines[first - 1 : last])
        if text.count(old) <= 1 or (first == 1 and last == len(lines)):
            return old
        first, last = max(1, first - 1), min(len(lines), last + 1)


def hook_input(sample: dict[str, Any], root: Path) -> tuple[str, dict[str, Any]] | str:
    """The edited file and the hook input for the sample's edit, or why there is none."""
    query = sample.get("query") or {}
    if sample["task_type"] == "edit2ripple":
        path = query.get("anchor_file")
        spans = []
        for match in cgraph_eval.HUNK_RE.finditer(query.get("anchor_diff") or ""):
            lo = int(match.group(1))
            spans.append((lo, lo + max(int(match.group(2) or 1), 1) - 1))
    else:
        path = query.get("given_file") or query.get("path")
        line = query.get("line")
        header = HUNK_HEADER_RE.match(query.get("diff_hunk_context") or "")
        if not isinstance(line, int) or header is None:
            return "comment has no line or no diff hunk header"
        base_line = line + int(header.group(1)) - int(header.group(2))
        spans = [(base_line, base_line)]
    if not path or not spans or not (root / path).is_file():
        return "edited file or edited lines not in the snapshot"
    text = (root / path).read_text(encoding="utf-8", errors="replace")
    lines = text.split("\n")
    if any(lo < 1 or hi > len(lines) for lo, hi in spans):
        return "edited line outside the base file"
    edits = []
    for lo, hi in spans:
        old = unique_old_string(lines, lo, hi, text)
        if old and old not in (edit["old_string"] for edit in edits):
            edits.append({"old_string": old, "new_string": old + "\n"})
    file_path = str((root / path).resolve())
    if len(edits) == 1:
        return path, {"hook_event_name": "PreToolUse", "tool_name": "Edit",
                      "tool_input": {"file_path": file_path, **edits[0]}}
    return path, {"hook_event_name": "PreToolUse", "tool_name": "MultiEdit",
                  "tool_input": {"file_path": file_path, "edits": edits}}


def run_hook(bin_dir: Path, payload: dict[str, Any]) -> tuple[float, str]:
    started = time.monotonic()
    proc = subprocess.run(
        [str(bin_dir / "cgraph-client"), "--daemon", str(bin_dir / "graphd"), "pre-edit"],
        input=json.dumps(payload), capture_output=True, text=True,
    )
    elapsed_ms = (time.monotonic() - started) * 1000
    if proc.returncode != 0:
        raise RuntimeError(f"pre-edit hook exited {proc.returncode}: {proc.stderr[-2000:]}")
    if not proc.stdout.strip():
        return elapsed_ms, ""
    return elapsed_ms, json.loads(proc.stdout)["hookSpecificOutput"]["additionalContext"]


def replay_snapshot(args, baseline, count_tokens, repo: str, sha: str, jobs: list[tuple[str, dict[str, Any]]]):
    root = args.work / f"{snapshots.repo_slug(repo)}-{sha[:12]}"
    built = (root / "cgraph-out" / "graph.json").is_file()
    if not built:
        if root.exists():
            snapshots.remove_tree(root)
        snapshots.materialize(args.cache, repo, sha, root)
    (root / ".git").mkdir(exist_ok=True)
    daemon = cgraph_eval.Daemon(args.cgraph_bin_dir, root)
    out = []
    pid = None
    try:
        if not built:
            daemon.build()
        status = daemon.wait_ready()
        pid = int(status["pid"])
        for task, sample in jobs:
            simulated = hook_input(sample, root)
            gold_all = baseline.target_gold_files(sample)
            row: dict[str, Any] = {"sample_id": sample["id"], "task_type": task, "repo": repo, "base_commit": sha}
            if isinstance(simulated, str):
                row.update({"skipped": simulated})
                out.append((task, row))
                continue
            edited, payload = simulated
            gold = [path for path in gold_all if path != edited]
            elapsed_ms, context = run_hook(args.cgraph_bin_dir, payload)
            repeats = [run_hook(args.cgraph_bin_dir, payload)[0] for _ in range(args.latency_repeats)]
            listed = LISTED_RE.findall(context.split("\n\n")[0]) if context.startswith("cgraph: files likely") else []
            row.update({
                "edited_file": edited,
                "tool_name": payload["tool_name"],
                "gold_files": gold,
                "injected_files": listed,
                "hit": bool(set(listed) & set(gold)),
                "gold_hits": [path for path in listed if path in gold],
                "context": context,
                "tokens_estimate": math.ceil(len(context) / 4),
                "tokens_arb": count_tokens(context),
                "latency_ms": elapsed_ms,
                "latency_warm_ms": statistics.median(repeats) if repeats else None,
                "engine_revision": status.get("engine_revision"),
            })
            out.append((task, row))
    finally:
        try:
            daemon.shutdown(pid)
        finally:
            if not args.keep_snapshots:
                snapshots.remove_tree(root)
    return out


def percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, math.ceil(q * len(ordered)) - 1))] if ordered else 0.0


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    scored = [row for row in rows if "hit" in row and row["gold_files"]]
    hits = sum(1 for row in scored if row["hit"])
    tokens = [row["tokens_estimate"] for row in scored]
    tokens_arb = [row["tokens_arb"] for row in scored]
    latency = [row["latency_ms"] for row in scored]
    warm = [row["latency_warm_ms"] for row in scored if row.get("latency_warm_ms") is not None]
    return {
        "samples": len(rows),
        "scored": len(scored),
        "skipped": {row["sample_id"]: row["skipped"] for row in rows if "skipped" in row},
        "no_gold_beyond_edited_file": sum(1 for row in rows if "hit" in row and not row["gold_files"]),
        "with_injection_hit": hits,
        "with_injection_hit_rate": hits / len(scored) if scored else 0.0,
        "with_injection_miss_all_rate": 1 - hits / len(scored) if scored else 0.0,
        "no_injection_hit_rate": 0.0,
        "no_injection_miss_all_rate": 1.0,
        "silent": sum(1 for row in scored if not row["injected_files"]),
        "injected_files_mean": statistics.fmean(len(row["injected_files"]) for row in scored) if scored else 0.0,
        "tokens_estimate_p50": percentile(tokens, 0.5),
        "tokens_estimate_p95": percentile(tokens, 0.95),
        "tokens_estimate_max": max(tokens, default=0),
        "tokens_arb_max": max(tokens_arb, default=0),
        "latency_ms_p50": percentile(latency, 0.5),
        "latency_ms_p95": percentile(latency, 0.95),
        "latency_ms_max": max(latency, default=0.0),
        "latency_warm_ms_p50": percentile(warm, 0.5),
        "latency_warm_ms_p95": percentile(warm, 0.95),
        "latency_warm_ms_max": max(warm, default=0.0),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arb-src", type=Path, required=True, help="Checkout of eyuansu62/agent-retrieval-bench.")
    parser.add_argument("--data", type=Path, required=True, help="ARB data dir (benchmark/ and corpus/ releases).")
    parser.add_argument("--cache", type=Path, required=True, help="Bare repo cache from snapshots.py.")
    parser.add_argument("--work", type=Path, required=True, help="Dir for materialized snapshots.")
    parser.add_argument("--out", type=Path, required=True, help="Output dir for <task>_pre_edit.jsonl and summary.json.")
    parser.add_argument("--cgraph-bin-dir", type=Path, required=True, help="Dir holding cgraph, cgraph-client and graphd.")
    parser.add_argument("--tasks", nargs="+", default=list(TASKS), choices=TASKS)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--latency-repeats", type=int, default=0,
                        help="Rerun each hook N more times and record the median as latency_warm_ms.")
    parser.add_argument("--keep-snapshots", action="store_true",
                        help="Keep each snapshot and its cgraph-out; a later run reuses a built one.")
    args = parser.parse_args(argv)
    args.cgraph_bin_dir = args.cgraph_bin_dir.resolve()
    args.work = args.work.resolve()
    args.cache = args.cache.resolve()
    baseline, _ = cgraph_eval.load_arb(args.arb_src)
    from agent_retrieval_bench.bcy_curve import count_tokens  # noqa: PLC0415  (ARB's own tokenizer)

    args.work.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    groups: dict[tuple[str, str], list[tuple[str, dict[str, Any]]]] = defaultdict(list)
    skipped: dict[str, int] = defaultdict(int)
    for task in args.tasks:
        manifest = cgraph_eval.load_manifest(args.data, task)
        for sample in cgraph_eval.load_samples(args.data, task):
            # The same sample set cgraph_eval.py scores.
            if baseline.query_has_leakage(sample, baseline.query_text_for_eval(sample)):
                skipped[f"{task}:query_leakage"] += 1
                continue
            chunks = manifest.get((sample["repo"], sample["base_commit"]))
            if chunks is None or not chunks.exists():
                skipped[f"{task}:missing_corpus"] += 1
                continue
            groups[(sample["repo"], sample["base_commit"])].append((task, sample))
    total = sum(len(v) for v in groups.values())
    print(f"{total} samples over {len(groups)} snapshots; skipped {dict(skipped)}", file=sys.stderr)

    rows: dict[str, list[dict[str, Any]]] = defaultdict(list)
    lock = threading.Lock()
    failures: list[str] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(replay_snapshot, args, baseline, count_tokens, repo, sha, jobs): (repo, sha)
            for (repo, sha), jobs in sorted(groups.items(), key=lambda item: -len(item[1]))
        }
        for future in as_completed(futures):
            repo, sha = futures[future]
            try:
                done = future.result()
            except Exception as exc:  # report and keep going; the run exits non-zero
                failures.append(f"{repo}@{sha}: {exc}")
                print(f"FAILED {repo}@{sha}: {exc}", file=sys.stderr)
                continue
            with lock:
                for task, row in done:
                    rows[task].append(row)
                print(f"{repo}@{sha[:12]}: {len(done)} samples", file=sys.stderr)
    summary = {}
    for task in args.tasks:
        ordered = sorted(rows[task], key=lambda row: row["sample_id"])
        (args.out / f"{task}_pre_edit.jsonl").write_text(
            "".join(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n" for row in ordered), encoding="utf-8")
        summary[task] = summarize(ordered)
    summary["overall"] = summarize([row for task in args.tasks for row in rows[task]])
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    if failures:
        print(f"{len(failures)} snapshot(s) failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
