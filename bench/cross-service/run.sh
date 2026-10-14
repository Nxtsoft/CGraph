#!/usr/bin/env bash
# One-command cross-service benchmark: CGraph vs GitNexus vs codebase-memory-mcp.
#
#   bench/cross-service/run.sh [--cgraph-bin-dir DIR] [--work DIR] [--skip-baselines]
#
# Steps:
#   1. check out every system in systems.json at its pinned commit and lay each service out as
#      its own git repository (materialize.py)
#   2. CGraph: `cgraph --root <service>` per service, `cgraph seam discover` per system
#   3. GitNexus 1.6.12 from npm into an isolated prefix (its install scripts are not run and
#      neither is `gitnexus setup`), HOME and storage under the work dir: `analyze
#      --index-only` per service, then `group create` / `group add` / `group sync` per system
#   4. codebase-memory-mcp 0.11.0 release binary (sha256-checked), HOME under the work dir:
#      `index_repository` per service, then `--mode cross-repo-intelligence` per service with
#      the system's other services as targets
#   5. score every tool against ground_truth.jsonl (score.py), per kind, at contract and
#      service level; writes <work>/results.md and <work>/results.json
#
# Needs: git, python3 >= 3.10, node + npm (GitNexus), curl, sha256sum, and a cgraph build
# (default: the `cgraph` on PATH).
set -euo pipefail

GITNEXUS_VERSION="1.6.12"
CBM_VERSION="0.11.0"
CBM_TARBALL="codebase-memory-mcp-linux-amd64.tar.gz"
CBM_SHA256="032b33c1833919a2d1de67ff6367fa6ea46aee8689c86ef223c88fae3b6e4536"
CBM_URL="https://github.com/DeusData/codebase-memory-mcp/releases/download/v${CBM_VERSION}"

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${CROSS_SERVICE_WORK:-$HOME/.cache/cgraph-cross-service}"
BIN_DIR=""
SKIP_BASELINES=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cgraph-bin-dir) BIN_DIR="$(cd "$2" && pwd)"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --skip-baselines) SKIP_BASELINES=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$BIN_DIR" ]]; then
  command -v cgraph >/dev/null || { echo "no cgraph on PATH; pass --cgraph-bin-dir" >&2; exit 1; }
  BIN_DIR="$(dirname "$(readlink -f "$(command -v cgraph)")")"
fi
CGRAPH="$BIN_DIR/cgraph"
[[ -x "$CGRAPH" ]] || { echo "missing $CGRAPH" >&2; exit 1; }
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
PRED="$WORK/predictions"
rm -rf "$PRED"
mkdir -p "$PRED"
SYSTEMS=$(python3 -I -c 'import json,sys; print(" ".join(s["name"] for s in json.load(open(sys.argv[1]))["systems"]))' "$HERE/systems.json")
services() { ls "$WORK/repos/$1"; }

# 1. Service repositories at the pinned commits.
python3 -I "$HERE/materialize.py" "$WORK"

# 2. CGraph.
echo "cgraph: $("$CGRAPH" --version)" >&2
: > "$PRED/cgraph.jsonl"
for sys in $SYSTEMS; do
  out="$WORK/cgraph/$sys"
  rm -rf "$out"
  mkdir -p "$out"
  args=()
  for svc in $(services "$sys"); do
    "$CGRAPH" --root "$WORK/repos/$sys/$svc" --out "$out/$svc" > "$out/$svc.log" 2>&1
    args+=(--graph "$svc=$out/$svc/graph.json")
  done
  "$CGRAPH" seam discover "${args[@]}" --out "$out/seam" > "$out/seam.log" 2>&1
  python3 -I "$HERE/predict_cgraph.py" "$sys" "$out/seam" >> "$PRED/cgraph.jsonl"
done
TOOLS=("cgraph=$PRED/cgraph.jsonl")

