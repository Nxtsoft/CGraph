## 1. Canonical engine
- [x] 1.1 Reuse canonical multi-source impact traversal and context packing.
- [x] 1.2 Validate source-root diff replay, paths, ranges and pins.
- [x] 1.3 Preserve old/new evidence, partial scope and qualified symbol ambiguity.
- [x] 1.4 Enforce complete serialized response budget and final source verification.

## 2. Product surface
- [x] 2.1 Expose CLI flags and the required MCP schema without daemon mutation.
- [x] 2.2 Verify installed CLI and MCP flows and retain machine-readable evidence.

## 3. Validation
- [x] 3.1 Exercise deletion, transitive dependents, rename, interface and barrel fixtures.
- [x] 3.2 Exercise malformed diff, stale source/pins, ambiguous scopes, omitted paths and budgets.
- [x] 3.3 Run full release tests and independent review; record sanitizer limitations or results.

## 4. Comparative evaluation
- [x] 4.1 Compare the new operation against existing primitives and Graphify under the matched maintenance evaluation; link actual results without superiority claims.

Matched evaluation: [CGR-2 PR 73](https://github.com/Nxtsoft/CGraph/pull/73), [scored summary](https://github.com/Nxtsoft/CGraph/blob/51bc83b574ca33201736bd9d21cd2c494b60326e/research/maintenance_eval/evidence/summary.json) and [independent source review](https://github.com/Nxtsoft/CGraph/blob/51bc83b574ca33201736bd9d21cd2c494b60326e/research/maintenance_eval/evidence/independent-source-review.json). All 31 scored cases completed their behavior and reported-edge checks: search, existing cgraph and Graphify each completed 9/9; change-context completed the same four-task interface/import/deletion/re-export subset at 4/4. Each fourth-arm case made a successful, nonempty real `graph_change_context` call. Independent source review approved all 22 edited outputs.

This small-task pilot reached the completion ceiling in every arm. It establishes that the new operation was exercised successfully, not completion superiority, lower cost, or a general competitive advantage.
