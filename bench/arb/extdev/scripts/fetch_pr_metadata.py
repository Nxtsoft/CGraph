"""Write ARB raw pull_requests.jsonl using ARB's own GraphQL crawl query, without per-PR REST calls.

ARB's `crawl` (crawler.crawl_repo) fetches merged PRs with crawler._fetch_merged_prs_graphql and then
spends 3+N REST calls per PR (files, review comments, commits, per-commit details). This driver performs
only the GraphQL step and writes the identical pull_request record crawl_repo writes; file, commit and
commit-detail records are then filled from git by `arb backfill-git-raw` (no REST).

Usage: PYTHONPATH=<arb>/src python3 fetch_pr_metadata.py --raw DIR --repo OWNER/NAME --limit-prs N
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from agent_retrieval_bench.crawler import _fetch_merged_prs_graphql, _pull_request_key, _upsert_jsonl
from agent_retrieval_bench.github_api import GitHubAPI
from agent_retrieval_bench.io import repo_slug, utc_now, write_json


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--raw", type=Path, required=True)
    p.add_argument("--repo", required=True)
    p.add_argument("--limit-prs", type=int, required=True)
    p.add_argument("--page-size", type=int, default=50)
    p.add_argument("--max-changed-files", type=int, default=20)  # crawl's default (cli.py crawl --max-changed-files)
    a = p.parse_args()
    api = GitHubAPI()
    assert api.authenticated, "set GITHUB_TOKEN"
    owner, name = a.repo.split("/", 1)
    repo_dir = a.raw / repo_slug(a.repo)
    fetched_at = utc_now()
    fetched = _fetch_merged_prs_graphql(api, owner, name, None, a.limit_prs, a.page_size, a.max_changed_files)
    records = [{"type": "pull_request", "repo": a.repo, "fetched_at": fetched_at, "data": pr} for pr in fetched["accepted"]]
    _upsert_jsonl(repo_dir / "pull_requests.jsonl", records, _pull_request_key)
    summary = {k: fetched[k] for k in ("source", "seen", "skipped", "existing_skipped", "cursor", "rate_limit")}
    summary.update({"repo": a.repo, "accepted": len(records), "limit_prs": a.limit_prs, "max_changed_files": a.max_changed_files, "fetched_at": fetched_at})
    write_json(repo_dir / "metadata_fetch_summary.json", summary)
    print(json.dumps(summary), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
