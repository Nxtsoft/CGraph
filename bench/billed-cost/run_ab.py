"""Run the paired A/B: each task x arm x seed is one headless Claude Code run.

Arms
  baseline  Claude Code with no plugins.
  cgraph    Claude Code with the CGraph plugin installed from GitHub
            (`claude plugin marketplace add Nxtsoft/CGraph`,
            `claude plugin install cgraph@cgraph`; the launcher pins bin-v0.9.1).
  cgraph_directed
            Diagnostic only, not part of the gate: the cgraph arm's install with
            one sentence added to the prompt telling the agent to use the graph
            tools, to measure what CGraph costs when it is actually called.

Both arms use the same binary, model, prompt and permission mode. Every run gets
its own HOME and CLAUDE_CONFIG_DIR, copied from a per-arm template made once by
`run_ab.py templates`, and its own checkout of the instance's base commit with a
single-commit history (so the fix cannot be read out of git).

Run it under the auth bundle so CLAUDE_CODE_OAUTH_TOKEN is set:
  secrets exec anthropic -- python run_ab.py templates --work /var/tmp/cgr29
  secrets exec anthropic -- python run_ab.py run --work /var/tmp/cgr29 \
      --tasks tasks.json --seeds 1 2 --jobs 6

Each run leaves <work>/runs/<instance>/<arm>-s<seed>/ with result.json,
patch.diff and stream.jsonl.gz (the full stream-json transcript). The checkout and
config dirs live under <work>/w/<hash>/ so no path the agent sees names its arm.
"""

import argparse
import concurrent.futures
import gzip
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

from datasets import load_dataset

CLAUDE = "/home/taylor/.agents/.history/versions/claude/2.1.260/node_modules/.bin/claude"
MODEL = "claude-sonnet-5-5"
ARMS = ("baseline", "cgraph")
TEMPLATE = {"baseline": "baseline", "cgraph": "cgraph", "cgraph_directed": "cgraph"}
MAX_BUDGET_USD = "5"
RUN_TIMEOUT_S = 40 * 60
CGRAPH_TOOL_PREFIX = "mcp__plugin_cgraph_cgraph__"
# Files the CGraph daemon writes into the project root; they are not part of the fix.
EXCLUDE_FROM_PATCH = ("cgraph-out",)

PROMPT = """Fix the following issue in the repository in the current directory.

Edit the non-test source files so that the issue is resolved. Do not add or modify tests. \
The project's dependencies are not installed in this environment, so do not try to install \
packages or run the project's test suite. Stop when the change is complete.

<issue>
{problem_statement}
</issue>
"""
DIRECTED = """Use the cgraph code-graph tools (the cgraph skill and the \
mcp__plugin_cgraph_cgraph__graph_* tools) to locate and understand the relevant code \
before reading or editing files.

"""


def mirror_path(work, repo):
    return work / "mirrors" / (repo.replace("/", "__") + ".git")


def clean_env(home, config_dir):
    """The run's environment: nothing from the calling Claude session leaks in."""
    return {
        "PATH": os.environ["PATH"],
        "LANG": os.environ.get("LANG", "C.UTF-8"),
        "HOME": str(home),
        "CLAUDE_CONFIG_DIR": str(config_dir),
        "CLAUDE_CODE_OAUTH_TOKEN": os.environ["CLAUDE_CODE_OAUTH_TOKEN"],
        "DISABLE_AUTOUPDATER": "1",
    }


def make_templates(work):
    """One config dir per arm. The cgraph arm installs the plugin from GitHub."""
    for arm in ARMS:
        tpl = work / "templates" / arm
        if tpl.exists():
            shutil.rmtree(tpl)
        (tpl / "home").mkdir(parents=True)
        (tpl / "cfg").mkdir()
        env = clean_env(tpl / "home", tpl / "cfg")
        if arm == "cgraph":
            for cmd in (["plugin", "marketplace", "add", "Nxtsoft/CGraph"],
                        ["plugin", "install", "cgraph@cgraph"]):
                subprocess.run([CLAUDE, *cmd], env=env, check=True)
        listing = subprocess.run([CLAUDE, "plugin", "list"], env=env, check=True,
                                 capture_output=True, text=True).stdout
        print(f"--- {arm} template plugins ---\n{listing}")


