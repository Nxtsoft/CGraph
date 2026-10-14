# Tasks

## 1. Engine

- [x] 1.1 `co_change.cpp`: edited symbols from line ranges (innermost), one pass over edges for
      direct dependents, same-file intermediates and every cross-file edge, a second pass for the
      depth-2 dependents, one pass over nodes to resolve them (four node passes in all); name-based test/source pairing;
      interleave of callers, tests and linked, capped by `limit`.
- [x] 1.2 `co-change` daemon op (`DaemonOp::CoChange`, op name, root-pinnable, `id` required).
- [x] 1.3 The serve loop skips a request whose client hung up after sending it (`peer_hung_up`),
      counting it in `ops.abandoned`; `daemon_server_test` covers it.

## 2. Hook

- [x] 2.1 `pre_edit_hook_output`: line ranges from `old_string` (every occurrence for
      `replace_all`, each edit of a MultiEdit, none for Write), the repository root without a
      workspace; the files section first, then the services section, the whole message within
      `kHookTokenBudget` (800), service rows outranking files (`pre_edit_context`).
- [x] 2.2 The hook asks only daemons already running (`ClientRequest::spawn = false`), never
      waits for a build, and shares one deadline across every ask (`ClientRequest::deadline`,
      enforced with poll() on the socket by `request_over_unix_socket`); `CGRAPH_HOOK_WAIT_MS`
      (default 250) sets it, and past it the hook prints nothing. Malformed input exits 0.

## 3. Tests

- [x] 3.1 `co_change_test`: ranking and reasons on a real one-shot graph, innermost method,
      whole-file write with a limit, test-to-source, an unconnected file, a non-file id, the daemon
      op and its required `id`.
- [x] 3.2 `client_runtime_test`: an Edit with its text lists `src/page.ts: calls `loadStats``
      before the services section; an Edit whose text is not in the file leaves only the services;
      a repository in no workspace gets the files section within the budget; a repository with
      no daemon is silent in under 100 ms and nothing is started or written; a running daemon
      answers; a SIGSTOP'd daemon is given up on at the deadline; 60 long service rows stay
      within 800 tokens with the rest counted.

## 4. Measurement

- [x] 4.1 `bench/arb/pre_edit_replay.py`: the real hook replayed on ARB edit2ripple and
      comment2context samples; gold-file hit rate, tokens, warm latency.
- [x] 4.2 Live A/B with headless Claude Code and the plugin installed from the branch.
