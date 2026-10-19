## Why

CGraph's Agent Retrieval Bench (ARB) result was tuned on 40 ARB samples (CGR-25). On the 234
samples from the other 22 repositories it trailed RepoMap on Recall@20 and BCY@8k (CGR-38). The
losses were in code2test (find the tests for a change) and edit2ripple (find what an edit
ripples into), and three engine gaps explained them:

- A test file reached its subject only when a call from the test resolved unambiguously, so most
  test files were islands: neither `context` nor `impact` from a source file reached its tests.
- A file node's span is its first line, so text no symbol encloses (top-level statements, a test
  file's anonymous `test("...", () => ...)` blocks, data modules) was in no BM25 document, and
  neither was the file's path.
- Once linked, a test file still ranked under every file its subject imports: `context` orders by
  depth then centrality, the `tests` edge hangs off the file node a hop past a symbol seed, and
  test files carry little centrality.

## What Changes

- Build pass `resolve_test_subjects` (one-shot and incremental): an inferred `tests` edge from
  each test file to the source file its name pairs with (`path_test.go` -> `path.go`,
  `test_core.py` -> `core.py`, `button.spec.ts` -> `button.ts`, `tests/help.rs` -> `src/help.rs`).
  A test outside any test-root directory pairs only within its own directory. One under a test
  root may also pair with the unique match inside its package (what precedes the test root) or the
  one file in the directory its test-root-free path mirrors. Generic names (`main`, `mod`, `lib`,
  `index`, `__init__`, `utils`...) pair only in-directory; example, doc and bench files are never
  subjects. `stem_of`/`test_core` move next to `is_test_source_path` and are shared with co-change.
- Body index: a file node's document is the lines no other node's span covers plus its path terms.
  Symbol documents are unchanged and nothing is counted twice.
- `context`: a seed file's `tests` counterparts enter at depth 1 (`via = "tests"`), unless
  `max_depth` is 0.
- Index version key `logic-26`.
- ARB harness: a declared dev/held-out split (`score.py --split`, `cgraph_eval.py --split`), and
  the per-file anchor op lists are fused among themselves before they vote (`cgraph_eval.fuse`).

## Impact

`impact` dependents of a source file now include its tests at depth 1, and `path` may route
through a test file. Reports (modules, clones, types) and the pre-edit hook's cross-service query
use relation allowlists and are unaffected.