def copy_config(work, arm, sandbox):  # arm: a template name
    """Copy the arm template; plugin manifests hold absolute paths, so rewrite them."""
    tpl = work / "templates" / arm
    for part in ("home", "cfg"):
        shutil.copytree(tpl / part, sandbox / part, symlinks=True)
    for manifest in (sandbox / "cfg" / "plugins").glob("*.json") if arm == "cgraph" else ():
        text = manifest.read_text().replace(str(tpl / "cfg"), str(sandbox / "cfg"))
        manifest.write_text(text)


def checkout(work, inst, repo_dir):
    """Base commit as a fresh one-commit repository (no upstream history to mine)."""
    mirror = mirror_path(work, inst["repo"])
    repo_dir.mkdir(parents=True)
    index = repo_dir.parent / "checkout.index"
    git_env = {**os.environ, "GIT_INDEX_FILE": str(index)}
    subprocess.run(["git", "--git-dir", str(mirror), "read-tree", inst["base_commit"]],
                   env=git_env, check=True)
    subprocess.run(["git", "--git-dir", str(mirror), "--work-tree", str(repo_dir),
                    "checkout-index", "-a"], env=git_env, check=True)
    index.unlink()
    ident = {**os.environ, "GIT_AUTHOR_NAME": "bench", "GIT_AUTHOR_EMAIL": "bench@local",
             "GIT_COMMITTER_NAME": "bench", "GIT_COMMITTER_EMAIL": "bench@local"}
    for cmd in (["init", "-q"], ["add", "-A", "-f"], ["commit", "-q", "-m", "base"]):
        subprocess.run(["git", "-C", str(repo_dir), *cmd], env=ident, check=True)


def extract_patch(repo_dir):
    excludes = [f":(exclude){p}" for p in EXCLUDE_FROM_PATCH]
    subprocess.run(["git", "-C", str(repo_dir), "add", "-A", "--", ".", *excludes], check=True)
    return subprocess.run(["git", "-C", str(repo_dir), "diff", "--cached", "HEAD"],
                          check=True, capture_output=True, text=True).stdout


def stop_graphd(repo_dir):
    """Stop the daemon the plugin started for this checkout (it outlives the session)."""
    out = subprocess.run(["pgrep", "-f", f"graphd --root {repo_dir}"],
                         capture_output=True, text=True).stdout.split()
    for pid in out:
        os.kill(int(pid), signal.SIGTERM)
    return len(out)


def summarize(events):
    tools = {}
    result = None
    init = None
    for ev in events:
        if ev.get("type") == "system" and ev.get("subtype") == "init":
            init = ev
        if ev.get("type") == "assistant":
            for block in ev["message"].get("content", []):
                if block.get("type") == "tool_use":
                    tools[block["name"]] = tools.get(block["name"], 0) + 1
        if ev.get("type") == "result":
            result = ev
    cgraph_calls = sum(n for name, n in tools.items() if name.startswith(CGRAPH_TOOL_PREFIX))
    return {
        "init_tools": init.get("tools") if init else None,
        "init_mcp_servers": init.get("mcp_servers") if init else None,
        "init_model": init.get("model") if init else None,
        "result_subtype": result.get("subtype") if result else None,
        "is_error": result.get("is_error") if result else None,
        "total_cost_usd": result.get("total_cost_usd") if result else None,
        "num_turns": result.get("num_turns") if result else None,
        "duration_ms": result.get("duration_ms") if result else None,
        "duration_api_ms": result.get("duration_api_ms") if result else None,
        "usage": result.get("usage") if result else None,
        "model_usage": result.get("modelUsage") if result else None,
        "tool_calls": tools,
        "tool_calls_total": sum(tools.values()),
        "cgraph_tool_calls": cgraph_calls,
        "skill_calls": tools.get("Skill", 0),
    }


