# Debug vs Release, same tree, same machine

macOS arm64, Apple Silicon. Best of 5 runs each, `cgraph --root . --out DIR` on this
repository (171 files, 1432 nodes, 1870 edges).

| preset | `CMAKE_BUILD_TYPE` | pipeline best |
| --- | --- | --- |
| `default` | Debug (no `-O`) | 474 ms |
| `release` | Release | **137 ms** |

Speedup: **3.46x**.

`graph.json` is byte-identical between the two presets, so optimization is not a
correctness variable — verified by comparing key-sorted JSON dumps of both outputs.

Before this change `Debug` was the only build type in the repository
(`CMakePresets.json`), so every committed benchmark in `docs/` and every binary a
user built from the README was unoptimized. Existing comparisons against Python
Graphify therefore understate the native engine by roughly this factor.
