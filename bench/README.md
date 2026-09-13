# Render benchmark for `graph.html`

Quantifies the client-side cost of the interactive viewer emitted by
`export_graph_html` (`src/engine/export_json.cpp`), so rendering optimizations can
be measured instead of asserted.

It measures, per graph size, without modifying the generated HTML:

- **payload** — `graph.html` size on disk (the embedded `graphData` dominates)
- **DOMContentLoaded** — parse/load time of the document
- **time-to-settle** — wall-clock until the force-directed layout stops animating
  (detected by wrapping `requestAnimationFrame`; settled = no frame scheduled for 400ms)
- **peak JS heap** — `performance.memory.usedJSHeapSize`

## Run

```sh
# build cgraph first (see repo README), then:
CGRAPH=/path/to/cgraph SIZES="1000 5000 10000" bash bench/run.sh
```

Writes `bench/baseline-report.md`. Requires Python 3, Node, and Playwright's
chromium (`bunx playwright install chromium`).

## Pieces

- `gen_tree.py` — deterministic synthetic Python project of ~N nodes, grouped into
  packages (communities) with cross-file calls/imports for realistic edge density.
- `bench.mjs` — Playwright driver; loads a `graph.html`, detects layout settle,
  reports one JSON row.
- `run.sh` — generates a tree per size, scans it with `cgraph`, runs the driver,
  collects a markdown table.

## Why it exists

The current viewer runs an O(N²) force simulation on the browser main thread on every
page open (no precomputed layout, no spatial index) and redraws every node/edge each
frame. This harness pins the baseline so the server-side-layout and render-culling
optimizations can show their gains.

## Historical change impact

`scripts/change_impact_benchmark.py` replays six pinned historical changes from
Click, Flask, and Requests. It starts from each parent commit and applies only
the initiating source files. Downstream consumer repairs from the historical
commit are not copied into the candidate. Inputs contain 129–238 tracked files
per case; these are full repository archives, not generated toy fixtures.

The [case corpus](change-impact-cases.json) was independently reviewed before
running the tools. Each positive and negative label has an exact source quote,
line, revision, and rationale. The runner verifies those quotes, parent commits,
seed identifiers, and archive bytes before retrieval. It rejects filtered
archives and submodules rather than silently omitting source. Source digests
must remain unchanged throughout retrieval. Candidate responses must match the
actual diff, roots, initiating paths, and old/new source hashes.

Three deterministic arms inspect the same base and candidate:

- `literal_search`: whole-word fixed-string ripgrep for the supplied identifiers.
- `cgraph_primitives`: exact symbol/path lookup and dependent impact traversal
  against cold exports of both snapshots, using the published `bin-v0.3.0` CLI.
- `change_context`: the installed source-pinned operation from
  [tracker #72](https://github.com/Nxtsoft/CGraph/pull/72), with a 24,000-token
  response budget and the same maximum traversal depth of six.

The baseline arms are uncapped; this is a retrieval-quality diagnostic with
different output formats, not an equal-token or warm-daemon latency contest.
All raw stdout, stderr, commands, timings, response bindings, and executable
hashes are retained. Three rotated repetitions measure timing variability;
they are not three independent quality samples.

### Reproduce

Requires Python 3.12+, Git, ripgrep, the installed release CLI, and a separately
installed candidate CLI with `change-context` support. The published
`bin-v0.3.0` binary does not include that draft operation.

```sh
python3 scripts/change_impact_benchmark.py \
  --cases bench/change-impact-cases.json \
  --cgraph /absolute/path/to/bin-v0.3.0/cgraph \
  --change-context /absolute/path/to/change-context/cgraph \
  --repos "$HOME/.agents/scratch/cgraph-impact-repos" \
  --out "$HOME/.agents/artifacts/2026-09-13/cgraph-impact-reproduction" \
  --repeats 3

python3 -m unittest discover -s scripts -p 'change_impact_benchmark_test.py' -v
```

The clone cache is populated from the pinned public repository URLs. Output must
be a new directory; `COMPLETE` is written only after all trials finish and names
the SHA-256 of `results.json`. Tool errors stay in the scored results and cause
a nonzero process exit. A missing dependency is a measured miss, not a tool error.

### Observed results — September 13, 2026

The [machine-generated results](change-impact-results.json) contain 54 trials
with no tool failures. The runner and corpus hashes match the committed files.
The baseline source is `4a31f3ed2f0d51315e63e74e0a5593ac00db1ddb`;
the candidate source is `60d148453e94a7247a6a7dec29e13ec11b04312d`.

| Arm | Mean required-file recall | Cases passing all explicit labels | Median unjudged files |
| --- | ---: | ---: | ---: |
| Literal search | 100% | 6/6 | 4 |
| Existing cgraph primitives | 83.3% | 1/6 | 21.5 |
| Change-context | 66.7% | 1/6 | 5 |

Passing requires every labeled required file and no explicitly labeled unrelated
file. Other returned files are **unjudged**, not assumed false positives.
The positive labels are not an exhaustive dependency inventory. These Python
cases do not establish results for other languages, agent task completion, or
Graphify. Explicit identifiers favor literal search, so this result is not a
general search superiority claim either. It does show that cgraph needs to
retain known consumers and control unrelated results before claiming an impact
retrieval advantage on these changes.

The Click callback rename makes one failure concrete: change-context missed
`examples/imagepipe/imagepipe.py` at the 24,000-token budget in all three trials,
while literal search and existing primitives found it. The response retained
29 impacts and reported 567 omitted impacts. A separate diagnostic at a
1,000,000-token response budget recovered the file at depth two through import
edges. This is a serialization budget, not model token consumption. The
diagnostic does not replace the scored result, and its import witness does not
prove precise callback resolution.

The next candidate engine experiment is to preserve one best impact witness per
file before filling the budget with additional symbols from represented files.
The intended C++ integration point is `src/engine/change_context.cpp` in tracker
#72, where depth-based impact shedding currently drops distant files. Keep
source identities and witnesses intact, then rerun the unchanged corpus; no
improvement has been measured yet.

Full raw runs: `mars:/home/taylor/.agents/artifacts/2026-09-13/cgraph-change-impact/run-1`.
Separate budget diagnostic:
`mars:/home/taylor/.agents/scratch/cgraph-change-context-diagnosis/click-result-1m.json`.
