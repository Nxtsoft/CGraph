# Tasks
- [x] 1.1 Add `is_enrichment_node_id` (doc:/concept:/media:/topic:) to types.hpp.
- [x] 1.2 `stable_partition` the search-route results structural-first in query_graph.
- [x] 2.1 Regression in daemon_ops_test: a concept whose label sorts before the functions ranks
      last in search (route=search, total=3, concept last); exact query still routes to entity.
      Verified it fails (exit 93) with the partition disabled.
- [x] 2.2 Full ctest --preset default green; sanitizers via CI.