def run_one(work, inst, arm, seed):
    run_dir = work / "runs" / inst["instance_id"] / f"{arm}-s{seed}"
    if (run_dir / "result.json").exists():
        return json.loads((run_dir / "result.json").read_text())
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    key = hashlib.sha256(f"{inst['instance_id']}/{arm}/{seed}".encode()).hexdigest()[:16]
    sandbox = work / "w" / key
    if sandbox.exists():
        shutil.rmtree(sandbox)
    repo_dir = sandbox / "repo"
    checkout(work, inst, repo_dir)
    copy_config(work, TEMPLATE[arm], sandbox)
    prompt = PROMPT.format(problem_statement=inst["problem_statement"])
    if arm == "cgraph_directed":
        prompt = DIRECTED + prompt
    cmd = [CLAUDE, "-p", prompt,
           "--model", MODEL, "--output-format", "stream-json", "--verbose",
           "--dangerously-skip-permissions", "--max-budget-usd", MAX_BUDGET_USD]
    started = time.time()
    with open(run_dir / "stream.jsonl", "w") as out, open(run_dir / "stderr.log", "w") as err:
        proc = subprocess.Popen(cmd, cwd=repo_dir, env=clean_env(sandbox / "home", sandbox / "cfg"),
                                stdout=out, stderr=err, stdin=subprocess.DEVNULL,
                                start_new_session=True)
        try:
            exit_code = proc.wait(timeout=RUN_TIMEOUT_S)
            timed_out = False
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            exit_code = proc.wait()
            timed_out = True
    wall_s = time.time() - started
    graphd_stopped = stop_graphd(repo_dir)
    events = [json.loads(line) for line in open(run_dir / "stream.jsonl") if line.strip()]
    patch = extract_patch(repo_dir)
    (run_dir / "patch.diff").write_text(patch)
    with open(run_dir / "stream.jsonl", "rb") as src, gzip.open(run_dir / "stream.jsonl.gz", "wb") as dst:
        shutil.copyfileobj(src, dst)
    (run_dir / "stream.jsonl").unlink()
    record = {"instance_id": inst["instance_id"], "repo": inst["repo"], "arm": arm, "seed": seed,
              "model": MODEL, "exit_code": exit_code, "timed_out": timed_out,
              "sandbox": str(sandbox), "wall_s": round(wall_s, 1), "graphd_stopped": graphd_stopped,
              "patch_bytes": len(patch.encode()), **summarize(events)}
    (run_dir / "result.json").write_text(json.dumps(record, indent=2))
    shutil.rmtree(repo_dir)
    return record


def load_tasks(path):
    ids = json.load(open(path))["instance_ids"]
    rows = load_dataset("SWE-bench/SWE-bench_Verified", split="test")
    by_id = {r["instance_id"]: r for r in rows}
    return [by_id[i] for i in ids]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["templates", "run"])
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--tasks", type=Path)
    ap.add_argument("--only", nargs="*", help="restrict to these instance ids")
    ap.add_argument("--seeds", nargs="+", type=int, default=[1, 2])
    ap.add_argument("--arms", nargs="+", choices=list(TEMPLATE), default=list(ARMS))
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()
    if args.command == "templates":
        make_templates(args.work)
        return
    tasks = load_tasks(args.tasks)
    if args.only:
        tasks = [t for t in tasks if t["instance_id"] in args.only]
    # Arms alternate inside each task/seed so both see the same load and rate limits.
    jobs = [(t, arm, seed) for seed in args.seeds for t in tasks for arm in args.arms]
    spent = 0.0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(run_one, args.work, *job): job for job in jobs}
        for done, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            t, arm, seed = futures[fut]
            rec = fut.result()
            spent += rec["total_cost_usd"] or 0.0
            print(f"[{done}/{len(jobs)}] {t['instance_id']} {arm} s{seed}: "
                  f"${rec['total_cost_usd']} turns={rec['num_turns']} wall={rec['wall_s']}s "
                  f"cgraph_calls={rec['cgraph_tool_calls']} patch={rec['patch_bytes']}B "
                  f"subtype={rec['result_subtype']} | spent so far ${spent:.2f}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
