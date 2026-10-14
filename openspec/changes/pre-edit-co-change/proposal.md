# Name the files likely to change before an edit (CGR-30)

## Why

Agents miss every gold file on 27-35% of Agent Retrieval Bench (ARB) samples, and they often never
call graph tools. ARB's own GPT-5.4-mini trajectory analysis (`docs/v1_4_trajectory_error_analysis.md`
at `07014c9`, line 17) counts 47 exact misses out of 80 comment2context samples. The pre-edit hook
already runs before every Edit, Write and MultiEdit, but it only spoke inside a
`cgraph.workspace.json`, and only about HTTP endpoints. An agent editing a function in an ordinary
repository was shown nothing.

## What Changes

- **New daemon op `co-change`** (`src/engine/co_change.cpp`). It takes a file node id and the line
  ranges an edit replaces. It finds the innermost symbols of the file that overlap those lines and
  ranks three lists:
  - **callers**: files that reach an edited symbol, directly or through one more symbol;
  - **tests**: the file's test or source counterpart by name;
  - **linked**: other files joined to the file by any edge.

  It interleaves the three lists and returns up to five files, each with a one-line reason. It reads
  the snapshot in at most two passes over the edges and four over the nodes. It builds no adjacency and
  runs no BFS, so on huggingface/transformers (108,602 nodes, 326,426 edges) the op answers in about
  43 ms, including client process start.
- **The hook asks it.** `pre_edit_hook_output` works out the edited lines from the hook input:
  - `old_string`, with every occurrence when `replace_all` is set;
  - each edit of a MultiEdit;
  - the whole file for a Write.

  It takes the nearest `.git` as the root even outside a workspace. It prints
  `cgraph: files likely to change with <file>`, one `- <path>: <reason>` line per file, inside an
  800-token budget (`kHookTokenBudget`, using the 4-characters-per-token estimate the report and
  context ops use). The cross-service section is unchanged and comes second. The files section is
  left out in four cases: the graph is still building, the daemon cannot answer, the file is not in
  the graph, or the edit's text is not in the file.
- **`bench/arb/pre_edit_replay.py`** replays the real `cgraph-client pre-edit` on ARB edit2ripple and
  comment2context samples. The simulated edit comes only from the query (anchor diff hunks, the
  review comment's line). It records whether the listed files include a gold file, the injected
  tokens, and the warm latency.

## Measured

Offline replay, every ARB edit2ripple and comment2context sample that `cgraph_eval.py` scores
(138; 3 comment2context samples skipped because their `given_file` is not in the base snapshot).
Command: `bench/arb/pre_edit_replay.py --jobs 1 --latency-repeats 5 --keep-snapshots`. The same
harness ran with the bin-v0.9.1 release as the before row.

| | before (bin-v0.9.1) | after |
|---|---:|---:|
| samples where the hook names >= 1 gold file, edit2ripple | 0 / 58 | 26 / 58 (44.8%) |
| same, comment2context | 0 / 77 | 29 / 77 (37.7%) |
| same, overall | 0 / 135 | 55 / 135 (40.7%) |
| samples shown no gold file before the edit (miss-all proxy) | 100% | 59.3% (down 40.7 points) |
| injected tokens per edit, p50 / p95 / max (4 chars per token) | 0 | 118 / 158 / 263 |
| max injected tokens, ARB `regex_code_tokenizer_v1` | 0 | 256 |
| warm hook latency (median of 5 reruns per sample), p50 / p95 / max | 3.8 / 6.7 / 7.7 ms | 19.4 / 86.7 / 94.3 ms |
| first call after the daemon reports ready, p95 / max | 6.7 / 7.7 ms | 199 / 260 ms |

Live check: headless Claude Code 2.1.296 (`--model sonnet`, clean `HOME` and `CLAUDE_CONFIG_DIR`),
with the plugin installed from this branch through `claude plugin marketplace add
https://github.com/Nxtsoft/CGraph.git#cgr-30-pre-edit-impact`. The launcher cache held this
branch's build, and the no-hook arm set `disableAllHooks`. The tasks were the first 8 edit2ripple
samples by id whose snapshot has at most 1,500 files. The agent got the intent and the anchor
diff, and was told to make every other change that was needed. Each arm ran twice per task (16 runs
per arm). In both repetitions, one task's no-hook run returned no output at all; each of those runs
was rerun once.

| | hook off | hook on |
|---|---:|---:|
| runs that opened (Read/Edit/Write) >= 1 gold file | 10 / 16 | 11 / 16 |
| runs that opened no gold file | 6 / 16 (37.5%) | 5 / 16 (31.3%) |

The live difference is one run out of 16, which is noise. In 3 of the 5 hook-on misses, the hook
had named the gold file (`binding/form_mapping_test.go` twice, `context_test.go` once) and the
agent did not open it. The missing step is the agent acting on the hint, not finding the file.

## Contract that tests verify

- `co_change_test.cpp` builds a real one-shot graph of a small Python package and checks:
  - an edit of `record()` lists `routes.py` (`calls `record``, chosen over the import), then
    `test_metrics.py`, `unrelated.py` and `service.py` (the second-hop reason);
  - an edit inside a method names the method, not its class;
  - an edit of `other()` lists its caller first;
  - a whole-file write with `limit 2` returns two files;
  - a test file names the code it covers;
  - an unconnected file lists nothing;
  - a symbol id is not found;
  - the daemon op answers and requires `id`.
- `client_runtime_test.cpp` uses real daemons and checks:
  - an Edit with its text lists `src/page.ts: calls `loadStats`` before the services section;
  - an Edit whose text is not in the file leaves only the services section;
  - a MultiEdit in a repository that no workspace lists names `src/use.ts` and `src/helper.test.ts`,
    says nothing about services, and stays within the budget.
- Each of these mutations makes a test fail: no innermost filter, the first relation names the
  reason, no test pairing, no root outside a workspace, Edits treated as whole-file writes.

## Non-goals

- Git co-change history (CGraph keeps none).
- Changing what the services section says or its 8-endpoint cap.
- Hosts other than Claude Code.

## Impact

- `src/engine/co_change.cpp`, `include/cgraph/co_change.hpp` (new). `daemon_ops.cpp`,
  `operation_stats.{hpp,cpp}`: the `co-change` op.
- `src/client/client_runtime.{cpp,hpp}`: the hook.
- `tests/smoke/co_change_test.cpp` (new), `client_runtime_test.cpp`.
- `bench/arb/pre_edit_replay.py` (new).
- README, the cgraph skill, `cgraph-pre-edit.sh` header, marketplace description, CLAUDE.md op list.
- No change to graph output, ids or the index (`kIndexVersionKey` unchanged).
