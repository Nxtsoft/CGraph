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
  `cgraph: files likely to change with <file>`, one `- <path>: <reason>` line per file. The
  cross-service section's lines are unchanged and come second. The whole message, both sections,
  stays within 800 tokens (`kHookTokenBudget`, the 4-characters-per-token estimate the report and
  context ops use); service rows outrank files, and service rows that do not fit are counted in one
  closing line (`pre_edit_context`). The files section is left out in four cases: the graph is
  still building, the daemon cannot answer, the file is not in the graph, or the edit's text is not
  in the file. Malformed input exits 0.
- **The hook never starts a daemon and never waits past one deadline.** It asks only daemons
  already running (`ClientRequest::spawn = false`; the MCP server or `cgraph-client` starts them),
  so an edit in a repository with no daemon builds nothing and writes no `cgraph-out/`. Every ask,
  the services lookup and `co-change` alike, shares one deadline (`ClientRequest::deadline`):
  `request_over_unix_socket` connects without blocking and waits in `poll()` for what is left of
  it, so a daemon that is busy (a rescan holds its serve loop) or stopped costs the edit the
  deadline and no more. `CGRAPH_HOOK_WAIT_MS` sets it for the whole hook (default 250 ms, was a
  3000 ms wait per cold daemon); past it the hook prints nothing. No build is waited for.
- **A request whose client has gone is not computed.** After reading a frame the daemon polls the
  connection (`POLLRDHUP`; a `MSG_PEEK` read of 0 bytes where that flag is absent); if the client
  has hung up (a hook that gave up while the daemon was busy), the request is skipped and counted
  in `status` `ops.abandoned` instead of being computed, so abandoned asks do not delay the ones
  behind them. A live client is answered as before.
- **A client that gives up no longer kills the daemon.** The daemon's write of an answer to a client
  that has hung up raised SIGPIPE and the daemon died (exit 141; found once clients started giving
  up at a deadline). Frames are now sent with `MSG_NOSIGNAL` (`SO_NOSIGPIPE` where that flag is
  absent), so the write fails and the daemon keeps serving.
- **`bench/arb/pre_edit_replay.py`** replays the real `cgraph-client pre-edit` on ARB edit2ripple and
  comment2context samples. The simulated edit comes only from the query (anchor diff hunks, the
  review comment's line). It records whether the listed files include a gold file, the injected
  tokens, and the warm latency.

## Measured

Offline replay, every ARB edit2ripple and comment2context sample that `cgraph_eval.py` scores
(138), with the hook as shipped (one 250 ms deadline, no spawn). A comment2context comment line is
on GitHub's head side; the replay moves it to the base snapshot by its diff hunk's offset, clamped to
the hunk's base range (38 samples by the offset alone, 37 clamped: comments on added lines land on
the base lines the hunk replaced). It skips a sample with no hunk header (2) or whose `given_file` is
not in the snapshot (3): 5 skipped, 133 scored.
Command: `bench/arb/pre_edit_replay.py --jobs 1 --latency-repeats 5 --keep-snapshots`. The
before row is the bin-v0.9.1 release on the earlier sample set (135 scored; it names no file on any).

| | before (bin-v0.9.1) | after |
|---|---:|---:|
| samples where the hook names >= 1 gold file, edit2ripple | 0 / 58 | 26 / 58 (44.8%) |
| same, comment2context | 0 / 77 | 29 / 75 (38.7%) |
| same, overall | 0 / 135 | 55 / 133 (41.4%) |
| samples shown no gold file before the edit (miss-all proxy) | 100% | 58.6% (down 41.4 points) |
| silent samples (of which: the deadline passed on the first call after the daemon reported ready) | 135 | 4 (1) |
| injected tokens per edit, p50 / p95 / max (4 chars per token) | 0 | 118 / 158 / 263 |
| max injected tokens, ARB `regex_code_tokenizer_v1` | 0 | 256 |
| warm hook latency (median of 5 reruns per sample), p50 / p95 / max | 3.8 / 6.7 / 7.7 ms | 19.1 / 86.5 / 93.1 ms |
| first call after the daemon reports ready, p95 / max | 6.7 / 7.7 ms | 136 / 255 ms |

