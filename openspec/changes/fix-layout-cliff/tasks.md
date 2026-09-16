# Tasks

- [x] 1.1 `analysis_test.cpp`: a 2,500-node graph (past the old threshold) lays out with finite
      canvas-scale coordinates spanning the sqrt(n) side; the test runs in well under a second.
- [x] 1.2 `write_layout`: Fruchterman-Reingold + AUTOGRID at every size; DrL branch and
      `kDrlThreshold` removed; `kLayoutIterations` in `analysis.hpp`.
- [x] 1.3 Spec delta: one layout algorithm regardless of node count.
- [x] 2.1 `ctest --preset default`: 78/78 pass (this Mac, Debug preset).
- [x] 2.2 Before/after from `stats.json`: CGraph's own tree (2,022 nodes) layout 13.4-16.9 s -> 0.3 s,
      build 13.4-16.9 s -> 1.0 s; `frontend` (17,439 nodes) layout 226.9 s -> 2.9 s, build 253.1 s ->
      26.8 s. Forced DrL under the old threshold: 11.0 s at 1,556 nodes, 14.9 s at 1,948.
- [ ] 2.3 Obtain non-author review and merge.
