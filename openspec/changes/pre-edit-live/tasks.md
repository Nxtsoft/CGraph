# Tasks

- [x] 1.1 `ensure_output_dir_ignored` + test (writes `*`, keeps an edited `.gitignore`).
- [x] 1.2 Daemon calls it at startup.
- [x] 1.3 `should_prewarm` + test (git dir, git worktree file, `cgraph-out/`, plain dir).
- [x] 2.1 Multi-file A/B with the hook live (PR #198): daemon in 213/213 plugin runs, but the
      gold-miss share is unchanged (72.9% both arms); the hook triggers on Edit|Write|MultiEdit
      and only 20 of 213 runs used those tools.
