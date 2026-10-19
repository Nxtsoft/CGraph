# Tasks

## 1. Graph

- [x] 1.1 `resolve_test_subjects` in one-shot and incremental builds; shared `stem_of`/`test_core`;
      `common_source_root` replaces the inline root derivation in contracts.
- [x] 1.2 Pairing scope: in-directory always; package-unique or mirrored only under a test root;
      generic stems in-directory only; examples/docs/benches never subjects.
- [x] 1.3 Index version key `logic-26`.

## 2. Search and context

- [x] 2.1 File node body document: uncovered lines + path terms.
- [x] 2.2 `context` admits `tests` counterparts of seed files at depth 1; none at `max_depth` 0.

## 3. Tests

- [x] 3.1 graph_builder_test: pairing rules, idempotence, root under a `tests` directory.
- [x] 3.2 body_index_test: file documents, no double counting, path terms.
- [x] 3.3 daemon_ops_test: counterpart depth and order, `max_depth` 0 (fails without the change).
- [x] 3.4 incremental_update_test: incremental and one-shot agree on `tests` edges.

## 4. Benchmark (CGR-38)

- [x] 4.1 Declared split (`score.py`/`cgraph_eval.py --split`) committed before tuning.
- [x] 4.2 External dev set from 10 repositories outside ARB, built with ARB's own miners.
- [x] 4.3 Anchor op lists vote once (`cgraph_eval.fuse`).
- [ ] 4.4 Held-out score on a released binary; bench/arb/RESULTS.md section.