The hook on a scratch copy of the huggingface/transformers snapshot (108,602 nodes), an Edit of
`modeling_utils.py` through the reviewer's `hook.sh`:

| scenario | result |
|---|---|
| no daemon running (9 inputs, malformed ones included) | all exit 0, silent, 5-17 ms; no daemon started, no `cgraph-out/` written |
| warm daemon, 20 edits | 19 answered in 86-139 ms (616-byte context), 1 silent at 258 ms |
| full rescan in progress, an edit every 200 ms for 20 s | 77 edits, all silent, p50 12 ms, max 260 ms |
| incremental update after touching a source file, 10 edits | all answered, 90-118 ms |
| daemon stopped (SIGSTOP), 3 edits | silent at 257-259 ms each; after SIGCONT the daemon skipped the 3 abandoned requests (`status` `ops.abandoned` 3), and 5 edits 2 s later were answered in 86-167 ms |

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

The live check ran before the hook got its deadline and lost its spawn (every run had a
pre-warmed daemon, so the spawn never happened; the deadline was then 3000 ms). The live
difference is one run out of 16, which is noise. In 3 of the 5 hook-on misses, the hook
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
- `client_runtime_test.cpp` uses real daemons (and a real `graphd` process) and checks:
  - with `graphd` available, an Edit in a repository with no daemon is silent in under 100 ms and
    starts nothing or writes nothing; a running daemon answers; a SIGSTOP'd daemon is given up on
    within 250-500 ms of a 300 ms budget, and once resumed it is still alive and answers;
  - 60 long service rows and two files make a message within 800 tokens with the left-out rows
    counted; a short one keeps everything;
  - an Edit with its text lists `src/page.ts: calls `loadStats`` before the services section;
  - an Edit whose text is not in the file leaves only the services section;
  - a MultiEdit in a repository that no workspace lists names `src/use.ts` and `src/helper.test.ts`,
    says nothing about services, and stays within the budget.
- `daemon_server_test.cpp`: while a stalled peer holds the serve loop, a client sends a whole
  `query` and hangs up; the daemon counts it as abandoned and does not compute it (the query count
  is unchanged), and a live client's query is then computed and answered.
- Each of these mutations makes a test fail: no innermost filter, the first relation names the
  reason, no test pairing, no root outside a workspace, Edits treated as whole-file writes, the
  hook allowed to spawn, no deadline on the socket (the test hangs), `write` instead of
  `send(MSG_NOSIGNAL)` (the daemon dies of signal 13), no check for a hung-up client (three
  `daemon_server_test` expectations fail).

## Non-goals

- Git co-change history (CGraph keeps none).
- Changing what the services section says or its 8-endpoint cap.
- Hosts other than Claude Code.

## Impact

- `src/engine/co_change.cpp`, `include/cgraph/co_change.hpp` (new). `daemon_ops.cpp`,
  `operation_stats.{hpp,cpp}`: the `co-change` op.
- `src/client/client_runtime.{cpp,hpp}`, `src/client/main.cpp`: the hook, `ClientRequest::spawn`
  and `deadline`, `pre_edit_context`.
- `src/engine/daemon_server.{cpp,hpp}`: `request_over_unix_socket` takes a deadline; frames are
  sent without SIGPIPE.
- `tests/smoke/co_change_test.cpp` (new), `client_runtime_test.cpp`.
- `bench/arb/pre_edit_replay.py` (new).
- README, the cgraph skill, `cgraph-pre-edit.sh` header, marketplace description, CLAUDE.md op list.
- No change to graph output, ids or the index (`kIndexVersionKey` unchanged).
