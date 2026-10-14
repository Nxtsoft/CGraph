# Tasks

## 1. Engine

- [x] 1.1 `pack_context` takes the path each entry names its file by (`ContextEntryPath`) and
      writes it into the focus and every candidate brief before any cost is projected or measured.
- [x] 1.2 The daemon's `context` op passes the path relative to `DaemonState::project_root`
      (`relative_file`, moved from report.cpp's anonymous namespace to `report.hpp`) and adds a
      top-level `root`.
- [x] 1.3 `change_context` passes each node's path relative to its own snapshot root to
      `pack_seed_context`.

## 2. Tests

- [x] 2.1 `daemon_ops_test`: one Python tree built with `run_one_shot` at a short root and at a
      root 142 characters longer; under greedy and knapsack packing at a shedding budget, both
      responses carry identical `focus`/`included` (order included), `omitted` and `tokens_used`;
      entries are relative and `root` is the canonical root. With the absolute path restored the
      same test packs 9 entries at the short root and 6 at the long one.
- [x] 2.2 `retrieval_quality_test` serves with the project root and is re-pinned.

## 3. Consumers

- [x] 3.1 `bench/arb/cgraph_eval.py` reads context paths as repo-relative; `run.sh` comment updated.
- [x] 3.2 `integrations/skills/cgraph/SKILL.md` notes the relative paths and `root`.

## 4. Verification

- [x] 4.1 ARB 30-sample run (`--limit 6`, all five tasks) from two snapshot roots 124 characters
      apart: identical top files and per-op lists on 30/30 samples (9/30 before).
- [x] 4.2 Full ctest.
