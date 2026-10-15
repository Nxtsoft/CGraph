"""Export the ContextBench Lite task inputs CGraph is allowed to see.

Reads the released parquet and writes one JSON line per instance with the
repository, base commit and issue text only. ``gold_context``, ``patch``,
``test_patch`` and the test lists are deliberately not exported, so retrieval
cannot read the labels. ``repo`` is the GitHub ``owner/name`` taken from
``repo_url``: the dataset's ``repo`` column is a project label that does not
always name the repository (``fasterxml/jackson`` covers three repositories).

Usage: instances.py PARQUET OUT.jsonl
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import pyarrow.parquet as pq

COLUMNS = ["instance_id", "original_inst_id", "repo_url", "base_commit", "problem_statement", "source", "language"]
GITHUB_RE = re.compile(r"^https://github\.com/([^/]+/[^/]+?)(?:\.git)?/?$")


def main(argv: list[str]) -> int:
    parquet, out = Path(argv[0]), Path(argv[1])
    rows = pq.read_table(parquet, columns=COLUMNS).to_pylist()
    with out.open("w", encoding="utf-8") as handle:
        for row in sorted(rows, key=lambda r: r["instance_id"]):
            match = GITHUB_RE.match(row["repo_url"] or "")
            if not match:
                raise SystemExit(f"{row['instance_id']}: not a GitHub repo_url: {row['repo_url']!r}")
            handle.write(json.dumps({
                "id": row["instance_id"],
                "original_inst_id": row["original_inst_id"],
                "repo": match.group(1),
                "base_commit": row["base_commit"],
                "source": row["source"],
                "language": row["language"],
                "problem_statement": row["problem_statement"],
            }, ensure_ascii=False) + "\n")
    print(f"{len(rows)} instances written to {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
