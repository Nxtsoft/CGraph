#!/usr/bin/env python3
"""Generate a synthetic Python project of a target node count for benchmarking
the graph.html viewer. Deterministic (index-seeded, no RNG) so runs are
reproducible. Files are grouped into packages to produce community structure,
and functions call/import across files to produce a realistic edge density.

Usage: gen_tree.py --nodes 10000 --out /tmp/bench-tree-10000
"""
import argparse
import os

FUNCS_PER_FILE = 4  # each file ~= 1 file node + FUNCS_PER_FILE function nodes


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nodes", type=int, required=True, help="approx target node count")
    ap.add_argument("--out", required=True)
    ap.add_argument("--packages", type=int, default=12, help="dirs -> communities")
    args = ap.parse_args()

    # nodes ~= files * (1 + FUNCS_PER_FILE); solve for file count.
    files = max(1, args.nodes // (1 + FUNCS_PER_FILE))
    per_pkg = max(1, files // args.packages)

    for i in range(files):
        pkg = i // per_pkg
        d = os.path.join(args.out, f"pkg{pkg:02d}")
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, f"mod{i:05d}.py")
        lines = []
        # import two neighbours in the same package (intra-community edges)
        for k in (1, 2):
            j = (i + k) % files
            if j // per_pkg == pkg:
                lines.append(f"from pkg{pkg:02d}.mod{j:05d} import fn{j:05d}_0")
        lines.append("")
        for f in range(FUNCS_PER_FILE):
            lines.append(f"def fn{i:05d}_{f}():")
            # call a couple of other functions to create CALLS edges
            t1 = (i + 1 + f) % files
            t2 = (i + 3) % files
            lines.append(f"    fn{t1:05d}_0()")
            lines.append(f"    fn{t2:05d}_{f % FUNCS_PER_FILE}()")
            lines.append("    return 0")
            lines.append("")
        with open(path, "w") as fh:
            fh.write("\n".join(lines))

    print(f"generated {files} files under {args.out} (~{files * (1 + FUNCS_PER_FILE)} nodes)")


if __name__ == "__main__":
    main()
