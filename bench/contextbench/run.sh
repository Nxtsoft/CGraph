#!/usr/bin/env bash
# One-command ContextBench Lite run for CGraph (file-level precision and recall).
#
#   bench/contextbench/run.sh [--cgraph-bin-dir DIR] [--jobs N] [--work DIR] [--limit N]
#
# --limit N evaluates only the first N instances (a smoke test, not a score).
#
# Pins: ContextBench code at CB_COMMIT (its metric code scores everything), data at
# HF_REVISION. Steps:
#   1. clone ContextBench at the pinned commit
#   2. download the Lite parquet (`contextbench_verified`, 500 instances) and check its sha256
#   3. export repo, commit and issue text only (instances.py; the labels stay behind)
#   4. fetch every instance's repository at its base commit (bench/arb/snapshots.py)
#   5. run CGraph on every instance (cgraph_eval.py)
#   6. score with ContextBench's file-level metric (score.py)
#
# Needs: git, curl, python3 >= 3.10 with pyarrow (set PYTHON to pick the interpreter),
# and a cgraph release (cgraph, cgraph-client, graphd in one directory; default: where
# `cgraph` on PATH resolves).
set -euo pipefail

CB_REPO="https://github.com/EuniAI/ContextBench.git"
CB_COMMIT="1436c28a8eb95496da4ea69ad458b9f8a8eb7d61"
HF_DATASET="Contextbench/ContextBench"
HF_REVISION="c2855792b006af41c67202d33883fb9d46362853"
LITE_FILE="contextbench_verified.parquet"
LITE_SHA256="e9dcfd504cbfb849ac815a79040c793d0d92f94eecc9b5a4ee3e1445a2f8a791"

HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"
WORK="${CONTEXTBENCH_WORK:-$HOME/.cache/cgraph-contextbench}"
JOBS=4
BIN_DIR=""
LIMIT=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cgraph-bin-dir) BIN_DIR="$(cd "$2" && pwd)"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
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
"$PY" -c 'import pyarrow' || { echo "$PY lacks pyarrow; set PYTHON to an interpreter that has it" >&2; exit 1; }
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
cd "$WORK"

# 1. ContextBench code at the pinned commit.
if [[ ! -d contextbench/.git ]]; then
  git init --quiet contextbench
  git -C contextbench remote add origin "$CB_REPO"
fi
git -C contextbench fetch --quiet --depth 1 origin "$CB_COMMIT"
git -C contextbench -c advice.detachedHead=false checkout --quiet --force "$CB_COMMIT"

# 2. Lite data, checked against the pinned hash.
mkdir -p data
if [[ ! -f "data/$LITE_FILE" ]]; then
  curl -fsSL -o "data/$LITE_FILE.part" \
    "https://huggingface.co/datasets/$HF_DATASET/resolve/$HF_REVISION/data/$LITE_FILE"
  mv "data/$LITE_FILE.part" "data/$LITE_FILE"
fi
echo "$LITE_SHA256  data/$LITE_FILE" | sha256sum -c --quiet -

# 3. Task inputs without labels.
"$PY" "$HERE/instances.py" "data/$LITE_FILE" instances.jsonl

# 4. Repository snapshots.
"$PY" "$HERE/../arb/snapshots.py" --cache repos --jobs "$JOBS" instances.jsonl

# 5. CGraph. Output is keyed by engine revision so a dev build never mixes with a release run.
REV="$("$BIN_DIR/cgraph" --version | awk '{gsub(/[()]/, "", $3); print $3}')"
RUN="runs/cgraph-$REV"
if [[ "$LIMIT" != 0 ]]; then RUN="$RUN-limit$LIMIT"; fi
# A failed instance does not stop scoring: score.py counts it as an error. The script still exits non-zero.
STATUS=0
"$PY" "$HERE/cgraph_eval.py" --instances instances.jsonl --cache repos --work "$WORK/snapshots" \
  --out "$RUN" --cgraph-bin-dir "$BIN_DIR" --jobs "$JOBS" --limit "$LIMIT" || STATUS=$?

# 6. Score.
"$PY" "$HERE/score.py" --contextbench-src contextbench --gold "data/$LITE_FILE" \
  --instances instances.jsonl --limit "$LIMIT" --details "$RUN/details.jsonl" --out "$RUN/results.json" --markdown "$RUN/results.md"
echo "results: $WORK/$RUN/results.md"
exit "$STATUS"
