"""Measure what an arm adds to every request before any task work.

Runs one trivial headless prompt per arm template in an empty one-commit repo, with
the same isolation, binary and model as run_ab.py, and reports the first request's
prompt prefix (cache write + cache read tokens, as analyze.py counts it), plus the
tools and skills the arm loads. The difference between two arms is the always-loaded
footprint that every turn of every run pays for.

CLAUDE_CODE_OAUTH_TOKEN must be set, and the templates must exist (run_ab.py templates):
  python prefix_probe.py --work /var/tmp/cgr29 --arms baseline cgraph --repeats 2
"""

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from run_ab import MODEL, clean_env, copy_config

PROMPT = "Reply with exactly: OK"


def probe(claude, work, arm):
    sandbox = Path(tempfile.mkdtemp(dir=work / "w", prefix="probe-"))
    try:
        copy_config(work, arm, sandbox)
        repo = sandbox / "repo"
        repo.mkdir()
        (repo / "README").write_text("probe\n")
        ident = ["-c", "user.name=bench", "-c", "user.email=bench@local"]
        for cmd in (["init", "-q"], ["add", "-A"], [*ident, "commit", "-q", "-m", "base"]):
            subprocess.run(["git", "-C", str(repo), *cmd], check=True)
        env = clean_env(sandbox / "home", sandbox / "cfg")
        out = subprocess.run([claude, "-p", PROMPT, "--model", MODEL, "--output-format", "stream-json",
                              "--verbose", "--dangerously-skip-permissions"],
                             cwd=repo, env=env, capture_output=True, text=True, timeout=600,
                             stdin=subprocess.DEVNULL)
        if out.returncode != 0:
            raise SystemExit(f"{arm}: claude exited {out.returncode}: {out.stderr.strip() or out.stdout[-2000:]}")
        events = [json.loads(line) for line in out.stdout.splitlines() if line.strip()]
        init = next(e for e in events if e.get("subtype") == "init")
        first = next(e for e in events if e.get("type") == "assistant")["message"]["usage"]
        return {
            "arm": arm,
            "prefix_tokens": first["cache_creation_input_tokens"] + first["cache_read_input_tokens"],
            "input_tokens": first["input_tokens"],
            "tools": sorted(t for t in init["tools"] if t.startswith("mcp__")),
            "skills": init.get("skills"),
            "mcp_servers": init.get("mcp_servers"),
        }
    finally:
        subprocess.run(["pkill", "-f", f"graphd --root {sandbox}"], check=False)
        shutil.rmtree(sandbox, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--claude", default="claude", help="Claude Code executable")
    ap.add_argument("--arms", nargs="+", required=True, help="template names under <work>/templates")
    ap.add_argument("--repeats", type=int, default=2)
    args = ap.parse_args()
    (args.work / "w").mkdir(exist_ok=True)
    results = [probe(args.claude, args.work, arm) for _ in range(args.repeats) for arm in args.arms]
    for r in results:
        print(json.dumps(r))
    base = min(r["prefix_tokens"] for r in results if r["arm"] == args.arms[0])
    for arm in args.arms[1:]:
        delta = min(r["prefix_tokens"] for r in results if r["arm"] == arm) - base
        print(f"{arm}: +{delta} prefix tokens over {args.arms[0]}")


if __name__ == "__main__":
    sys.exit(main())
