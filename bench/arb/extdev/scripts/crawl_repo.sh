#!/usr/bin/env bash
# Per-repo raw crawl: full clone (so backfill diffs are local), ARB GraphQL PR metadata, ARB backfill-git-raw.
# Usage: crawl_repo.sh OWNER/NAME LIMIT. Needs GH_TOKEN or GITHUB_TOKEN. Work dir: $ARB_WORK/extdev/work
# (ARB_WORK is run.sh's work dir, default ~/.cache/cgraph-arb); ARB is expected at $ARB_WORK/arb.
set -uo pipefail
repo="$1"; limit="$2"
ARB_WORK="${ARB_WORK:-$HOME/.cache/cgraph-arb}"
HERE="$(cd "$(dirname "$0")" && pwd)"
W="$ARB_WORK/extdev/work"; slug="${repo/\//__}"
export PYTHONPATH="$ARB_WORK/arb/src" PYTHONHASHSEED=0
retry() { "$@" || { echo "retry after exit $?: $*" >&2; "$@"; }; }
mkdir -p "$W/logs"
cd "$W"
[[ -d "git_raw_repos/$slug/.git" ]] || retry git clone --quiet --no-checkout "https://github.com/$repo.git" "git_raw_repos/$slug"
retry python3 "$HERE/fetch_pr_metadata.py" --raw raw --repo "$repo" --limit-prs "$limit"
retry python3 -m agent_retrieval_bench.cli backfill-git-raw --raw raw --repo "$repo" --repos-dir git_raw_repos > "logs/backfill_$slug.json"
echo "DONE $repo"
