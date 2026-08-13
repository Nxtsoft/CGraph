#!/usr/bin/env bash
# Reproducible render benchmark for the graph.html viewer.
# For each target node count: generate a synthetic tree, scan it with cgraph,
# then measure the viewer's client cost (payload / DOMContentLoaded / settle /
# heap) with a headless-chromium driver. Writes a markdown report.
#
# Env:
#   CGRAPH   path to the cgraph binary (default: cgraph on PATH)
#   SIZES    space-separated node targets (default: "1000 5000 10000")
set -euo pipefail

CGRAPH="${CGRAPH:-cgraph}"
SIZES="${SIZES:-1000 5000 10000}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
REPORT="${REPORT:-$HERE/baseline-report.md}"

command -v "$CGRAPH" >/dev/null || { echo "cgraph not found: $CGRAPH" >&2; exit 1; }

echo "# graph.html render benchmark" > "$REPORT"
echo "" >> "$REPORT"
echo "| target nodes | actual nodes | edges | payload (MB) | DOMContentLoaded (ms) | time-to-settle (ms) | peak heap (MB) |" >> "$REPORT"
echo "|---|---|---|---|---|---|---|" >> "$REPORT"

for n in $SIZES; do
  tree="$WORK/tree-$n"
  out="$WORK/out-$n"
  python3 "$HERE/gen_tree.py" --nodes "$n" --out "$tree" >/dev/null
  "$CGRAPH" --root "$tree" --out "$out" >/dev/null 2>&1
  nodes=$(python3 -c "import json;d=json.load(open('$out/stats.json'));print(d['node_count'])")
  edges=$(python3 -c "import json;d=json.load(open('$out/stats.json'));print(d['edge_count'])")
  metrics=$(bunx --bun node "$HERE/bench.mjs" "$out/graph.html" "$n" 2>/dev/null || node "$HERE/bench.mjs" "$out/graph.html" "$n")
  read -r payload dom settle heap < <(python3 -c "
import json,sys
m=json.loads('''$metrics''')
print(m['payload_mb'], m['dom_ms'], m['settle_ms'], m['peak_heap_mb'])
")
  echo "| $n | $nodes | $edges | $payload | $dom | $settle | $heap |" >> "$REPORT"
  echo "n=$n -> nodes=$nodes edges=$edges payload=${payload}MB dom=${dom}ms settle=${settle}ms heap=${heap}MB"
done

rm -rf "$WORK"
echo ""
echo "report written to $REPORT"
cat "$REPORT"