if [[ "$SKIP_BASELINES" == 0 ]]; then
  # 3. GitNexus, isolated.
  GN_PREFIX="$WORK/tools/gitnexus-$GITNEXUS_VERSION"
  if [[ ! -x "$GN_PREFIX/node_modules/.bin/gitnexus" ]]; then
    npm install --prefix "$GN_PREFIX" --no-audit --no-fund --ignore-scripts "gitnexus@$GITNEXUS_VERSION" >&2
  fi
  GN="$GN_PREFIX/node_modules/.bin/gitnexus"
  GN_HOME="$WORK/gitnexus/home"
  rm -rf "$WORK/gitnexus"
  mkdir -p "$GN_HOME" "$WORK/gitnexus/store"
  gn() { env HOME="$GN_HOME" GITNEXUS_STORAGE_ROOT="$WORK/gitnexus/store" DO_NOT_TRACK=1 SCARF_ANALYTICS=false "$GN" "$@"; }
  echo "gitnexus: $(gn --version)" >&2
  : > "$PRED/gitnexus.jsonl"
  for sys in $SYSTEMS; do
    for svc in $(services "$sys"); do
      gn analyze --index-only --force --name "$sys-$svc" "$WORK/repos/$sys/$svc" > "$WORK/gitnexus/$sys-$svc.log" 2>&1
    done
    gn group create "$sys" > /dev/null
    for svc in $(services "$sys"); do
      gn group add "$sys" "$sys/$svc" "$sys-$svc" > /dev/null
    done
    gn group sync "$sys" > "$WORK/gitnexus/$sys-sync.log" 2>&1
    python3 -I "$HERE/predict_gitnexus.py" "$sys" "$GN_HOME/.gitnexus/groups/$sys/contracts.json" >> "$PRED/gitnexus.jsonl"
  done
  TOOLS+=("gitnexus=$PRED/gitnexus.jsonl")

  # 4. codebase-memory-mcp, isolated.
  CBM_DIR="$WORK/tools/codebase-memory-mcp-$CBM_VERSION"
  CBM="$CBM_DIR/codebase-memory-mcp"
  if [[ ! -x "$CBM" ]]; then
    mkdir -p "$CBM_DIR"
    curl -fsSL -o "$CBM_DIR/$CBM_TARBALL" "$CBM_URL/$CBM_TARBALL"
    echo "$CBM_SHA256  $CBM_DIR/$CBM_TARBALL" | sha256sum -c - >&2
    tar -xzf "$CBM_DIR/$CBM_TARBALL" -C "$CBM_DIR"
  fi
  CBM_HOME="$WORK/cbm/home"
  rm -rf "$WORK/cbm"
  mkdir -p "$CBM_HOME"
  cbm() { env HOME="$CBM_HOME" "$CBM" cli --quiet "$@"; }
  echo "codebase-memory-mcp: $(env HOME="$CBM_HOME" "$CBM" --version)" >&2
  : > "$PRED/cbm.jsonl"
  for sys in $SYSTEMS; do
    targets=$(services "$sys" | sed "s/^/\"$sys-/;s/\$/\"/" | paste -sd, -)
    for svc in $(services "$sys"); do
      cbm index_repository "{\"repo_path\":\"$WORK/repos/$sys/$svc\",\"name\":\"$sys-$svc\"}" > "$WORK/cbm/$sys-$svc.log" 2>&1
    done
    for svc in $(services "$sys"); do
      printf '{"repo_path":"%s","name":"%s-%s","mode":"cross-repo-intelligence","target_projects":[%s]}' \
        "$WORK/repos/$sys/$svc" "$sys" "$svc" "$targets" > "$WORK/cbm/$sys-$svc.cross.json"
      cbm index_repository --args-file "$WORK/cbm/$sys-$svc.cross.json" > "$WORK/cbm/$sys-$svc.cross.log" 2>&1
    done
    env HOME="$CBM_HOME" python3 -I "$HERE/predict_cbm.py" "$sys" "$CBM" $(services "$sys") >> "$PRED/cbm.jsonl"
  done
  TOOLS+=("codebase-memory-mcp=$PRED/cbm.jsonl")
fi

# 5. Score.
python3 -I "$HERE/score.py" "$HERE/ground_truth.jsonl" "${TOOLS[@]}" --json="$WORK/results.json" | tee "$WORK/results.md"
