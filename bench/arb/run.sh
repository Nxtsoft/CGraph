#!/usr/bin/env bash
# One-command Agent Retrieval Bench (ARB) run for CGraph.
#
#   bench/arb/run.sh [--cgraph-bin-dir DIR] [--jobs N] [--work DIR] [--skip-baselines] [--limit N]
#
# --limit N evaluates only the first N samples of each task (a smoke test, not a score).
#
# Pins: ARB code at ARB_COMMIT, ARB data at HF_REVISION. Steps:
#   1. clone ARB at the pinned commit (its metric code scores everything)
#   2. download the five v2 release bundles; ARB's own `download-benchmark
#      --skip-download` verifies the sha256 and extracts them
#   3. fetch every sample's repository at its base commit (bare shallow caches)
#   4. reproduce the lexical / BM25 / RepoMap leaderboard baselines with ARB's harness
#   5. run CGraph on all 427 samples (cgraph_eval.py)
#   6. score every method with ARB's sample_metrics + canonical BCY (score.py)
#
# Needs: git, curl, zstd, tar, python3 >= 3.10, and a cgraph release
# (cgraph, cgraph-client, graphd in one directory; default: where `cgraph` on PATH resolves).
set -euo pipefail

ARB_REPO="https://github.com/eyuansu62/agent-retrieval-bench.git"
ARB_COMMIT="07014c986f3deadb1548c62b32c0ffbe6a81465d"
HF_DATASET="eyuansu71/agent_retrieval_bench"
HF_REVISION="5901e1ee3aff048290db72edf9c63bc498b79ea3"
RELEASES=(v2_code2test v2_comment2context v2_trace2code v2_edit2ripple v2_abstention)
TASKS=(code2test comment2context trace2code edit2ripple)

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${ARB_WORK:-$HOME/.cache/cgraph-arb}"
JOBS=4
BIN_DIR=""
SKIP_BASELINES=0
LIMIT=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cgraph-bin-dir) BIN_DIR="$(cd "$2" && pwd)"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --skip-baselines) SKIP_BASELINES=1; shift ;;
    --limit) LIMIT="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$BIN_DIR" ]]; then
  command -v cgraph >/dev/null || { echo "no cgraph on PATH; pass --cgraph-bin-dir" >&2; exit 1; }
  BIN_DIR="$(dirname "$(readlink -f "$(command -v cgraph)")")"
fi
for exe in cgraph cgraph-client graphd; do
  [[ -x "$BIN_DIR/$exe" ]] || { echo "missing $BIN_DIR/$exe" >&2; exit 1; }
done
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
cd "$WORK"   # ARB corpus manifests record chunk paths relative to the dir holding data/

# 1. ARB code at the pinned commit.
if [[ ! -d arb/.git ]]; then
  git init --quiet arb
  git -C arb remote add origin "$ARB_REPO"
fi
git -C arb fetch --quiet --depth 1 origin "$ARB_COMMIT"
git -C arb -c advice.detachedHead=false checkout --quiet --force "$ARB_COMMIT"
export PYTHONPATH="$WORK/arb/src"
# ARB's RepoMap baseline iterates Python sets, so its ranking moves with the hash seed
# (edit2ripple R@20 0.5948 at seed 0 vs 0.5934 at seed 1). Pin it.
export PYTHONHASHSEED=0
arb() { python3 -m agent_retrieval_bench.cli "$@"; }

# 2. Data releases, verified and extracted by ARB.
for rel in "${RELEASES[@]}"; do
  if [[ -f "data/benchmark/$rel/samples.jsonl" ]]; then continue; fi
  mkdir -p "data/releases/$rel"
  for f in "agent_retrieval_bench_$rel.tar.zst" "agent_retrieval_bench_$rel.tar.zst.sha256"; do
    curl -fsSL -o "data/releases/$rel/$f" \
      "https://huggingface.co/datasets/$HF_DATASET/resolve/$HF_REVISION/releases/$rel/$f"
  done
  arb download-benchmark --version "$rel" --local-dir data --skip-download --force >/dev/null
  rm -f "data/releases/$rel/agent_retrieval_bench_$rel.tar.zst"
done

# 3. Repository snapshots.
python3 "$HERE/snapshots.py" --cache repos --jobs "$JOBS" data/benchmark/*/samples.jsonl

# 4. Leaderboard baselines through ARB's own harness.
if [[ "$SKIP_BASELINES" == 0 ]]; then
  mkdir -p baselines
  for t in "${TASKS[@]}"; do
    [[ -f "baselines/${t}_repomap_details.jsonl" ]] || arb eval-repomap \
      --derived "data/benchmark/v2_$t" --corpus "data/corpus/v2_$t" --candidate-filter all_files --no-keep-list \
      --out "baselines/${t}_repomap_summary.json" --details "baselines/${t}_repomap_details.jsonl" >/dev/null
    for r in lexical bm25; do
      [[ -f "baselines/${t}_${r}_details.jsonl" ]] || arb eval-baseline \
        --derived "data/benchmark/v2_$t" --corpus "data/corpus/v2_$t" --ranker "$r" --candidate-filter all_files \
        --no-keep-list --out "baselines/${t}_${r}_summary.json" --details "baselines/${t}_${r}_details.jsonl" >/dev/null
    done
  done
fi

# 5. CGraph. Output is keyed by engine revision so a dev build never mixes with a release run.
REV="$("$BIN_DIR/cgraph" --version | awk '{gsub(/[()]/, "", $3); print $3}')"
RUN="runs/cgraph-$REV"
if [[ "$LIMIT" != 0 ]]; then RUN="$RUN-limit$LIMIT"; SKIP_BASELINES=1; fi
# Snapshots are checked out at a fixed absolute path. Since CGR-33 `context` names files
# relative to the snapshot root, so its packing no longer depends on where the snapshot
# sits (engines before it charged the absolute path); the fixed path keeps runs of those
# older engines comparable too.
SNAPSHOTS="${ARB_SNAPSHOTS:-/tmp/cgraph-arb-snapshots}"
python3 "$HERE/cgraph_eval.py" --arb-src arb --data data --cache repos --work "$SNAPSHOTS" \
  --out "$RUN" --cgraph-bin-dir "$BIN_DIR" --jobs "$JOBS" --limit "$LIMIT"

# 6. Score.
METHODS=("CGraph=$RUN")
if [[ "$SKIP_BASELINES" == 0 ]]; then
  METHODS+=("RepoMap=baselines:repomap" "Lexical=baselines:lexical" "BM25=baselines:bm25")
fi
python3 "$HERE/score.py" --arb-src arb --data data --out "$RUN/results.json" --markdown "$RUN/results.md" "${METHODS[@]}"
echo "results: $WORK/$RUN/results.md"
